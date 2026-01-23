#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <Wire.h>
#include <SD.h>
#include <time.h>

#include "SensirionI2CSen5x.h"
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

/* ===================== CONFIG ===================== */
#define WIFI_SSID "Lebse"
#define WIFI_PASSWORD ""

#define API_KEY "AIzaSyDeT6jMxkcVfsaA__vtLAvtKCq1h81jrXM"
#define DATABASE_URL "https://air-quality-4283c-default-rtdb.europe-west1.firebasedatabase.app/"
#define USER_EMAIL "natnaelabdissa8@gmail.com"
#define USER_PASSWORD "oogabooga"

#define NTP_SERVER "pool.ntp.org"

#define SENSOR_READ_INTERVAL 30000UL
#define UPLOAD_INTERVAL      120000UL   // upload every 2 minutes
#define WARMUP_TIME          60000UL     // SEN55 warmup

#define SD_CS 5
#define LOG_FILE "/log.txt"
#define UPLOAD_PTR_FILE "/upload.ptr"

#define MAX_LINES_PER_UPLOAD 10

/* ===================== GLOBALS ===================== */
SensirionI2CSen5x sen5x;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long lastSensorRead = 0;
unsigned long lastUploadTime = 0;
unsigned long warmupStartTime = 0;

bool sensorReady = false;

char deviceID[18];

/* ===================== FUNCTION PROTOTYPES ===================== */
void readSEN55(
  float& pm1_0, float& pm2_5, float& pm4_0, float& pm10_0,
  float& humidity, float& temperature, float& vocIndex, float& noxIndex
);

void logReadingToSD(FirebaseJson& reading);
void uploadFromSD();
void getDeviceID(char* id);
String getTimestamp();

/* ===================== SETUP ===================== */
void setup() {
  Serial.begin(9600);
  Wire.begin(21, 22);

  /* ---- SEN55 ---- */
  sen5x.begin(Wire);
  sen5x.deviceReset();
  getDeviceID(deviceID);
  sen5x.startMeasurement();
  warmupStartTime = millis();

  /* ---- SD ---- */
  if (!SD.begin(SD_CS, SPI, 1000000)) {
    Serial.println("SD init failed");
  } else {
    Serial.println("SD ready");
  }

  /* ---- WIFI ---- */
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  /* ---- TIME ---- */
  setenv("TZ", "EAT", 1);   // UTC+3
  tzset();
  configTime(0, 0, NTP_SERVER);

  /* ---- FIREBASE ---- */
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  Firebase.begin(&config, &auth);
  Firebase.setDoubleDigits(5);

  Serial.println("Firebase ready");
}

/* ===================== LOOP ===================== */
void loop() {

  /* ---- SEN55 WARMUP ---- */
  if (!sensorReady) {
    if (millis() - warmupStartTime >= WARMUP_TIME) {
      sensorReady = true;
      Serial.println("SEN55 ready");
    }
    return;
  }

  /* ---- SENSOR READ ---- */
  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();

    float pm1_0, pm2_5, pm4_0, pm10_0;
    float humidity, temperature, vocIndex, noxIndex;

    readSEN55(pm1_0, pm2_5, pm4_0, pm10_0,
              humidity, temperature, vocIndex, noxIndex);

    FirebaseJson reading;
    reading.set("timestamp", getTimestamp());
    reading.set("pm1_0", pm1_0);
    reading.set("pm2_5", pm2_5);
    reading.set("pm4_0", pm4_0);
    reading.set("pm10", pm10_0);
    reading.set("humidity", humidity);
    reading.set("temperature", temperature);
    reading.set("voc_index", vocIndex);
    reading.set("nox_index", noxIndex);

    logReadingToSD(reading);
  }

  /* ---- SD → FIREBASE UPLOAD ---- */
  if (millis() - lastUploadTime >= UPLOAD_INTERVAL) {
    lastUploadTime = millis();
    uploadFromSD();
  }
}

/* ===================== FUNCTIONS ===================== */

void readSEN55(
  float& pm1_0, float& pm2_5, float& pm4_0, float& pm10_0,
  float& humidity, float& temperature, float& vocIndex, float& noxIndex
) {
  uint16_t error = sen5x.readMeasuredValues(
    pm1_0, pm2_5, pm4_0, pm10_0,
    humidity, temperature, vocIndex, noxIndex
  );
if (error) {
    Serial.println("SEN55 read error");
  }
}

void logReadingToSD(FirebaseJson& reading) {
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("SD write failed");
    return;
  }

  String s;
  reading.toString(s, false);  // compact JSON
  f.println(s);
  f.close();

  Serial.println("Logged to SD");
}
/*
void uploadFromSD() {
  if (!Firebase.ready()) return;

  File f = SD.open(LOG_FILE, FILE_READ);
  if (!f) {
    Serial.println("No SD log file");
    return;
  }

  FirebaseJson readings;
  FirebaseJson batch;

  int count = 0;
  String remaining = "";

  while (f.available()) {
    String line = f.readStringUntil('\n');

    if (count < MAX_LINES_PER_UPLOAD) {
      FirebaseJson r;
      r.setJsonData(line);
      readings.set(String(count), r);
      count++;
    } else {
      remaining
       += line + "\n";
    }
  }
  f.close();

  if (count == 0) return;

  batch.set("device_id", deviceID);
  batch.set("readings", readings);

  if (Firebase.pushJSON(fbdo, "/sensor/data", batch)) {
    Serial.println("SD batch uploaded");

    File w = SD.open(LOG_FILE, FILE_WRITE);
    if (w) {
      w.print(remaining);
      w.close();
    }
  } else {
    Serial.println(" Upload failed");
    Serial.println(fbdo.errorReason());
  }
}
*/
void uploadFromSD() {
  if (!Firebase.ready()) return;


  File logFile = SD.open(LOG_FILE, FILE_READ);
  if (!logFile) {
    Serial.println("Failed to open log file");
    return;
  }


  size_t offset = 0;
  File ptrFile = SD.open(UPLOAD_PTR_FILE, FILE_READ);
  if (ptrFile) {
    offset = ptrFile.parseInt();
    ptrFile.close();
  }

  
  if (!logFile.seek(offset)) {
    Serial.println("Seek failed, resetting pointer");
    offset = 0;
    logFile.seek(0);
  }

  FirebaseJson readings;
  FirebaseJson batch;

  int count = 0;
  size_t newOffset = offset;

  
  while (logFile.available() && count < MAX_LINES_PER_UPLOAD) {
    String line = logFile.readStringUntil('\n');
    if (line.length() == 0) continue;

    FirebaseJson r;
    r.setJsonData(line);
    readings.set(String(count), r);

    newOffset = logFile.position();
    count++;
  }

  logFile.close();

  if (count == 0) {
    Serial.println("No new data to upload");
    return;
  }

  // ---- Build batch ----
  batch.set("device_id", deviceID);
  batch.set("readings", readings);

  delay(500); 

  // ---- Upload ----
  if (Firebase.pushJSON(fbdo, "/sensor/data", batch)) {
    Serial.println("Batch uploaded successfully");

    // ---- Save new pointer ----
    File ptrWrite = SD.open(UPLOAD_PTR_FILE, FILE_WRITE);
    if (ptrWrite) {
      ptrWrite.print(newOffset);
      ptrWrite.close();
    }
  } else {
    Serial.println("Upload failed");
    Serial.println(fbdo.errorReason());
  }
}


void getDeviceID(char* id) {
  uint8_t serial[6];
  if (sen5x.getSerialNumber(serial, sizeof(serial))) return;

  sprintf(id, "%02X:%02X:%02X:%02X:%02X:%02X",
          serial[0], serial[1], serial[2],
          serial[3], serial[4], serial[5]);
}

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01 00:00:00";

  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}