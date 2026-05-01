/*
 * ============================================================
 *  Air Quality Monitor Firmware
 *  Communication: GSM SIM7600E (PRIMARY) | WiFi (BACKUP)
 *  Sensor: SEN55 | Storage: SD Card | API: Heroku REST API
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SD.h>
#include <time.h>
#include <esp_task_wdt.h>
#include "SensirionI2CSen5x.h"
#include <string.h>

#define LOG_PATH_MAX 48
#define JSON_POST_BODY_MAX 768

/* ===================== CONFIG ===================== */

// --- WiFi (BACKUP) ---
#define WIFI_SSID     "project"
#define WIFI_PASSWORD ""

// --- GSM (PRIMARY) ---
#define GSM_RX_PIN    16
#define GSM_TX_PIN    17
#define GSM_APN       "internet"   // Change to "ethionet" for Ethio Telecom if needed

// --- Device Identity ---
#define DEVICE_ID     "5afd4455-93ab-4501-b12a-19feb37c607e"
#define API_KEY       "1NkPL7xXwa-JlPqPS_FmQDHH_qp3f-DOMpRhGX4kC04"
#define SERIAL_NUMBER "AQ-ET-AA-ETB-2026-01"

// --- API Endpoint ---
#define API_URL "https://air-q-9f333037f389.herokuapp.com/api/v1/sensor-readings/device"

// --- Timing ---
#define REALTIME_UPLOAD_INTERVAL  30000UL
#define WARMUP_TIME               180000UL
#define WIFI_CHECK_INTERVAL       10000UL

// --- SD ---
#define SD_CS             5
#define LOG_INDEX_FILE    "/log_index.txt"
#define CURRENT_FILE_META "/current_log.txt"

// --- NTP ---
#define NTP_SERVER            "pool.ntp.org"
#define NTP_SERVER_2          "time.google.com"
#define NTP_SERVER_3          "time.windows.com"

// --- Time Validation ---
#define MIN_VALID_EPOCH         1704067200UL
#define TIME_FUTURE_MARGIN      300
#define TIME_MONOTONIC_MARGIN   10
#define MAX_INVALID_TIME_RETRIES 5

// --- Misc ---
#define SCHEDULED_RESET_GRACE_MS  5000UL
#define WDT_TIMEOUT_MS            300000UL
#define MAX_NTP_SYNC_FAILURE_CYCLES 10
#define NTP_ATTEMPT_WINDOW_MS     60000UL

/* ===================== GSM CONSTANTS ===================== */
#define GSM_BAUD              115200
#define GSM_INIT_TIMEOUT_MS   60000UL   // Time to wait for GSM network registration
#define GSM_CMD_DELAY_MS      1000
#define GSM_HTTP_TIMEOUT_MS   15000UL   // Wait for +HTTPACTION reply
#define GSM_DOWNLOAD_TIMEOUT  5000UL
#define GSM_MAX_INIT_RETRIES  3         // Full GSM re-init attempts before fallback

/* ===================== STATE MACHINE ===================== */
enum SystemState {
  STATE_INITIALIZING,
  STATE_GSM_INIT,           // NEW: Initialize GSM first
  STATE_CONNECTING_WIFI,    // Fallback WiFi path
  STATE_SYNCING_TIME,
  STATE_OPERATIONAL
};

/* ===================== GLOBALS ===================== */

SensirionI2CSen5x sen5x;
HardwareSerial sim7600(2);  // UART2 for SIM7600E

unsigned long lastUploadTime    = 0;
unsigned long warmupStartTime   = 0;
unsigned long lastWiFiCheck     = 0;
unsigned long stateTimer        = 0;

static time_t lastValidTimestamp = 0;
static int    invalidTimeCount   = 0;
static bool   sensorReady        = false;

SystemState currentState = STATE_INITIALIZING;

// Log rotation state
static char currentLogFile[LOG_PATH_MAX] = "";
static char currentPtrFile[LOG_PATH_MAX] = "";
static int  lastRotYear   = -1;
static int  lastRotMon    = -1;
static int  lastRotMday   = -1;
static int  lastRotPeriod = -1;

static bool resetTriggeredThisMinute = false;

// GSM state tracking
static bool gsmReady          = false;   // Module registered on network & PDP active
static bool gsmHttpInitialized = false;  // AT+HTTPINIT done
static int  gsmInitRetries    = 0;

/* ===================== STRUCT ===================== */

struct SensorReading {
  float pm1_0;
  float pm2_5;
  float pm4_0;
  float pm10_0;
  float humidity;
  float temperature;
  float vocIndex;
  float noxIndex;
  bool  valid;
};

/* ===================== PROTOTYPES ===================== */

// Sensor
bool sensor_read(SensorReading& reading);
bool sensor_get_validated_timestamp(time_t& outEpoch, char* buf, size_t bufLen);
void sensor_attempt_ntp_resync();

// System
void system_check_scheduled_restart();
void updateLogFileFromRTC();
void system_init_watchdog();
void system_feed_watchdog();

// Sensor flow
void processRealtimeSensorFlow();

// NTP / WiFi helpers
static void applyConfigTime();
void feedNtpOnWifiConnectEdge();
void maintainWiFi();

// WiFi upload (BACKUP)
bool postReadingWiFi(JsonDocument& doc, int* outHttpCode = nullptr);

// GSM helpers & upload (PRIMARY)
void    gsm_send_at(const String& cmd, int delayMs = GSM_CMD_DELAY_MS);
bool    gsm_wait_response(const String& expected, unsigned long timeoutMs = 10000UL);
bool    gsm_init();
void    gsm_term_http();
bool    gsm_init_http();
bool    postReadingGSM(JsonDocument& doc);
bool    gsm_get_time_via_at(struct tm& timeinfo);

/* ===================== GSM LOW-LEVEL ===================== */

/**
 * Send an AT command and print echoed response to Serial monitor.
 * Non-blocking: just fires the command and waits `delayMs` then drains.
 */
void gsm_send_at(const String& cmd, int delayMs) {
  sim7600.println(cmd);
  Serial.println("[GSM >>] " + cmd);
  delay(delayMs);
  while (sim7600.available()) {
    Serial.write(sim7600.read());
  }
}

/**
 * Block until `expected` substring appears in SIM7600 output OR timeout.
 * Also drains to Serial for debugging.
 */
bool gsm_wait_response(const String& expected, unsigned long timeoutMs) {
  unsigned long start = millis();
  String response = "";
  while (millis() - start < timeoutMs) {
    system_feed_watchdog();
    while (sim7600.available()) {
      char c = sim7600.read();
      Serial.write(c);
      response += c;
    }
    if (response.indexOf(expected) != -1) return true;
  }
  return false;
}

/**
 * Full GSM initialization sequence (mirrored from working test code):
 *  1. Basic AT handshake
 *  2. Confirm SIM card
 *  3. Wait for network registration
 *  4. Activate PDP context with APN
 *  5. Initialize HTTP session
 * Returns true if all steps succeed.
 */
bool gsm_init() {
  Serial.println("[GSM] Starting initialization...");

  // Basic handshake & echo off
  gsm_send_at("AT", 1000);
  gsm_send_at("ATE0", 500);

  // SIM check
  sim7600.println("AT+CPIN?");
  Serial.println("[GSM >>] AT+CPIN?");
  if (!gsm_wait_response("READY", 5000)) {
    Serial.println("[GSM] SIM not ready (no READY response)");
    return false;
  }

  // Network registration - wait up to GSM_INIT_TIMEOUT_MS
  Serial.println("[GSM] Waiting for network registration...");
  unsigned long regStart = millis();
  bool registered = false;
  while (millis() - regStart < GSM_INIT_TIMEOUT_MS) {
    system_feed_watchdog();
    sim7600.println("AT+CREG?");
    Serial.println("[GSM >>] AT+CREG?");
    delay(1000);
    String resp = "";
    unsigned long t = millis();
    while (millis() - t < 1500) {
      while (sim7600.available()) { char c = sim7600.read(); Serial.write(c); resp += c; }
    }
    // +CREG: 0,1 or +CREG: 0,5 = registered
    if (resp.indexOf("+CREG: 0,1") != -1 || resp.indexOf("+CREG: 0,5") != -1 ||
        resp.indexOf("+CREG: 1")   != -1 || resp.indexOf("+CREG: 5")   != -1) {
      registered = true;
      Serial.println("[GSM] Network registered");
      break;
    }
    delay(2000);
  }
  if (!registered) {
    Serial.println("[GSM] Network registration timeout");
    return false;
  }

  // GPRS attach check
  gsm_send_at("AT+CGATT?", 1000);

  // Set APN and activate PDP
  gsm_send_at("AT+CGDCONT=1,\"IP\",\"" + String(GSM_APN) + "\"", 1000);
  sim7600.println("AT+CGACT=1,1");
  Serial.println("[GSM >>] AT+CGACT=1,1");
  if (!gsm_wait_response("OK", 10000)) {
    Serial.println("[GSM] PDP activation failed");
    return false;
  }

  // Initialize HTTP session
  if (!gsm_init_http()) {
    return false;
  }

  gsmReady = true;
  Serial.println("[GSM] Initialization complete. Module ready.");
  return true;
}

/**
 * Terminates any existing HTTP session cleanly.
 */
void gsm_term_http() {
  sim7600.println("AT+HTTPTERM");
  Serial.println("[GSM >>] AT+HTTPTERM");
  delay(500);
  while (sim7600.available()) Serial.write(sim7600.read());
  gsmHttpInitialized = false;
}

/**
 * Opens a persistent HTTP session pointed at the API endpoint.
 * Must be called after PDP activation.
 */
bool gsm_init_http() {
  gsm_term_http();  // Clean slate

  gsm_send_at("AT+HTTPINIT", 1000);
  gsm_send_at("AT+HTTPPARA=\"URL\",\"" + String(API_URL) + "\"", 1000);
  gsm_send_at("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 1000);

  // Set custom headers: Device-ID and API-Key
  // SIM7600 HTTPPARA supports USERDATA for extra headers
  String extraHeaders = "X-Device-ID: " + String(DEVICE_ID) + "\r\nX-API-Key: " + String(API_KEY);
  gsm_send_at("AT+HTTPPARA=\"USERDATA\",\"" + extraHeaders + "\"", 1000);

  gsmHttpInitialized = true;
  Serial.println("[GSM] HTTP session initialized");
  return true;
}

/**
 * POST a JSON document over GSM.
 * Re-initializes HTTP session if not already done.
 * Returns true on HTTP 201 response.
 */
bool postReadingGSM(JsonDocument& doc) {
  if (!gsmReady) {
    Serial.println("[GSM] Not ready - skipping GSM POST");
    return false;
  }

  // Ensure HTTP session is up
  if (!gsmHttpInitialized) {
    if (!gsm_init_http()) {
      Serial.println("[GSM] HTTP re-init failed");
      return false;
    }
  }

  // Serialize JSON payload
  char payload[JSON_POST_BODY_MAX];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0 || n >= sizeof(payload)) {
    Serial.println("[GSM] JSON serialization failed or too large");
    return false;
  }

  Serial.print("[GSM] Payload ("); Serial.print(n); Serial.println(" bytes):");
  Serial.println(payload);

  // Send data length request
  sim7600.print("AT+HTTPDATA=");
  sim7600.print(n);
  sim7600.println(",10000");
  Serial.print("[GSM >>] AT+HTTPDATA="); Serial.print(n); Serial.println(",10000");

  // Wait for DOWNLOAD prompt
  if (!gsm_wait_response("DOWNLOAD", GSM_DOWNLOAD_TIMEOUT)) {
    Serial.println("[GSM] No DOWNLOAD prompt - resetting HTTP session");
    gsmHttpInitialized = false;
    return false;
  }

  // Send the JSON payload
  sim7600.print(payload);
  if (!gsm_wait_response("OK", 5000)) {
    Serial.println("[GSM] Data send not acknowledged");
    gsmHttpInitialized = false;
    return false;
  }

  // Execute POST
  sim7600.println("AT+HTTPACTION=1");
  Serial.println("[GSM >>] AT+HTTPACTION=1");
  Serial.println("[GSM] Waiting for server response...");

  if (!gsm_wait_response("+HTTPACTION:", GSM_HTTP_TIMEOUT_MS)) {
    Serial.println("[GSM] POST timed out - no +HTTPACTION response");
    gsmHttpInitialized = false;
    return false;
  }

  // Drain any remaining response and look for status code 201
  String actionResp = "";
  unsigned long t = millis();
  while (millis() - t < 2000) {
    while (sim7600.available()) { char c = sim7600.read(); Serial.write(c); actionResp += c; }
  }

  // +HTTPACTION: 1,201,XX  -> success
  if (actionResp.indexOf(",201,") != -1 || actionResp.indexOf("201") != -1) {
    Serial.println("[GSM] POST successful (201)");
    return true;
  }

  // For cases where 201 was already consumed by gsm_wait_response buffer
  // (the wait captured it), treat any +HTTPACTION as sent and check for non-error codes
  // Common non-error: 200 (if server responds 200 instead of 201)
  if (actionResp.indexOf(",200,") != -1 || actionResp.indexOf("200") != -1) {
    Serial.println("[GSM] POST successful (200)");
    return true;
  }

  Serial.println("[GSM] POST failed - unexpected HTTP status");
  return false;
}

/**
 * Attempt to get time from GSM network clock (AT+CCLK?).
 * Populates a tm struct. Returns true on success.
 * Used as fallback when NTP is not reachable but GSM is up.
 */
bool gsm_get_time_via_at(struct tm& timeinfo) {
  sim7600.println("AT+CCLK?");
  Serial.println("[GSM >>] AT+CCLK?");
  String resp = "";
  unsigned long start = millis();
  while (millis() - start < 3000) {
    while (sim7600.available()) { char c = sim7600.read(); Serial.write(c); resp += c; }
  }

  // Response format: +CCLK: "YY/MM/DD,HH:MM:SS+QQ"
  int idx = resp.indexOf("+CCLK: \"");
  if (idx == -1) return false;

  // Parse: YY/MM/DD,HH:MM:SS plus or muinus ZZ
  int yy, mo, dd, hh, mm, ss;
  if (sscanf(resp.c_str() + idx + 8, "%d/%d/%d,%d:%d:%d", &yy, &mo, &dd, &hh, &mm, &ss) != 6) {
    return false;
  }

  memset(&timeinfo, 0, sizeof(timeinfo));
  timeinfo.tm_year = (yy + 2000) - 1900;
  timeinfo.tm_mon  = mo - 1;
  timeinfo.tm_mday = dd;
  timeinfo.tm_hour = hh;
  timeinfo.tm_min  = mm;
  timeinfo.tm_sec  = ss;

  // Apply UTC+3 (EAT) offset since CCLK is local time when network provides it
  // mktime + timegm trick: set system time from parsed struct
  time_t epoch = mktime(&timeinfo);
  if (epoch < (time_t)MIN_VALID_EPOCH) {
    Serial.println("[GSM] CCLK time invalid (too old)");
    return false;
  }

  struct timeval tv = { epoch, 0 };
  settimeofday(&tv, nullptr);
  setenv("TZ", "EAT-3", 1);
  tzset();

  Serial.printf("[GSM] Time from CCLK set: %04d-%02d-%02d %02d:%02d:%02d\n",
                yy + 2000, mo, dd, hh, mm, ss);
  return true;
}

/* ===================== SETUP ===================== */

void setup() {
  Serial.begin(115200);
  delay(100);

  // CRITICAL: Watchdog first
  system_init_watchdog();

  // SEN55 sensor
  Wire.begin(21, 22);
  sen5x.begin(Wire);
  sen5x.deviceReset();
  delay(200);
  sen5x.startMeasurement();
  warmupStartTime = millis();

  // SD card - reboot on failure
  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed! Rebooting in 5s...");
    delay(5000);
    esp_restart();
  } else {
    Serial.println("SD ready");
    updateLogFileFromRTC();
  }

  // GSM UART init
  sim7600.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  delay(3000);  // Allow SIM7600E to power-stabilize

  // Start with GSM init state
  currentState = STATE_GSM_INIT;
  stateTimer   = millis();

  Serial.println("[BOOT] Firmware started. GSM=PRIMARY, WiFi=BACKUP");
}

/* ===================== LOOP ===================== */

void loop() {
  system_feed_watchdog();

  feedNtpOnWifiConnectEdge();

  if (!sensorReady && millis() - warmupStartTime >= WARMUP_TIME) {
    sensorReady = true;
    Serial.println("SEN55 warmup complete - sensor ready");
  }

  system_check_scheduled_restart();

  // ---- State Machine ----
  switch (currentState) {

    /* ------ GSM INIT (PRIMARY path) ------ */
    case STATE_GSM_INIT: {
      Serial.println("[STATE] GSM_INIT - attempting GSM initialization");
      if (gsm_init()) {
        // GSM up - try to get time via CCLK or NTP over GSM
        struct tm tmGsm;
        if (gsm_get_time_via_at(tmGsm)) {
          Serial.println("[STATE] Time acquired from GSM network clock");
          updateLogFileFromRTC();
          currentState = STATE_OPERATIONAL;
        } else {
          // GSM up but no time from CCLK - try NTP over GSM data connection
          Serial.println("[STATE] CCLK unavailable - trying NTP over GSM...");
          applyConfigTime();
          unsigned long ntpWait = millis();
          bool ntpOk = false;
          while (millis() - ntpWait < 20000UL) {
            system_feed_watchdog();
            time_t now; time(&now);
            if (now > (time_t)MIN_VALID_EPOCH) { ntpOk = true; break; }
            delay(500);
          }
          if (ntpOk) {
            setenv("TZ", "EAT-3", 1); tzset();
            updateLogFileFromRTC();
            Serial.println("[STATE] NTP via GSM succeeded");
          } else {
            Serial.println("[STATE] NTP via GSM failed - operating with no time sync");
          }
          currentState = STATE_OPERATIONAL;
        }
      } else {
        gsmInitRetries++;
        Serial.printf("[STATE] GSM init failed (%d/%d)\n", gsmInitRetries, GSM_MAX_INIT_RETRIES);
        if (gsmInitRetries >= GSM_MAX_INIT_RETRIES) {
          Serial.println("[STATE] GSM unavailable - falling back to WiFi");
          gsmReady = false;
          // Start WiFi fallback
          WiFi.mode(WIFI_STA);
          WiFi.setSleep(false);
          WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
          currentState = STATE_CONNECTING_WIFI;
          stateTimer   = millis();
        } else {
          // Retry GSM after a short wait
          delay(5000);
        }
      }
      break;
    }

    /* ------ WiFi FALLBACK connect ------ */
    case STATE_CONNECTING_WIFI:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] Connected (backup path)");
        currentState = STATE_SYNCING_TIME;
        stateTimer   = millis();
      } else if (millis() - stateTimer > 30000) {
        Serial.println("[WiFi] Connection timeout - proceeding offline");
        currentState = STATE_OPERATIONAL;
      }
      break;

    /* ------ NTP sync (WiFi fallback path) ------ */
    case STATE_SYNCING_TIME: {
      static bool ntpStarted = false;
      static int  ntpSyncFailCycles = 0;

      maintainWiFi();

      if (!ntpStarted) {
        Serial.println("[NTP] Starting NTP sync...");
        applyConfigTime();
        ntpStarted = true;
        stateTimer = millis();
      }

      time_t now; time(&now);
      Serial.print("[NTP] epoch: "); Serial.println(now);

      if (now > (time_t)MIN_VALID_EPOCH) {
        Serial.println("[NTP] Time synced");
        setenv("TZ", "EAT-3", 1); tzset();
        Serial.println("[NTP] Timezone -> EAT (UTC+3)");
        updateLogFileFromRTC();
        ntpSyncFailCycles = 0;
        ntpStarted = false;
        currentState = STATE_OPERATIONAL;
      } else if (millis() - stateTimer > NTP_ATTEMPT_WINDOW_MS) {
        ntpSyncFailCycles++;
        Serial.printf("[NTP] Window expired (%d/%d)\n", ntpSyncFailCycles, MAX_NTP_SYNC_FAILURE_CYCLES);
        if (ntpSyncFailCycles >= MAX_NTP_SYNC_FAILURE_CYCLES) {
          Serial.println("[NTP] Max failures -> going operational without time");
          ntpStarted = false; ntpSyncFailCycles = 0;
          currentState = STATE_OPERATIONAL;
        } else {
          ntpStarted = false;
          stateTimer = millis();
        }
      }
      break;
    }

    /* ------ OPERATIONAL ------ */
    case STATE_OPERATIONAL:
      // Keep WiFi alive only if GSM is not ready (fallback mode)
      if (!gsmReady) {
        maintainWiFi();
      }
      if (millis() - lastUploadTime >= REALTIME_UPLOAD_INTERVAL) {
        lastUploadTime = millis();
        processRealtimeSensorFlow();
      }
      break;

    default:
      break;
  }

  delay(10);
}

/* ===================== SENSOR FLOW ===================== */

void processRealtimeSensorFlow() {
  SensorReading reading;
  if (!sensor_read(reading)) return;

  time_t recordedEpoch = 0;
  char   tsBuf[32];
  if (!sensor_get_validated_timestamp(recordedEpoch, tsBuf, sizeof(tsBuf))) return;

  StaticJsonDocument<512> doc;
  doc["pm2_5"]         = reading.pm2_5;
  doc["pm10"]          = reading.pm10_0;
  doc["humidity"]      = reading.humidity;
  doc["temperature"]   = reading.temperature;
  doc["voc_index"]     = reading.vocIndex;
  doc["nox_index"]     = reading.noxIndex;
  doc["recorded_at"]   = tsBuf;
  doc["device_id"]     = DEVICE_ID;
  doc["serial_number"] = SERIAL_NUMBER;

  /* ======== UPLOAD: GSM PRIMARY, WiFi BACKUP ======== */
  bool uploaded = false;

  // --- PRIMARY: GSM ---
  if (gsmReady) {
    Serial.println("[UPLOAD] Attempting GSM (primary)...");
    uploaded = postReadingGSM(doc);
    if (uploaded) {
      Serial.println("[UPLOAD] GSM upload OK");
    } else {
      Serial.println("[UPLOAD] GSM upload FAILED - will try WiFi backup");
      // Re-init HTTP session on next attempt; don't kill gsmReady
      gsmHttpInitialized = false;
    }
  }

  // --- BACKUP: WiFi (only if GSM failed or not ready) ---
  if (!uploaded) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[UPLOAD] Attempting WiFi (backup)...");
      int httpCode = 0;
      uploaded = postReadingWiFi(doc, &httpCode);
      if (uploaded) {
        Serial.println("[UPLOAD] WiFi upload OK");
      } else {
        Serial.printf("[UPLOAD] WiFi upload FAILED (HTTP %d)\n", httpCode);
      }
    } else {
      Serial.println("[UPLOAD] WiFi not connected - no backup available");
      // If GSM failed and WiFi not up, attempt to reconnect WiFi quietly
      if (!gsmReady) {
        if (WiFi.status() != WL_CONNECTED) {
          WiFi.reconnect();
        }
      }
    }
  }

  /* ======== SD BACKUP LOG ======== */
  updateLogFileFromRTC();

  if (currentLogFile[0] == '\0') {
    Serial.println("SD log skipped: no log path (time/rotation)");
    return;
  }

  File f = SD.open(currentLogFile, FILE_APPEND);
  if (f) {
    serializeJson(doc, f);
    f.println();
    f.close();
    Serial.println("Backup logged to SD");
  } else {
    Serial.println("SD log failed");
  }
}

/* ===================== LOG ROTATION ===================== */

void updateLogFileFromRTC() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("RTC not valid -> cannot rotate logs");
    return;
  }

  const int y             = timeinfo.tm_year + 1900;
  const int mon           = timeinfo.tm_mon + 1;
  const int mday          = timeinfo.tm_mday;
  const int rotationPeriod = timeinfo.tm_hour / 2;

  if (y == lastRotYear && mon == lastRotMon && mday == lastRotMday &&
      rotationPeriod == lastRotPeriod) {
    return;
  }

  lastRotYear   = y;
  lastRotMon    = mon;
  lastRotMday   = mday;
  lastRotPeriod = rotationPeriod;

  int startHour = rotationPeriod * 2;
  char fileName[LOG_PATH_MAX];
  char ptrName[LOG_PATH_MAX];

  snprintf(fileName, sizeof(fileName), "/log_%04d-%02d-%02d_%02d-%02d.txt",
           y, mon, mday, startHour, startHour + 2);
  snprintf(ptrName,  sizeof(ptrName),  "/ptr_%04d-%02d-%02d_%02d-%02d.txt",
           y, mon, mday, startHour, startHour + 2);

  strncpy(currentLogFile, fileName, sizeof(currentLogFile) - 1);
  currentLogFile[sizeof(currentLogFile) - 1] = '\0';
  strncpy(currentPtrFile, ptrName, sizeof(currentPtrFile) - 1);
  currentPtrFile[sizeof(currentPtrFile) - 1] = '\0';

  Serial.print("Rotated to new 2-hour log file: ");
  Serial.println(currentLogFile);

  File meta = SD.open(CURRENT_FILE_META, FILE_WRITE);
  if (meta) {
    meta.println(currentLogFile);
    meta.println(currentPtrFile);
    meta.close();
    Serial.println("Current file meta updated (2-hour rotation)");
  } else {
    Serial.println("Failed to update current file meta");
  }
}

/* ===================== WIFI UPLOAD (BACKUP) ===================== */

bool postReadingWiFi(JsonDocument& doc, int* outHttpCode) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);
  client.setTimeout(30000);

  HTTPClient https;
  https.setReuse(false);

  if (!https.begin(client, API_URL)) {
    if (outHttpCode) *outHttpCode = -2;
    return false;
  }

  doc["device_id"]     = DEVICE_ID;
  doc["serial_number"] = SERIAL_NUMBER;

  char payload[JSON_POST_BODY_MAX];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n >= sizeof(payload)) {
    https.end();
    client.stop();
    if (outHttpCode) *outHttpCode = -3;
    return false;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-Device-ID", DEVICE_ID);
  https.addHeader("X-API-Key",   API_KEY);

  int code = https.POST((uint8_t*)payload, strlen(payload));
  https.end();
  client.stop();

  if (outHttpCode) *outHttpCode = code;
  return (code == 201);
}

/* ===================== SENSOR HELPERS ===================== */

static bool recoverSEN55() {
  uint32_t status;
  uint16_t error = sen5x.readDeviceStatus(status);
  if (error) {
    char errMsg[256];
    errorToString(error, errMsg, 256);
    Serial.print("Status read error: "); Serial.println(errMsg);
    return false;
  }

  bool hasError = false;
  if (status & (1UL << 21)) { Serial.println("Fan speed error"); hasError = true; }
  if (status & (1UL << 5))  { Serial.println("Laser failure");   hasError = true; }
  if (status & (1UL << 4))  { Serial.println("Fan failure");     hasError = true; }
  if (status & (1UL << 7))  { Serial.println("Gas sensor error");hasError = true; }
  if (status & (1UL << 6))  { Serial.println("RHT error");       hasError = true; }
  if (!hasError) return true;

  static int retryCount = 0;
  if (retryCount < 3) {
    retryCount++;
    Serial.println("Attempting sensor reset...");
    error = sen5x.deviceReset();
    if (error) { Serial.println("Reset failed"); return false; }
    delay(100);
    error = sen5x.startMeasurement();
    if (error) { Serial.println("Restart measurement failed"); return false; }
    return true;
  } else {
    Serial.println("Max retries reached");
    retryCount = 0;
    return false;
  }
}

static bool waitForDataReadyAndRead(SensorReading& reading) {
  const unsigned long TIMEOUT_MS     = 5000UL;
  const unsigned long POLL_INTERVAL_MS = 100UL;
  unsigned long start = millis();

  bool     dataReady = false;
  uint16_t error;
  char     errMsg[256];

  while (millis() - start < TIMEOUT_MS) {
    system_feed_watchdog();
    error = sen5x.readDataReady(dataReady);
    if (error) {
      errorToString(error, errMsg, 256);
      Serial.print("DataReady error: "); Serial.println(errMsg);
      recoverSEN55();
      return false;
    }
    if (dataReady) {
      error = sen5x.readMeasuredValues(
        reading.pm1_0, reading.pm2_5, reading.pm4_0, reading.pm10_0,
        reading.humidity, reading.temperature, reading.vocIndex, reading.noxIndex
      );
      if (error) {
        errorToString(error, errMsg, 256);
        Serial.print("Read values error: "); Serial.println(errMsg);
        recoverSEN55();
        return false;
      }
      delay(100);
      return true;
    }
    delay(POLL_INTERVAL_MS);
  }
  Serial.println("DataReady timeout");
  recoverSEN55();
  return false;
}

static bool validateSensorData(SensorReading& reading) {
  if (isnan(reading.pm1_0) || isnan(reading.pm2_5) || isnan(reading.pm4_0) || isnan(reading.pm10_0) ||
      isnan(reading.humidity) || isnan(reading.temperature) || isnan(reading.vocIndex) || isnan(reading.noxIndex)) {
    Serial.println("Validation failed: NaN"); return false;
  }
  if (reading.pm1_0  < 0 || reading.pm1_0  > 1000 || reading.pm2_5  < 0 || reading.pm2_5  > 1000 ||
      reading.pm4_0  < 0 || reading.pm4_0  > 1000 || reading.pm10_0 < 0 || reading.pm10_0 > 1000) {
    Serial.println("Validation failed: PM range"); return false;
  }
  if (reading.humidity < 0 || reading.humidity > 100) {
    Serial.println("Validation failed: Humidity range"); return false;
  }
  if (reading.temperature < -10 || reading.temperature > 50) {
    Serial.println("Validation failed: Temperature range"); return false;
  }
  if (reading.vocIndex < 1 || reading.vocIndex > 500 || reading.noxIndex < 1 || reading.noxIndex > 500) {
    Serial.println("Validation failed: VOC/NOx range"); return false;
  }
  return true;
}

bool sensor_read(SensorReading& reading) {
  if (!waitForDataReadyAndRead(reading)) return false;
  reading.valid = validateSensorData(reading);
  if (!reading.valid) { Serial.println("Invalid data - skipping"); return false; }
  return true;
}

bool sensor_get_validated_timestamp(time_t& outEpoch, char* buf, size_t bufLen) {
  if (!buf || bufLen < 26) { outEpoch = 0; return false; }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("getLocalTime failed");
    outEpoch = 0; buf[0] = '\0'; return false;
  }

  time_t now; time(&now);

  if (now < (time_t)MIN_VALID_EPOCH) {
    Serial.println("Invalid: epoch too old");
    outEpoch = 0; buf[0] = '\0'; return false;
  }
  if (lastValidTimestamp > 0 && now > lastValidTimestamp + (time_t)TIME_FUTURE_MARGIN) {
    Serial.println("Invalid: large forward time jump");
    outEpoch = 0; buf[0] = '\0'; return false;
  }
  if (lastValidTimestamp > 0 && now < lastValidTimestamp - TIME_MONOTONIC_MARGIN) {
    Serial.println("Invalid: back-dated");
    outEpoch = 0; buf[0] = '\0'; return false;
  }
  if (strftime(buf, bufLen, "%Y-%m-%dT%H:%M:%S+03:00", &timeinfo) == 0) {
    outEpoch = 0; buf[0] = '\0'; return false;
  }

  outEpoch = now;
  lastValidTimestamp = now;
  return true;
}

void sensor_attempt_ntp_resync() {
  Serial.println("Attempting NTP resync...");
  applyConfigTime();
  time_t now;
  int attempts = 0;
  while (attempts < 10) {
    system_feed_watchdog();
    time(&now);
    if (now > (time_t)MIN_VALID_EPOCH) { Serial.println("Resync success"); return; }
    delay(500);
    attempts++;
  }
  Serial.println("Resync failed");
}

/* ===================== NTP / WIFI HELPERS ===================== */

static void applyConfigTime() {
  configTime(0, 0, NTP_SERVER, NTP_SERVER_2, NTP_SERVER_3);
}

void feedNtpOnWifiConnectEdge() {
  static wl_status_t prev = (wl_status_t)254;
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED && prev != WL_CONNECTED) {
    Serial.println("[WiFi] Link up - SNTP (re)sync");
    applyConfigTime();
  }
  prev = st;
}

void maintainWiFi() {
  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL) return;
  lastWiFiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost. Reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

/* ===================== SCHEDULED RESTART ===================== */

void system_check_scheduled_restart() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("NTP time invalid - skipping scheduled reset check");
    return;
  }
  if ((timeinfo.tm_hour == 0 || timeinfo.tm_hour == 12) && timeinfo.tm_min == 0 && !resetTriggeredThisMinute) {
    Serial.printf("Scheduled restart triggered at %d:00 EAT\n", timeinfo.tm_hour);
    delay(1000);
    resetTriggeredThisMinute = true;
    esp_restart();
  } else if (timeinfo.tm_min != 0) {
    resetTriggeredThisMinute = false;
  }
}

/* ===================== WATCHDOG ===================== */

void system_init_watchdog() {
  esp_task_wdt_deinit();

  esp_task_wdt_config_t twdt_config = {
    .timeout_ms     = WDT_TIMEOUT_MS,
    .idle_core_mask = (1 << 0) | (1 << 1),
    .trigger_panic  = true
  };

  esp_err_t err = esp_task_wdt_init(&twdt_config);
  if (err != ESP_OK) {
    Serial.printf("Watchdog init failed: %s\n", esp_err_to_name(err));
    return;
  }

  err = esp_task_wdt_add(NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed to subscribe to watchdog: %s\n", esp_err_to_name(err));
    return;
  }

  Serial.printf("Watchdog enabled - will reset after %lu seconds of hang\n", WDT_TIMEOUT_MS / 1000);
}

void system_feed_watchdog() {
  esp_task_wdt_reset();
}
