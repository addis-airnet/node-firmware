#include "sd_logger.h"
#include "config.h"
#include <SD.h>

bool sdInit() {
  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed");
    return false;
  }
  return true;
}

bool sdAppend(const JsonDocument& doc) {
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("SD write failed");
    return false;
  }
  serializeJson(doc, f);
  f.println();
  f.close();
  return true;
}

bool sdReadNext(JsonDocument& doc, size_t& newOffset) {

  File log = SD.open(LOG_FILE, FILE_READ);
  if (!log) return false;

  size_t offset = 0;
  File ptr = SD.open(UPLOAD_PTR_FILE, FILE_READ);
  if (ptr) {
    offset = ptr.parseInt();
    ptr.close();
  }

  if (!log.seek(offset)) {
    offset = 0;
    log.seek(0);
  }

  String line = log.readStringUntil('\n');
  newOffset = log.position();
  log.close();

  return !deserializeJson(doc, line);
}
