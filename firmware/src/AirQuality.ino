#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <SD.h>


#include "config.h"
#include "sensor_sen55.h"
#include "sd_logger.h"
#include "http_uploader.h"
#include "time_utils.h"

/* ===================== GLOBALS ===================== */

SEN55 sensor;

unsigned long lastSensorRead = 0;
unsigned long lastUploadTime = 0;

/* ===================== SETUP ===================== */

void setup() {
  Serial.begin(9600);
  delay(200);

  Wire.begin(21, 22);

  // ---- Sensor ----
  if (!sensor.begin(Wire)) {
    Serial.println("SEN55 init failed");
  }

  // ---- SD ----
  if (!sdInit()) {
    Serial.println("SD not available, logging disabled");
  }

  // ---- WiFi ----
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  // ---- Time ----
  timeInit();
  Serial.println("Time initialized");
}

/* ===================== LOOP ===================== */

void loop() {

  // ---- Wait for SEN55 warmup ----
  if (!sensor.ready()) {
    return;
  }

  // ---- Sensor Read ----
  if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = millis();

    float pm1, pm25, pm4, pm10;
    float humidity, temperature, vocIndex, noxIndex;

    if (!sensor.read(pm1, pm25, pm4, pm10,
                     humidity, temperature, vocIndex, noxIndex)) {
      Serial.println("Sensor read failed");
      return;
    }

    StaticJsonDocument<256> doc;
    doc["pm2_5"] = pm25;
    doc["pm10"] = pm10;
    doc["humidity"] = humidity;
    doc["temperature"] = temperature;
    doc["voc_index"] = vocIndex;
    doc["nox_index"] = noxIndex;
    doc["recorded_at"] = getTimestampISO();

    if (!sdAppend(doc)) {
      Serial.println("SD log failed");
    }
  }

  // ---- Upload ----
  if (millis() - lastUploadTime >= UPLOAD_INTERVAL) {
    lastUploadTime = millis();

    size_t newOffset;
    StaticJsonDocument<256> doc;

    if (sdReadNext(doc, newOffset)) {
      if (postReading(doc)) {
        File ptr = SD.open(UPLOAD_PTR_FILE, FILE_WRITE);
        if (ptr) {
          ptr.print(newOffset);
          ptr.close();
        }
      } else {
        Serial.println("HTTP upload failed");
      }
    }
  }
}
