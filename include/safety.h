#pragma once

#include "sensors.h"

// Latched safety alarm. Once tripped, the alarm cannot be silenced: the
// heater is held off until the condition that tripped it is measurably gone
// (see safety.cpp). Safety is evaluated on the validated sensor sample, so it
// is independent of PID, profile and web/MQTT commands.
enum SafetyFaultCode {
  SAFETY_OK = 0,
  SAFETY_OVER_TEMP_BT,  // bean temperature at or above SAFETY_MAX_TEMP_C
  SAFETY_OVER_TEMP_ET,  // environment temperature at or above SAFETY_MAX_ET_TEMP_C
  SAFETY_SENSOR_BT,     // BT probe NaN / implausible / stuck jump, sustained
  SAFETY_SENSOR_ET,     // ET probe NaN / implausible / stuck jump, sustained
};

void safety_init();
// Call once per sensor sample, before any control update.
void safety_update(const SensorReading &r);

bool safety_faulted();
SafetyFaultCode safety_code();
const char *safety_code_text();          // "none", "bt over temp", ...
unsigned long safety_fault_since_ms();   // millis() when the latch tripped, 0 when clear
