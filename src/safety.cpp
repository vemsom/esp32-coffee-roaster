#include "safety.h"
#include "config.h"
#include <Arduino.h>
#include <Preferences.h>

// Four independent trip conditions feed the same latch:
//   1. Hard temperature limit - a measured value at or above the configured
//      ceiling (SAFETY_MAX_TEMP_C for BT, SAFETY_MAX_ET_TEMP_C for ET).
//   2. Sensor fault - NaN, implausible value, or an implausible jump, sustained
//      for SENSOR_FAULT_MAX_COUNT samples. MAX6675 has no fault reporting, so
//      this is the only way to notice a dropped probe. A probe that falls off
//      mid-roast reads 0 C or NaN, both of which land here (a jump from roast
//      temperature to 0 C is > SENSOR_FAULT_MAX_JUMP_C), and a probe that is
//      disconnected at power-up reads a steady 0 C, which SENSOR_MIN_VALID_C
//      catches - the lower bound used to be -10 C, so a bench run with
//      nothing connected read 0 C everywhere and never tripped at all.
//   3. Probe disagreement - the two probes live in the same chamber, so while
//      both are still cold they must agree. A spread above
//      SENSOR_MAX_SPREAD_C means one reading is wrong even though each is
//      plausible on its own, and there is no way to tell which one.
//   4. Stuck probe - the same reading, bit for bit, for SENSOR_STUCK_MAX_MS
//      while the element is asking for power. A frozen probe is plausible,
//      does not jump and (alone) disagrees with nobody, so none of the checks
//      above can see it. The gate on live heat is what keeps cooling from
//      false-tripping; see include/config.h for the reasoning and the
//      numbers.
//
// The latch is deliberately one-way until the fault is gone: there is no
// command, no web call and no MQTT message that clears it. It clears only
// after SAFETY_CLEAR_STREAK consecutive samples that are all fault-free and
// below the limit minus SAFETY_CLEAR_MARGIN_C.
//
// Every transition is written to NVS (namespace "safety"), and safety_init()
// reads it back, so a power cycle in the middle of an alarm boots the heater
// straight back into the latched state instead of silently starting clear.
// Writes happen on transitions only - never per sample - because flash
// endurance is not a 4 Hz resource.
static bool latched = false;
static SafetyFaultCode activeCode = SAFETY_OK;
static unsigned long trippedAt = 0;
static int faultStreakBT = 0;
static int faultStreakET = 0;
static int faultStreakSpread = 0;
static int healthyStreak = 0;

// Stuck-probe bookkeeping: the value each channel last changed to, and when.
static float stuckValueBT = NAN;
static unsigned long stuckSinceBT = 0;
static float stuckValueET = NAN;
static unsigned long stuckSinceET = 0;

// A stuck alarm is released only by proof that the probe moves again - see
// safety_update(). Set when a SAFETY_STUCK_* code is tripped or restored.
static bool stuckPending = false;
static float stuckPendingValue = NAN;

static const char *PREFS_NAMESPACE = "safety";
static Preferences prefs;
static bool prefsOpen = false;

static void persistLatch() {
  if (!prefsOpen) return;
  // Code first, flag second: a reset between the two writes must never leave
  // "latched" pointing at a stale or unknown reason.
  size_t codeWritten = prefs.putInt("code", (int32_t)activeCode);
  size_t flagWritten = prefs.putBool("latched", latched);
  if (codeWritten == 0 || flagWritten == 0) {
    Serial.println("[SAFETY] NVS write failed - the latch will not survive a reboot");
  }
}

static const char *codeText(SafetyFaultCode c) {
  switch (c) {
    case SAFETY_OVER_TEMP_BT: return "bt over temp";
    case SAFETY_OVER_TEMP_ET: return "et over temp";
    case SAFETY_SENSOR_BT:    return "bt sensor fault";
    case SAFETY_SENSOR_ET:    return "et sensor fault";
    case SAFETY_SPREAD:       return "bt/et disagree";
    case SAFETY_STUCK_BT:     return "bt sensor stuck";
    case SAFETY_STUCK_ET:     return "et sensor stuck";
    default:                  return "none";
  }
}

static bool isStuckCode(SafetyFaultCode c) {
  return c == SAFETY_STUCK_BT || c == SAFETY_STUCK_ET;
}

const char *safety_code_text() { return codeText(activeCode); }

// True when this channel has read the identical value for longer than
// SENSOR_STUCK_MAX_MS *while the element was asking for power for all of it*.
// With the element off the timer is held at "now", so the window can only be
// spent under real heat - that single rule is what makes the tail of a cooling
// cycle (where the reading stalls for minutes) unable to trip.
static bool stuckForTooLong(float value, float &lastValue, unsigned long &since,
                            bool heaterActive, unsigned long now) {
  if (!heaterActive) {
    lastValue = value;
    since = now;
    return false;
  }
  // A NaN resets the timer: an open probe is the sensor-fault streak's job,
  // not this one's.
  if (isnan(lastValue) || value != lastValue) {
    lastValue = value;
    since = now;
    return false;
  }
  return (now - since) >= SENSOR_STUCK_MAX_MS;
}

static void trip(SafetyFaultCode c) {
  if (latched) return;
  latched = true;
  activeCode = c;
  trippedAt = millis();
  healthyStreak = 0;
  if (isStuckCode(c)) {
    // The value it froze on, so a later sample that differs from *that* is
    // the proof that releases the latch (see safety_update).
    stuckPending = true;
    stuckPendingValue = (c == SAFETY_STUCK_ET) ? stuckValueET : stuckValueBT;
  }
  Serial.print("[SAFETY] LATCHED: ");
  Serial.println(codeText(c));
  persistLatch();
}

void safety_init() {
  latched = false;
  activeCode = SAFETY_OK;
  trippedAt = 0;
  faultStreakBT = 0;
  faultStreakET = 0;
  faultStreakSpread = 0;
  healthyStreak = 0;
  stuckValueBT = NAN;
  stuckSinceBT = 0;
  stuckValueET = NAN;
  stuckSinceET = 0;
  stuckPending = false;
  stuckPendingValue = NAN;

  // Restore the latch written by a previous run. If NVS will not open the
  // alarm still works - it just lives in RAM only, which is logged once.
  prefsOpen = prefs.begin(PREFS_NAMESPACE, false);
  if (!prefsOpen) {
    Serial.println("[SAFETY] NVS unavailable - the alarm will not survive a reboot");
    return;
  }
  if (!prefs.getBool("latched", false)) return;

  int32_t code = prefs.getInt("code", (int32_t)SAFETY_OK);
  if (code <= (int32_t)SAFETY_OK || code >= (int32_t)SAFETY_FAULT_COUNT) {
    // Half-written or corrupt record: boot clear rather than into an unknown
    // state, and fix the record so the next boot does not see it again.
    Serial.println("[SAFETY] stored alarm record was invalid - ignored");
    prefs.putInt("code", (int32_t)SAFETY_OK);
    prefs.putBool("latched", false);
    return;
  }

  latched = true;
  activeCode = (SafetyFaultCode)code;
  trippedAt = millis();
  if (isStuckCode(activeCode)) {
    // A stuck alarm restored across a power cycle would otherwise clear on
    // the first healthy streak - the sample looks healthy, that is exactly
    // what "stuck" means - and the detector is disarmed at boot (no heat
    // yet), so nothing would re-trip it. Hold it until the probe reports a
    // value that differs from the one it is showing right now.
    stuckPending = true;
    stuckPendingValue = NAN;
  }
  Serial.print("[SAFETY] restored from NVS, still latched: ");
  Serial.println(codeText(activeCode));
}

// Both probes in the same chamber, both still cold: they must agree. This
// catches a probe that is wrong but individually plausible (a stray reading
// sitting at room temperature while the other one is right), which the
// per-channel plausibility window cannot see. Returns false as soon as either
// probe is hot - once roasting has started, ET vs BT is real physics.
static bool coldSpreadFault(const SensorReading &r) {
  if (isnan(r.bt) || isnan(r.et)) return false;
  if (r.bt >= SENSOR_SPREAD_MAX_COLD_C || r.et >= SENSOR_SPREAD_MAX_COLD_C) return false;
  return fabs(r.bt - r.et) > SENSOR_MAX_SPREAD_C;
}

void safety_update(const SensorReading &r, bool heaterActive) {
  unsigned long now = millis();

  if (r.btFault) faultStreakBT++; else faultStreakBT = 0;
  if (r.etFault) faultStreakET++; else faultStreakET = 0;
  if (coldSpreadFault(r)) faultStreakSpread++; else faultStreakSpread = 0;

  // Stuck-probe tracking runs on every sample, latched or not: while latched
  // it keeps watching so a healed probe can be seen to move again.
  const bool stuckBT =
      stuckForTooLong(r.bt, stuckValueBT, stuckSinceBT, heaterActive, now);
  const bool stuckET =
      stuckForTooLong(r.et, stuckValueET, stuckSinceET, heaterActive, now);

  const bool btKnown = !isnan(r.bt);
  const bool etKnown = !isnan(r.et);
  const bool overBT = btKnown && r.bt >= SAFETY_MAX_TEMP_C;
  const bool overET = etKnown && r.et >= SAFETY_MAX_ET_TEMP_C;

  if (!latched) {
    if (overBT) { trip(SAFETY_OVER_TEMP_BT); return; }
    if (overET) { trip(SAFETY_OVER_TEMP_ET); return; }
    if (faultStreakBT >= SENSOR_FAULT_MAX_COUNT) { trip(SAFETY_SENSOR_BT); return; }
    if (faultStreakET >= SENSOR_FAULT_MAX_COUNT) { trip(SAFETY_SENSOR_ET); return; }
    if (faultStreakSpread >= SENSOR_FAULT_MAX_COUNT) { trip(SAFETY_SPREAD); return; }
    if (stuckBT) { trip(SAFETY_STUCK_BT); return; }
    if (stuckET) { trip(SAFETY_STUCK_ET); return; }
    return;
  }

  // A stuck alarm is held by proof, not by time. Its samples look perfectly
  // healthy - that is what "stuck" means - so the clear streak below would
  // silence it 2.5 s after it tripped, or immediately after a reboot (where
  // the detector starts out disarmed). The latch releases once the frozen
  // channel reports something other than the value it froze on.
  if (stuckPending) {
    float frozen = (activeCode == SAFETY_STUCK_ET) ? r.et : r.bt;
    if (!isnan(frozen)) {
      if (isnan(stuckPendingValue)) {
        stuckPendingValue = frozen;
      } else if (frozen != stuckPendingValue) {
        stuckPending = false;
        stuckPendingValue = NAN;
        Serial.println("[SAFETY] the stuck probe reports movement again - latch can clear normally");
      }
    }
  }

  const bool healthySample =
      !r.btFault && !r.etFault && btKnown && etKnown && !coldSpreadFault(r) &&
      !stuckPending &&
      r.bt <= SAFETY_MAX_TEMP_C - SAFETY_CLEAR_MARGIN_C &&
      r.et <= SAFETY_MAX_ET_TEMP_C - SAFETY_CLEAR_MARGIN_C;

  healthyStreak = healthySample ? healthyStreak + 1 : 0;

  if (healthyStreak >= SAFETY_CLEAR_STREAK) {
    latched = false;
    activeCode = SAFETY_OK;
    trippedAt = 0;
    healthyStreak = 0;
    stuckPending = false;
    stuckPendingValue = NAN;
    Serial.println("[SAFETY] cleared - readings healthy again");
    persistLatch();
  }
}

bool safety_faulted() { return latched; }
SafetyFaultCode safety_code() { return activeCode; }
