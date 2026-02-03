#include "time_utils.h"
#include "config.h"
#include <time.h>

void timeInit() {
  configTime(0, 0, NTP_SERVER);
}

String getTimestampISO() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01T00:00:00Z";
  }

  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}
