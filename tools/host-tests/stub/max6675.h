#pragma once
// Host stub for the MAX6675 converter. The test injects the reading that the
// real chip would hand over, which is how "probe disconnected" is simulated:
// the converter itself has no fault flag, it just reports whatever the data
// line says - including a steady 0 C from a floating input.
#include <Arduino.h>
#include "config.h"

extern float g_probeBT;
extern float g_probeET;

// How many times the ET converter was read. The only thing that separates a
// BT-only build (SAFETY_ET_ENABLED 0) from a build that happens to see a dead
// ET module is that the BT-only one does not read it at all - and "did not
// read it" is not visible in any value, so it is counted here instead. inline
// so the counter is one variable per test program, not one per translation
// unit that includes this header.
inline int g_max6675EtReads = 0;

class MAX6675 {
 public:
  MAX6675(int8_t clk, int8_t cs, int8_t miso) : _cs(cs) { (void)clk; (void)miso; }

  float readCelsius() const {
    if (_cs == PIN_MAX6675_CS_ET) {
      g_max6675EtReads++;
      return g_probeET;
    }
    return g_probeBT;
  }

 private:
  int8_t _cs;
};
