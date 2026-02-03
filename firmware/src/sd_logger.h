#pragma once
#include <ArduinoJson.h>

bool sdInit();
bool sdAppend(const JsonDocument&);
bool sdReadNext(JsonDocument&, size_t& newOffset);
