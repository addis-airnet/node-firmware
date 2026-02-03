#include "sensor_sen55.h"
#include "config.h"
#include <Arduino.h>

bool SEN55::begin(TwoWire& wire) {
  sen5x.begin(wire);
  sen5x.deviceReset();

  if (sen5x.startMeasurement()) {
    Serial.println("SEN55 start failed");
    return false;
  }

  warmupStart = millis();
  return true;
}

bool SEN55::ready() {
  if (!warmed && millis() - warmupStart >= WARMUP_TIME) {
    warmed = true;
    Serial.println("SEN55 ready");
  }
  return warmed;
}

bool SEN55::read(float& pm1, float& pm25, float& pm4, float& pm10,
                 float& hum, float& temp, float& voc, float& nox) {

  uint16_t err = sen5x.readMeasuredValues(
    pm1, pm25, pm4, pm10, hum, temp, voc, nox
  );

  if (err) {
    Serial.printf("SEN55 read error: %u\n", err);
    return false;
  }
  return true;
}
