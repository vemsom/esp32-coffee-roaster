#include "safety.h"
#include "config.h"
#include <Arduino.h>

// Two independent trip conditions feed the same latch:
//   1. Hard temperature limit - a measured value at or above the configured
//      ceiling (SAFETY_MAX_TEMP_C for BT, SAFETY_MAX_ET_TEMP_C for ET).
//   2. Sensor fault - NaN, implausible value, or an implausible jump, sustained
//      for SENSOR_FAULT_MAX_COUNT samples. MAX6675 has no fault reporting, so
//      this is the only way to notice a dropped probe. A probe that falls off
//      mid-roast reads 0 C or NaN, both of which land here (a jump from roast
//      temperature to 0 C is > SENSOR_FAULT_MAX_JUMP_C).
//
// The latch is deliberately one-way until the fault is gone: there is no
// command, no web call and no MQTT message that clears it. It clears only
// after SAFETY_CLEAR_STREAK consecutive samples that are all fault-free and
// below the limit minus SAFETY_CLEAR_MARGIN_C.
static bool latched = false;
static SafetyFaultCode activeCode = SAFETY_OK;
static unsigned long trippedAt = 0;
static int faultStreakBT = 0;
static int faultStreakET = 0;
static int healthyStreak = 0;

static const char *codeText(SafetyFaultCode c) {
  switch (c) {
    case SAFETY_OVER_TEMP_BT: return "bt over temp";
    case SAFETY_OVER_TEMP_ET: return "et over temp";
    case SAFETY_SENSOR_BT:    return "bt sensor fault";
    case SAFETY_SENSOR_ET:    return "et sensor fault";
    default:                  return "none";
  }
}

const char *safety_code_text() { return codeText(activeCode); }

static void trip(SafetyFaultCode c) {
  if (latched) return;
  latched = true;
  activeCode = c;
  trippedAt = millis();
  healthyStreak = 0;
  Serial.print("[SAFETY] LATCHED: ");
  Serial.println(codeText(c));
}

void safety_init() {
  latched = false;
  activeCode = SAFETY_OK;
  trippedAt = 0;
  faultStreakBT = 0;
  faultStreakET = 0;
  healthyStreak = 0;
}

void safety_update(const SensorReading &r) {
  if (r.btFault) faultStreakBT++; else faultStreakBT = 0;
  if (r.etFault) faultStreakET++; else faultStreakET = 0;

  const bool btKnown = !isnan(r.bt);
  const bool etKnown = !isnan(r.et);
  const bool overBT = btKnown && r.bt >= SAFETY_MAX_TEMP_C;
  const bool overET = etKnown && r.et >= SAFETY_MAX_ET_TEMP_C;

  if (!latched) {
    if (overBT) { trip(SAFETY_OVER_TEMP_BT); return; }
    if (overET) { trip(SAFETY_OVER_TEMP_ET); return; }
    if (faultStreakBT >= SENSOR_FAULT_MAX_COUNT) { trip(SAFETY_SENSOR_BT); return; }
    if (faultStreakET >= SENSOR_FAULT_MAX_COUNT) { trip(SAFETY_SENSOR_ET); return; }
    return;
  }

  const bool healthySample =
      !r.btFault && !r.etFault && btKnown && etKnown &&
      r.bt <= SAFETY_MAX_TEMP_C - SAFETY_CLEAR_MARGIN_C &&
      r.et <= SAFETY_MAX_ET_TEMP_C - SAFETY_CLEAR_MARGIN_C;

  healthyStreak = healthySample ? healthyStreak + 1 : 0;

  if (healthyStreak >= SAFETY_CLEAR_STREAK) {
    latched = false;
    activeCode = SAFETY_OK;
    trippedAt = 0;
    healthyStreak = 0;
    Serial.println("[SAFETY] cleared - readings healthy again");
  }
}

bool safety_faulted() { return latched; }
SafetyFaultCode safety_code() { return activeCode; }
unsigned long safety_fault_since_ms() { return trippedAt; }
