#pragma once
// Host stub for the MAX6675 converter. The test injects the reading that the
// real chip would hand over, which is how "probe disconnected" is simulated:
// the converter itself has no fault flag, it just reports whatever the data
// line says - including a steady 0 C from a floating input.
#include <Arduino.h>
#include "config.h"

extern float g_probeBT;
extern float g_probeET;

class MAX6675 {
 public:
  MAX6675(int8_t clk, int8_t cs, int8_t miso) : _cs(cs) { (void)clk; (void)miso; }

  float readCelsius() const {
    return _cs == PIN_MAX6675_CS_BT ? g_probeBT : g_probeET;
  }

 private:
  int8_t _cs;
};
