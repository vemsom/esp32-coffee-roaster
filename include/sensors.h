#pragma once

// One validated sample of both thermocouples. When a channel is faulted the
// value holds the last known good reading (NaN if there never was one), so
// callers must check the fault flag before trusting the number.
struct SensorReading {
  float bt;      // bean temperature, C
  float et;      // environment temperature, C
  bool btFault;  // NaN, implausible value, or implausible jump
  bool etFault;
};

void sensors_init();
SensorReading sensors_read();
