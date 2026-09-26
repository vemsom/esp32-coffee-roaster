#pragma once

#include "sensors.h"

// Latched safety alarm. Once tripped, the alarm cannot be silenced: the
// heater is held off until the condition that tripped it is measurably gone
// (see safety.cpp). Safety is evaluated on the validated sensor sample, so it
// is independent of PID, profile and web/MQTT commands. The latch is written
// to NVS on every transition, so a power cycle during an alarm boots straight
// back into it (restored in safety_init()).
enum SafetyFaultCode {
  SAFETY_OK = 0,
  SAFETY_OVER_TEMP_BT,  // bean temperature at or above SAFETY_MAX_TEMP_C
  SAFETY_OVER_TEMP_ET,  // environment temperature at or above SAFETY_MAX_ET_TEMP_C
  SAFETY_SENSOR_BT,     // BT probe NaN / implausible / stuck jump, sustained
  SAFETY_SENSOR_ET,     // ET probe NaN / implausible / stuck jump, sustained
  SAFETY_SPREAD,        // BT and ET disagree while both are still cold
  SAFETY_STUCK_BT,      // BT identical for SENSOR_STUCK_MAX_MS with heat on
  SAFETY_STUCK_ET,      // ET identical for SENSOR_STUCK_MAX_MS with heat on
  // Not a fault: upper bound for codes read back from NVS, so a corrupt store
  // cannot boot the firmware into an unknown alarm state.
  SAFETY_FAULT_COUNT,
};

void safety_init();
// Call once per sensor sample, before any control update. heaterActive is
// whether the element is being asked for power right now - it is what arms the
// stuck-probe check (see safety.cpp for why the gate is "now" and not
// "recently").
void safety_update(const SensorReading &r, bool heaterActive);

bool safety_faulted();
SafetyFaultCode safety_code();
const char *safety_code_text();          // "none", "bt over temp", ...
