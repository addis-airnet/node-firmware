#pragma once
#include "SensirionI2CSen5x.h"

class SEN55 {
public:
  bool begin(TwoWire& wire);
  bool ready();
  bool read(float&, float&, float&, float&, float&, float&, float&, float&);

private:
  SensirionI2CSen5x sen5x;
  unsigned long warmupStart = 0;
  bool warmed = false;
};
