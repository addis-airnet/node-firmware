#pragma once

#define WIFI_SSID "YOUR_WIFI"
#define WIFI_PASSWORD "YOUR_PASS"

#define DEVICE_ID "PUT_UUID_HERE"
#define API_KEY   "PUT_API_KEY_HERE"
#define SERIAL_NUMBER "AQ-ET-AA-ETB-2026-01"

#define API_URL "https://air-q-9f333037f389.herokuapp.com/api/v1/sensor-readings/device"

#define SENSOR_READ_INTERVAL 30000UL
#define UPLOAD_INTERVAL      600000UL
#define WARMUP_TIME          60000UL

#define SD_CS 5
#define LOG_FILE "/log.txt"
#define UPLOAD_PTR_FILE "/upload.ptr"
#define MAX_LINES_PER_UPLOAD 10

#define NTP_SERVER "pool.ntp.org"
