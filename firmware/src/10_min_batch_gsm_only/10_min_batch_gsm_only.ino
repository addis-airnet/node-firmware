/*
 * ============================================================
 *  Air Quality Monitor – Addis Ababa
 *  Firmware  : GSM-only (SIM7600E)
 *              Read every 1 min → log to SD
 *              Batch-upload 60 readings every 60 min
 *  Board     : ESP32
 *  Sensor    : Sensirion SEN55
 *  Comms     : SIM7600E  UART2  RX=16  TX=17
 *  Storage   : SD card   SPI    CS=5
 *  Time      : GSM AT+CNTP → pool.ntp.org / time.google.com /
 *              time.windows.com  →  EAT (UTC+3, +03:00)
 *
 *  AT command pattern taken verbatim from the validated
 *  SIM7600E test sketch that confirmed a working HTTPS POST.
 * ============================================================
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SD.h>
#include <time.h>
#include <esp_task_wdt.h>
#include "SensirionI2CSen5x.h"

/* ===================== CONFIG ===================== */

#define DEVICE_ID     "5afd4455-93ab-4501-b12a-19feb37c607e"
#define API_KEY       "AIzaSyAWCvZuaKVbtPYlu83KDlz1QkLDDY0pj-E"
#define SERIAL_NUMBER "AQ-ET-AA-ETB-2026-01"

#define API_URL  "https://sen55-air-quality-monitor-default-rtdb.europe-west1.firebasedatabase.app/sensor/data.json"

// GSM
#define GSM_RX_PIN  16
#define GSM_TX_PIN  17
#define GSM_BAUD    115200
#define APN         "internet"   

// SD
#define SD_CS  5

// Timing
#define SENSOR_READ_INTERVAL  60000UL      // 1 min
#define UPLOAD_INTERVAL       600000UL   // 10 min
#define WARMUP_TIME           180000UL     // 3 min SEN55 thermal warm-up

// Batch
#define MAX_LINES_PER_UPLOAD  10

// SD meta (survives reboots)
#define CURRENT_FILE_META  "/current_log.txt"

// Time validation (same thresholds as original)
#define MIN_VALID_EPOCH        1704067200UL   // 2024-01-01 00:00:00 UTC
#define TIME_FUTURE_MARGIN     300
#define TIME_MONOTONIC_MARGIN  10
#define MAX_INVALID_TIME_RETRIES  5

// Watchdog
#define WDT_TIMEOUT_MS  300000UL   // 5 min

/* ===================== GLOBALS ===================== */

SensirionI2CSen5x sen5x;
HardwareSerial    sim7600(2);          // UART2

unsigned long lastSensorRead  = 0;
unsigned long lastUploadTime  = 0;
unsigned long warmupStartTime = 0;

static time_t lastValidTimestamp = 0;
static int    invalidTimeCount   = 0;
static bool   sensorReady        = false;

// Log-rotation state – exact mirror of original (hourly files)
static String currentLogFile = "";
static String currentPtrFile = "";
static int    lastLoggedHour = -1;
static int    lastLoggedDay  = -1;

static bool resetTriggeredThisMinute = false;

/* ===================== STRUCT ===================== */

struct SensorReading {
  float pm1_0, pm2_5, pm4_0, pm10_0;
  float humidity, temperature;
  float vocIndex, noxIndex;
  bool  valid;
};

/* ===================== PROTOTYPES ===================== */

// Watchdog
void  wdt_init();
void  wdt_feed();

// GSM AT helpers  (signature matches the working test sketch)
void  gsm_sendAT(String cmd, int delayMs = 1000);
bool  gsm_waitResponse(String expected, unsigned long timeout = 10000);

// GSM network + time
bool  gsm_initModem();
bool  gsm_ntpSync(const char* ntpServer);
bool  gsm_syncTime();

// GSM HTTP
void  gsm_httpInit();
void  gsm_httpTerm();
bool  gsm_postReading(const String& payload);

// Sensor  (unchanged from original)
bool   sensor_read(SensorReading& reading);
String sensor_get_validated_timestamp(time_t& outEpoch);
void   sensor_attempt_ntp_resync();

// SD / rotation  (unchanged from original)
void updateLogFileFromRTC();
void loadCurrentLogFileFromMeta();

// Upload
void uploadFromSD();

// System
void system_check_scheduled_restart();

/* ============================================================
 *  WATCHDOG
 * ============================================================ */

void wdt_init() {
  esp_task_wdt_deinit();
  esp_task_wdt_config_t cfg = {
    .timeout_ms     = WDT_TIMEOUT_MS,
    .idle_core_mask = (1 << 0) | (1 << 1),
    .trigger_panic  = true
  };
  if (esp_task_wdt_init(&cfg) != ESP_OK) {
    Serial.println("WDT init failed");
    return;
  }
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    Serial.println("WDT subscribe failed");
    return;
  }
  Serial.printf("WDT armed (%lu s)\n", WDT_TIMEOUT_MS / 1000UL);
}

void wdt_feed() {
  esp_task_wdt_reset();
}

/* ============================================================
 *  GSM – AT helpers
 *  Signatures and body taken directly from the working test sketch.
 * ============================================================ */

void gsm_sendAT(String cmd, int delayMs) {
  sim7600.println(cmd);
  Serial.println(">> " + cmd);
  delay(delayMs);
  while (sim7600.available()) {
    Serial.write(sim7600.read());
  }
}

bool gsm_waitResponse(String expected, unsigned long timeout) {
  unsigned long start = millis();
  String response = "";
  while (millis() - start < timeout) {
    wdt_feed();
    while (sim7600.available()) {
      char c = sim7600.read();
      Serial.write(c);
      response += c;
      // Prevent runaway buffer growth from URC spam
      if (response.length() > 2048) {
        response = response.substring(response.length() - 1024);
      }
    }
    if (response.indexOf(expected) != -1) return true;
  }
  return false;
}

/* ============================================================
 *  GSM – Modem + data bearer init
 *  Same command sequence as the working test sketch setup().
 * ============================================================ */

bool gsm_initModem() {
  Serial.println("GSM: initialising modem...");

  gsm_sendAT("AT");
  gsm_sendAT("ATE0");           // echo off – makes response parsing reliable
  gsm_sendAT("AT+CPIN?");
  gsm_sendAT("AT+CREG?");
  gsm_sendAT("AT+CGATT?");
  gsm_sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"");
  gsm_sendAT("AT+CGACT=1,1", 4000);

  // Verify an IP was assigned – required before any HTTP or NTP
  sim7600.println("AT+CGPADDR=1");
  Serial.println(">> AT+CGPADDR=1");
  if (!gsm_waitResponse("+CGPADDR:", 8000)) {
    Serial.println("GSM: no IP address – init failed");
    return false;
  }
  Serial.println("GSM: data bearer up");
  return true;
}

/* ============================================================
 *  GSM – NTP sync (one server attempt)
 *
 *  AT+SAPBR  opens bearer for NTP service
 *  AT+CNTP   configures NTP server and triggers sync
 *  AT+CCLK?  reads the updated RTC string
 *  The parsed UTC epoch is pushed into ESP32 RTC via settimeofday().
 *  TZ is set to EAT-3 (UTC+3) so getLocalTime() always returns
 *  Addis Ababa local time and timestamps carry +03:00.
 * ============================================================ */

bool gsm_ntpSync(const char* ntpServer) {
  Serial.printf("GSM NTP: trying server %s ...\n", ntpServer);

  // SAPBR bearer (context 1 already has APN from CGDCONT)
  gsm_sendAT("AT+SAPBR=3,1,\"Contype\",\"GPRS\"", 500);
  gsm_sendAT("AT+SAPBR=3,1,\"APN\",\"" + String(APN) + "\"", 500);
  gsm_sendAT("AT+SAPBR=1,1", 5000);

  // Configure NTP: server, UTC offset=0, bearer 1, auto-update modem RTC
  String ntpCmd = "AT+CNTP=\"";
  ntpCmd += ntpServer;
  ntpCmd += "\",0,1,2";
  gsm_sendAT(ntpCmd, 1000);

  // Trigger – module replies +CNTP: 1 when done
  sim7600.println("AT+CNTP");
  Serial.println(">> AT+CNTP");
  if (!gsm_waitResponse("+CNTP:", 15000)) {
    Serial.println("GSM NTP: no +CNTP response");
    return false;
  }

  // Read modem clock
  sim7600.println("AT+CCLK?");
  Serial.println(">> AT+CCLK?");
  delay(500);

  String cclk = "";
  unsigned long t0 = millis();
  while (millis() - t0 < 5000) {
    wdt_feed();
    while (sim7600.available()) cclk += (char)sim7600.read();
    if (cclk.indexOf("+CCLK:") != -1) break;
    delay(50);
  }
  Serial.println("CCLK raw: " + cclk);

  // Parse:  +CCLK: "YY/MM/DD,HH:MM:SS+TZ"
  int q1 = cclk.indexOf('"');
  int q2 = cclk.lastIndexOf('"');
  if (q1 < 0 || q2 <= q1 + 1) {
    Serial.println("GSM NTP: CCLK parse error");
    return false;
  }
  String dt = cclk.substring(q1 + 1, q2);
  if (dt.length() < 17) {
    Serial.println("GSM NTP: CCLK string too short");
    return false;
  }

  int yy = dt.substring(0,  2).toInt() + 2000;
  int mo = dt.substring(3,  5).toInt();
  int dd = dt.substring(6,  8).toInt();
  int hh = dt.substring(9,  11).toInt();
  int mm = dt.substring(12, 14).toInt();
  int ss = dt.substring(15, 17).toInt();

  // Force TZ=UTC0 so mktime() treats the parsed values as UTC
  setenv("TZ", "UTC0", 1);
  tzset();

  struct tm tmv = {};
  tmv.tm_year = yy - 1900;
  tmv.tm_mon  = mo  - 1;
  tmv.tm_mday = dd;
  tmv.tm_hour = hh;
  tmv.tm_min  = mm;
  tmv.tm_sec  = ss;

  time_t epoch = mktime(&tmv);
  if (epoch < (time_t)MIN_VALID_EPOCH) {
    Serial.printf("GSM NTP: epoch %lu out of range\n", (unsigned long)epoch);
    return false;
  }

  // Set ESP32 RTC
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  // Apply East Africa Time for all getLocalTime() calls
  setenv("TZ", "EAT-3", 1);
  tzset();

  Serial.printf("GSM NTP: synced OK – UTC epoch %lu, EAT applied\n",
                (unsigned long)epoch);
  return true;
}

/* gsm_syncTime – tries three NTP servers in order */
bool gsm_syncTime() {
  const char* servers[] = {
    "pool.ntp.org",
    "time.google.com",
    "time.windows.com"
  };
  for (int i = 0; i < 3; i++) {
    wdt_feed();
    if (gsm_ntpSync(servers[i])) return true;
    Serial.printf("GSM NTP: server %s failed, trying next...\n", servers[i]);
    delay(2000);
  }
  Serial.println("GSM NTP: all three servers failed");
  return false;
}

/* ============================================================
 *  GSM – HTTP session management
 *
 *  One session is opened at the start of every upload batch
 *  and closed at the end, so the TLS handshake is paid once
 *  per hour instead of 60 times.
 *  Pattern: HTTPINIT → HTTPPARA (set once) → loop HTTPDATA/
 *  HTTPACTION → HTTPTERM (same as working test sketch).
 * ============================================================ */

void gsm_httpInit() {
  gsm_sendAT("AT+HTTPTERM", 500);    // clear any stale session
  gsm_sendAT("AT+HTTPINIT");
  gsm_sendAT("AT+HTTPPARA=\"URL\",\"" + String(API_URL) + "\"");
  gsm_sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"");
  // Auth headers injected via USERDATA
  String hdr = "AT+HTTPPARA=\"USERDATA\",\"X-Device-ID: ";
  hdr += DEVICE_ID;
  hdr += "\\r\\nX-API-Key: ";
  hdr += API_KEY;
  hdr += "\"";
  gsm_sendAT(hdr);
  Serial.println("GSM: HTTP session open");
}

void gsm_httpTerm() {
  gsm_sendAT("AT+HTTPTERM", 500);
  Serial.println("GSM: HTTP session closed");
}

/* ============================================================
 *  GSM – POST one JSON string
 *  Body taken directly from the working test sketch loop().
 * ============================================================ */

bool gsm_postReading(const String& payload) {
  // Step 1 – tell modem how many bytes to expect; it replies "DOWNLOAD"
  sim7600.print("AT+HTTPDATA=");
  sim7600.print(payload.length());
  sim7600.println(",10000");
  Serial.println(">> AT+HTTPDATA=" + String(payload.length()) + ",10000");

  if (!gsm_waitResponse("DOWNLOAD", 5000)) {
    Serial.println("GSM POST: no DOWNLOAD prompt");
    return false;
  }

  // Step 2 – send JSON payload (raw bytes, no println)
  sim7600.print(payload);
  Serial.println(">> [payload sent]");

  if (!gsm_waitResponse("OK", 5000)) {
    Serial.println("GSM POST: payload ACK missing");
    return false;
  }

  // Step 3 – execute POST
  sim7600.println("AT+HTTPACTION=1");
  Serial.println(">> AT+HTTPACTION=1");
  Serial.println("[Waiting for server response...]");

  // ── FIX: do NOT use gsm_waitResponse here. ──────────────────────────────
  // gsm_waitResponse() drains the UART into its own local buffer and returns
  // only a bool, so "+HTTPACTION: 1,200,31" would be consumed and lost.
  // Instead, accumulate everything in actionLine until we see "+HTTPACTION:"
  // and then linger 1 s more to capture the status code and length fields.
  String actionLine = "";
  bool   found      = false;
  unsigned long t0  = millis();
  while (millis() - t0 < 25000) {
    wdt_feed();
    while (sim7600.available()) {
      char c = sim7600.read();
      Serial.write(c);
      actionLine += c;
      if (actionLine.length() > 2048)
        actionLine = actionLine.substring(actionLine.length() - 1024);
    }
    if (actionLine.indexOf("+HTTPACTION:") != -1) {
      found = true;
      // Linger to grab the rest of the line (", 1,200,31\r\n")
      unsigned long t1 = millis();
      while (millis() - t1 < 1500) {
        wdt_feed();
        while (sim7600.available()) {
          char c = sim7600.read();
          Serial.write(c);
          actionLine += c;
        }
        delay(20);
      }
      break;
    }
  }

  if (!found) {
    Serial.println("GSM POST: no +HTTPACTION response (timeout)");
    return false;
  }

  Serial.println("HTTPACTION line: " + actionLine);
  bool ok = (actionLine.indexOf(",200,") != -1 || actionLine.indexOf(",201,") != -1);
  if (!ok) Serial.println("GSM POST: non-2xx response");
  return ok;
}

/* ============================================================
 *  LOG ROTATION  (unchanged from original source)
 *  Hourly files:  /log_YYYY-MM-DD_HH.txt
 *  Pointer files: /ptr_YYYY-MM-DD_HH.txt
 *  Active paths persisted in /current_log.txt for reboot recovery.
 * ============================================================ */

void updateLogFileFromRTC() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("RTC not valid → cannot rotate logs");
    return;
  }

  int hour = timeinfo.tm_hour;
  int day  = timeinfo.tm_mday;

  if (hour == lastLoggedHour && day == lastLoggedDay) return;

  lastLoggedHour = hour;
  lastLoggedDay  = day;

  char fileName[32];
  char ptrName[32];
  strftime(fileName, sizeof(fileName), "/log_%Y-%m-%d_%H.txt", &timeinfo);
  strftime(ptrName,  sizeof(ptrName),  "/ptr_%Y-%m-%d_%H.txt", &timeinfo);

  currentLogFile = String(fileName);
  currentPtrFile = String(ptrName);

  Serial.print("RTC Log file updated: ");
  Serial.println(currentLogFile);

  File meta = SD.open(CURRENT_FILE_META, FILE_WRITE);
  if (meta) {
    meta.println(currentLogFile);
    meta.println(currentPtrFile);
    meta.close();
    Serial.println("Current file meta updated");
  } else {
    Serial.println("Failed to update current file meta");
  }
}

void loadCurrentLogFileFromMeta() {
  if (!SD.exists(CURRENT_FILE_META)) {
    Serial.println("Meta file not found, will create on first rotation");
    return;
  }

  File meta = SD.open(CURRENT_FILE_META, FILE_READ);
  if (!meta) return;

  currentLogFile = meta.readStringUntil('\n');
  currentLogFile.trim();
  currentPtrFile = meta.readStringUntil('\n');
  currentPtrFile.trim();
  meta.close();

  Serial.print("Recovered active log file: ");
  Serial.println(currentLogFile);
  Serial.print("Recovered active ptr file: ");
  Serial.println(currentPtrFile);
}

/* ============================================================
 *  SENSOR  (unchanged from original source)
 * ============================================================ */

static bool recoverSEN55() {
  uint32_t status;
  uint16_t error = sen5x.readDeviceStatus(status);
  if (error) {
    char errMsg[256];
    errorToString(error, errMsg, 256);
    Serial.print("Status read error: ");
    Serial.println(errMsg);
    return false;
  }

  bool hasError = false;
  if (status & (1UL << 21)) { Serial.println("Fan speed error");  hasError = true; }
  if (status & (1UL <<  5)) { Serial.println("Laser failure");    hasError = true; }
  if (status & (1UL <<  4)) { Serial.println("Fan failure");      hasError = true; }
  if (status & (1UL <<  7)) { Serial.println("Gas sensor error"); hasError = true; }
  if (status & (1UL <<  6)) { Serial.println("RHT error");        hasError = true; }

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
  }
  Serial.println("Max retries reached");
  retryCount = 0;
  return false;
}

static bool waitForDataReadyAndRead(SensorReading& reading) {
  const unsigned long TIMEOUT_MS       = 5000UL;
  const unsigned long POLL_INTERVAL_MS = 100UL;
  unsigned long start = millis();
  bool dataReady = false;
  uint16_t error;
  char errMsg[256];

  while (millis() - start < TIMEOUT_MS) {
    wdt_feed();
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
  if (isnan(reading.pm1_0)  || isnan(reading.pm2_5) ||
      isnan(reading.pm4_0)  || isnan(reading.pm10_0) ||
      isnan(reading.humidity) || isnan(reading.temperature) ||
      isnan(reading.vocIndex) || isnan(reading.noxIndex)) {
    Serial.println("Validation failed: NaN"); return false;
  }
  if (reading.pm1_0 < 0 || reading.pm1_0 > 1000 || reading.pm2_5 < 0 || reading.pm2_5 > 1000 ||
      reading.pm4_0 < 0 || reading.pm4_0 > 1000 || reading.pm10_0 < 0 || reading.pm10_0 > 1000) {
    Serial.println("Validation failed: PM range"); return false;
  }
  if (reading.humidity    <   0 || reading.humidity    > 100) {
    Serial.println("Validation failed: Humidity range"); return false;
  }
  if (reading.temperature < -10 || reading.temperature >  50) {
    Serial.println("Validation failed: Temperature range"); return false;
  }
  if (reading.vocIndex < 1 || reading.vocIndex > 500 ||
      reading.noxIndex < 1 || reading.noxIndex > 500) {
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

/* ============================================================
 *  TIMESTAMP  (unchanged from original source)
 *  Returns ISO-8601 with +03:00 EAT offset, or "" on failure.
 * ============================================================ */

String sensor_get_validated_timestamp(time_t& outEpoch) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("getLocalTime failed");
    outEpoch = 0;
    return "";
  }

  time_t now;
  time(&now);

  if (now < (time_t)MIN_VALID_EPOCH) {
    Serial.println("Invalid: epoch too old");
    outEpoch = 0; return "";
  }
  if (now > time(nullptr) + TIME_FUTURE_MARGIN) {
    Serial.println("Invalid: future dated");
    outEpoch = 0; return "";
  }
  if (lastValidTimestamp > 0 && now < lastValidTimestamp - TIME_MONOTONIC_MARGIN) {
    Serial.println("Invalid: back-dated");
    outEpoch = 0; return "";
  }

  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+03:00", &timeinfo);
  lastValidTimestamp = now;
  outEpoch = now;
  return String(buf);
}

void sensor_attempt_ntp_resync() {
  Serial.println("Attempting NTP resync via GSM...");
  if (gsm_syncTime()) {
    Serial.println("NTP resync success");
  } else {
    Serial.println("NTP resync failed");
  }
}

/* ============================================================
 *  SCHEDULED RESTART  (unchanged from original source)
 *  Fires at 00:00 and 12:00 EAT.
 * ============================================================ */

void system_check_scheduled_restart() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("NTP time invalid - skipping scheduled reset check");
    return;
  }
  if ((timeinfo.tm_hour == 0 || timeinfo.tm_hour == 12) &&
      timeinfo.tm_min == 0 && !resetTriggeredThisMinute) {
    Serial.println("Scheduled restart triggered at " +
                   String(timeinfo.tm_hour) + ":00 EAT");
    delay(1000);
    resetTriggeredThisMinute = true;
    esp_restart();
  } else if (timeinfo.tm_min != 0) {
    resetTriggeredThisMinute = false;
  }
}

/* ============================================================
 *  UPLOAD  –  batch-upload the 10 most recent unuploaded SD lines
 *
 *  Two-phase design
 *  ────────────────
 *  PHASE 1  SCAN  (read-only, no SD writes)
 *    Single forward pass from the saved pointer offset.
 *    A rolling circular buffer of MAX_LINES_PER_UPLOAD byte-positions
 *    is maintained as each non-blank line is encountered.
 *    After the scan the buffer holds exactly the byte-positions of the
 *    last N line-starts (or all of them if fewer than N exist).
 *    No SD write happens here – safe to lose power at any point.
 *
 *  PHASE 2  UPLOAD  (one SD write per confirmed POST)
 *    If more than N lines were found, the skip-advance pointer is
 *    written to SD ONCE before the HTTP session opens.  This is the
 *    only "commitment" that abandons older unuploaded lines.
 *    After each confirmed 200/201 the pointer is written immediately
 *    so the next cycle never re-uploads a line that already succeeded.
 *
 *  Power-loss guarantees
 *  ─────────────────────
 *  • Power dies during Phase 1
 *      → restart re-scans from scratch.  Identical result. ✓
 *  • Power dies after skip-pointer write, before first POST
 *      → restart re-scans, finds same 10 lines, re-attempts upload. ✓
 *  • Power dies after POST but before pointer update (worst case)
 *      → one reading re-POSTed on restart (at-least-once delivery).
 *         Server deduplicates by recorded_at + device_id. ✓
 *  • Power dies after pointer update
 *      → perfectly clean state. ✓
 *  • Pointer file corrupted / zero-length on restart
 *      → parseInt() returns 0, scan begins from file start.
 *         At most N readings re-posted. ✓
 *
 *  Pointer semantics
 *  ─────────────────
 *  The stored value is the byte offset of the first byte AFTER the
 *  last successfully uploaded (or intentionally skipped) line.
 *  It only ever moves forward.  It is written to a dedicated ptr file
 *  so the log file itself is never modified.
 * ============================================================ */

// Helper – write a size_t offset to the pointer file atomically.
// Returns true on success.
static bool writePointer(const String& ptrName, size_t value) {
  File pw = SD.open(ptrName, FILE_WRITE);
  if (!pw) return false;
  pw.print((long)value);
  pw.close();
  return true;
}

void uploadFromSD() {
  // ── Guards: paths must be non-empty ──────────────────────
  if (currentLogFile.length() == 0 || currentPtrFile.length() == 0) {
    Serial.println("Upload skipped: log/ptr path not set");
    return;
  }
  if (!SD.exists(CURRENT_FILE_META)) {
    Serial.println("No meta file → nothing to upload");
    return;
  }

  // Re-read meta fresh (guards against rotation since last write)
  File metaCheck = SD.open(CURRENT_FILE_META, FILE_READ);
  if (!metaCheck) { Serial.println("Failed to open meta file"); return; }
  String logName = metaCheck.readStringUntil('\n'); logName.trim();
  String ptrName = metaCheck.readStringUntil('\n'); ptrName.trim();
  metaCheck.close();

  if (logName.length() == 0 || ptrName.length() == 0) {
    Serial.println("Meta file invalid"); return;
  }
  if (!SD.exists(logName)) {
    Serial.println("Log file does not exist yet: " + logName); return;
  }

  File logFile = SD.open(logName, FILE_READ);
  if (!logFile) { Serial.println("Cannot open log file: " + logName); return; }

  size_t fileSize = logFile.size();
  if (fileSize == 0) {
    Serial.println("Log file is empty"); logFile.close(); return;
  }

  // ── Read and validate saved pointer ──────────────────────
  size_t offset = 0;
  if (SD.exists(ptrName)) {
    File pf = SD.open(ptrName, FILE_READ);
    if (pf) {
      long parsed = pf.parseInt();
      pf.close();
      if (parsed > 0 && (size_t)parsed <= fileSize) {
        offset = (size_t)parsed;
      } else if (parsed < 0 || (size_t)parsed > fileSize) {
        Serial.printf("Ptr %ld out of bounds (fileSize=%u) → reset to 0\n",
                      parsed, (unsigned)fileSize);
        offset = 0;
      }
    }
  }

  if (!logFile.seek(offset)) {
    Serial.printf("seek(%u) failed → reset to 0\n", (unsigned)offset);
    offset = 0; logFile.seek(0);
  }

  // ═══════════════════════════════════════════════════════
  //  PHASE 1 – SCAN  (read-only, no SD writes)
  //
  //  Walk every non-blank line from offset to EOF.
  //  Keep a rolling circular buffer of the byte-position at the
  //  START of each line.  After the scan:
  //    lineCount  = total non-blank lines found
  //    lineBuf[]  = positions of the LAST min(lineCount,N) lines
  //    oldest-of-last-N is at index  lineCount % N
  // ═══════════════════════════════════════════════════════
  const int   N = MAX_LINES_PER_UPLOAD;
  size_t lineBuf[N];        // circular: index = lineCount % N
  int    lineCount = 0;

  {
    size_t curPos = logFile.position();   // = offset after seek
    while (logFile.available()) {
      wdt_feed();
      // Record start-of-line position BEFORE reading it
      lineBuf[lineCount % N] = curPos;
      String l = logFile.readStringUntil('\n');
      l.trim();
      if (l.length() > 0) lineCount++;
      curPos = logFile.position();
    }
  }

  Serial.printf("Scan: %d unuploaded line(s) in %s from offset %u\n",
                lineCount, logName.c_str(), (unsigned)offset);

  if (lineCount == 0) {
    Serial.println("Nothing new to upload");
    logFile.close(); return;
  }

  // ═══════════════════════════════════════════════════════
  //  PHASE 2 – UPLOAD
  //
  //  Determine where to start:
  //    • ≤ N lines unread → start from current offset (upload all)
  //    • > N lines unread → oldest-of-last-N is the upload start.
  //      Write skip-advance pointer ONCE before opening HTTP so
  //      the commitment to skip survives power loss.
  // ═══════════════════════════════════════════════════════
  size_t uploadStart = offset;

  if (lineCount > N) {
    // oldest of the last-N lines is at circular index lineCount % N
    uploadStart = lineBuf[lineCount % N];
    int skipped = lineCount - N;

    if (!writePointer(ptrName, uploadStart)) {
      Serial.println("WARNING: skip-advance pointer write failed – aborting upload");
      logFile.close(); return;
    }
    Serial.printf("Skipped %d old line(s) → upload start offset %u\n",
                  skipped, (unsigned)uploadStart);
  }

  // Seek to upload start
  if (!logFile.seek(uploadStart)) {
    Serial.printf("seek(%u) failed → aborting\n", (unsigned)uploadStart);
    logFile.close(); return;
  }

  Serial.printf("Uploading %s  from offset %u / %u bytes  (up to %d lines)\n",
                logName.c_str(), (unsigned)uploadStart,
                (unsigned)fileSize, N);

  // Open one HTTP session for the whole batch (TLS handshake paid once)
  gsm_httpInit();

  int  count    = 0;
  bool hadError = false;

  while (logFile.available() && count < N) {
    wdt_feed();

    String line = logFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;   // blank / newline-only line – skip silently

    // Parse JSON
    StaticJsonDocument<256> doc;
    DeserializationError derr = deserializeJson(doc, line);
    if (derr) {
      // Corrupt line: advance pointer past it so it is never retried
      size_t badOfs = logFile.position();
      Serial.printf("JSON parse error (%s) – skipping corrupt line, ptr → %u\n",
                    derr.c_str(), (unsigned)badOfs);
      writePointer(ptrName, badOfs);
      continue;
    }

    // Stamp identity fields (always present in every POST)
    doc["device_id"]     = DEVICE_ID;
    doc["serial_number"] = SERIAL_NUMBER;

    String payload;
    serializeJson(doc, payload);

    if (gsm_postReading(payload)) {
      // POST confirmed – advance pointer immediately (at-least-once guarantee)
      size_t newOffset = logFile.position();
      if (!writePointer(ptrName, newOffset)) {
        Serial.println("WARNING: pointer write failed after POST (may duplicate)");
      }
      count++;
      Serial.printf("--- POST OK (%d/%d)  ptr → %u ---\n",
                    count, N, (unsigned)newOffset);
      delay(1200);   // brief pause for modem stability between requests
    } else {
      Serial.println("POST failed – stopping batch, will retry next cycle");
      hadError = true;
      break;
    }
  }

  logFile.close();
  gsm_httpTerm();

  Serial.printf("Upload batch done. sent=%d%s\n",
                count, hadError ? " (stopped early)" : "");
}

/* ============================================================
 *  SETUP
 * ============================================================ */

void setup() {
  Serial.begin(115200);
  delay(100);

  wdt_init();

  // SEN55
  Wire.begin(21, 22);
  sen5x.begin(Wire);
  sen5x.deviceReset();
  delay(200);
  sen5x.startMeasurement();
  warmupStartTime = millis();
  Serial.println("SEN55 measurement started (3-min warm-up)");

  // GSM UART – always needed, open unconditionally
  sim7600.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  delay(3000);   // modem power-on boot time
  Serial.println("SIM7600 UART open");

  // Modem init – retry up to 3 times then reboot
  bool modemOk = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    wdt_feed();
    Serial.printf("Modem init attempt %d/3...\n", attempt);
    if (gsm_initModem()) { modemOk = true; break; }
    delay(5000);
  }
  if (!modemOk) {
    Serial.println("Modem init failed after 3 attempts – rebooting in 10 s");
    delay(10000);
    esp_restart();
  }

  // Time sync – try 3 times (each call itself tries 3 NTP servers)
  Serial.println("Syncing time via GSM NTP...");
  bool timeOk = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    wdt_feed();
    Serial.printf("NTP sync attempt %d/3...\n", attempt);
    if (gsm_syncTime()) { timeOk = true; break; }
    delay(5000);
  }
  if (!timeOk) {
    // Not fatal – sensor readings will be skipped until timestamps validate,
    // and resync is attempted after MAX_INVALID_TIME_RETRIES bad cycles.
    Serial.println("Time sync failed – will retry automatically in operation");
  }

  // SD – reboot on failure
  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed – rebooting in 5 s");
    delay(5000);
    esp_restart();
  }
  Serial.println("SD ready");
  loadCurrentLogFileFromMeta();
  updateLogFileFromRTC();

  Serial.println("=== Setup complete – entering main loop ===");
}

/* ============================================================
 *  LOOP
 * ============================================================ */

void loop() {
  wdt_feed();
  system_check_scheduled_restart();

  // Warm-up guard (same as original)
  if (!sensorReady) {
    if (millis() - warmupStartTime >= WARMUP_TIME) {
      sensorReady = true;
      Serial.println("SEN55 ready");
    }
    return;
  }

  /* ── Sensor read + SD log  every 1 minute ── */
  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();

    SensorReading reading;
    if (sensor_read(reading)) {
      time_t recordedEpoch = 0;
      String timestamp = sensor_get_validated_timestamp(recordedEpoch);

      if (timestamp == "") {
        invalidTimeCount++;
        Serial.println("Invalid timestamp → skipping this reading");
        if (invalidTimeCount >= MAX_INVALID_TIME_RETRIES) {
          Serial.println("Too many invalid timestamps → forcing NTP resync");
          sensor_attempt_ntp_resync();
          invalidTimeCount = 0;
        }
      } else {
        invalidTimeCount = 0;

        StaticJsonDocument<256> doc;
        doc["pm2_5"]         = reading.pm2_5;
        doc["pm10"]          = reading.pm10_0;
        doc["humidity"]      = reading.humidity;
        doc["temperature"]   = reading.temperature;
        doc["voc_index"]     = reading.vocIndex;
        doc["nox_index"]     = reading.noxIndex;
        doc["recorded_at"]   = timestamp;
        doc["device_id"]     = DEVICE_ID;
        doc["serial_number"] = SERIAL_NUMBER;

        // Rotate log file if the hour has changed
        updateLogFileFromRTC();

        // Guard: path must be non-empty before opening SD
        if (currentLogFile.length() == 0) {
          Serial.println("SD log skipped: log path not set (time invalid)");
        } else {
          File f = SD.open(currentLogFile, FILE_APPEND);
          if (f) {
            serializeJson(doc, f);
            f.println();
            f.close();
            Serial.println("Logged to SD: " + timestamp);
          } else {
            Serial.println("sd failed to log");
          }
        }
      }
    }
  }

  /* ── Batch upload  every 10 minutes ── */
  if (millis() - lastUploadTime >= UPLOAD_INTERVAL) {
    lastUploadTime = millis();
    Serial.println("Starting upload from SD...");
    uploadFromSD();
    Serial.println("Upload batch done");
  }

  delay(100);
}
