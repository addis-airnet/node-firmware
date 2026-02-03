#include "http_uploader.h"
#include "config.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFi.h>

bool postReading(JsonDocument& doc) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi down");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, API_URL)) return false;

  doc["device_id"] = DEVICE_ID;
  doc["serial_number"] = SERIAL_NUMBER;

  String payload;
  serializeJson(doc, payload);

  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-Device-ID", DEVICE_ID);
  https.addHeader("X-API-Key", API_KEY);

  int code = https.POST(payload);
  https.end();

  if (code != 201) {
    Serial.printf("HTTP error: %d\n", code);
    return false;
  }

  return true;
}
