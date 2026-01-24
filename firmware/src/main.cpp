#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SD.h>
#include <time.h>

#include "SensirionI2CSen5x.h"

/* ===================== CONFIG ===================== */

#define WIFI_SSID "Lebse"
#define WIFI_PASSWORD ""

#define DEVICE_ID "5afd4455-93ab-4501-b12a-19feb37c607e"
#define API_KEY   "1NkPL7xXwa-JlPqPS_FmQDHH_qp3f-DOMpRhGX4kC04"
#define SERIAL_NUMBER "AQ-ET-AA-ETB-2026-01"

#define API_URL "https://air-q-9f333037f389.herokuapp.com/api/v1/sensor-readings/device"

#define SENSOR_READ_INTERVAL 60000UL
#define UPLOAD_INTERVAL      600000UL   // 10 minutes
#define WARMUP_TIME          10000UL

#define SD_CS 5
#define LOG_FILE "/log.txt"
#define UPLOAD_PTR_FILE "/upload.ptr"

#define MAX_LINES_PER_UPLOAD 10

#define NTP_SERVER "pool.ntp.org"

/* ===================== GLOBALS ===================== */

SensirionI2CSen5x sen5x;

unsigned long lastSensorRead = 0;
unsigned long lastUploadTime = 0;
unsigned long warmupStartTime = 0;


bool sensorReady = false;

/* ===================== PROTOTYPES ===================== */

void readSEN55(float&, float&, float&, float&, float&, float&, float&, float&);
void logReadingToSD(JsonDocument&);
void uploadFromSD();
String getTimestampISO();

/* ===================== SETUP ===================== */

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  sen5x.begin(Wire);
  sen5x.deviceReset();
  sen5x.startMeasurement();
  warmupStartTime = millis();

  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed");
  } else {
    Serial.println("SD ready");
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  /* ---------- NTP TIME SYNC (CRITICAL FOR HTTPS) ---------- */
  configTime(0, 0, NTP_SERVER);

  Serial.print("Syncing time");
  time_t now;
  while (true) {
    time(&now);
    if (now > 1700000000) break; // valid time (post-2023)
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nTime synced");
}

/* ===================== LOOP ===================== */

void loop() {

  if (!sensorReady) {
    if (millis() - warmupStartTime >= WARMUP_TIME) {
      sensorReady = true;
      Serial.println("SEN55 ready");
    }
    return;
  }

  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();

    float pm1_0, pm2_5, pm4_0, pm10_0;
    float humidity, temperature, vocIndex, noxIndex;

    Serial.println("Starting sensor read...");
    readSEN55(pm1_0, pm2_5, pm4_0, pm10_0,
              humidity, temperature, vocIndex, noxIndex);
    Serial.println("Sensor read completed");

    StaticJsonDocument<256> doc;
    doc["pm2_5"] = pm2_5;
    doc["pm10"] = pm10_0;
    doc["humidity"] = humidity;
    doc["temperature"] = temperature;
    doc["voc_index"] = (float)vocIndex;
    doc["nox_index"] = (float)noxIndex;
    doc["recorded_at"] = getTimestampISO();
    doc["device_id"] = DEVICE_ID;
    doc["serial_number"] = SERIAL_NUMBER;

    Serial.println("Starting SD log...");
    logReadingToSD(doc);
    Serial.println("SD log completed");
  }

  if (millis() - lastUploadTime >= UPLOAD_INTERVAL) {
    lastUploadTime = millis();
    Serial.println("Starting upload from SD...");
    uploadFromSD();
    Serial.println("Upload batch done");
  }
}


/* ===================== FUNCTIONS ===================== */

void readSEN55(
  float& pm1_0, float& pm2_5, float& pm4_0, float& pm10_0,
  float& humidity, float& temperature, float& vocIndex, float& noxIndex
) {
  sen5x.readMeasuredValues(pm1_0, pm2_5, pm4_0, pm10_0,
                           humidity, temperature, vocIndex, noxIndex);
}

void logReadingToSD(JsonDocument& doc) {
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) return;

  serializeJson(doc, f);
  f.println();
  f.close();

  Serial.println("Logged to SD");
}

bool postReading(JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30);
  client.setTimeout(30000);

  HTTPClient https;
  https.setReuse(false);

  if (!https.begin(client, API_URL)) {
    Serial.println("HTTPS begin failed");
    return false;
  }

  doc["device_id"] = DEVICE_ID;
  doc["serial_number"] = SERIAL_NUMBER;

  String payload;
  serializeJson(doc, payload);

  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-Device-ID", DEVICE_ID);
  https.addHeader("X-API-Key", API_KEY);
  
  Serial.println("Payload:");
  Serial.println(payload);
  int code = https.POST(payload);

  Serial.print("HTTP code: ");
  Serial.println(code);

  https.end();

  return (code == 201);
}

void uploadFromSD() {

  File logFile = SD.open(LOG_FILE, FILE_READ);
  if (!logFile) return;

  size_t offset = 0;
  File ptrFile = SD.open(UPLOAD_PTR_FILE, FILE_READ);
  if (ptrFile) {
    offset = ptrFile.parseInt();
    ptrFile.close();
  }

  if (!logFile.seek(offset)) {
    offset = 0;
    logFile.seek(0);
  }

  int count = 0;
  size_t newOffset = offset;

  while (logFile.available() && count < MAX_LINES_PER_UPLOAD) {

    String line = logFile.readStringUntil('\n');
    if (line.length() == 0) continue;

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, line)) {
      newOffset = logFile.position();
      continue;
    }

    if (postReading(doc)) {
      newOffset = logFile.position();

      File w = SD.open(UPLOAD_PTR_FILE, FILE_WRITE);
      if (w) {
        w.print(newOffset);
        w.close();
      }

      count++;
    } else {
      Serial.println("Upload failed, stopping batch");
      break;
    }
  }

  logFile.close();
}

String getTimestampISO() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01T00:00:00Z";

  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}