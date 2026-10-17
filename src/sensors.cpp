#include "sensors.h"
#include "config.h"
#include <Arduino.h>
#include <max6675.h>

// Each module now has its own SO and its own CS; only SCLK is shared.
MAX6675 thermoBT(PIN_MAX6675_CLK, PIN_MAX6675_CS_BT, PIN_MAX6675_MISO_BT);
#if SAFETY_ET_ENABLED
MAX6675 thermoET(PIN_MAX6675_CLK, PIN_MAX6675_CS_ET, PIN_MAX6675_MISO_ET);
#endif

static float lastGoodBT = NAN;
static int consecutiveFaultsBT = 0;
#if SAFETY_ET_ENABLED
static float lastGoodET = NAN;
static int consecutiveFaultsET = 0;
#endif

void sensors_init() {
  delay(300);  // MAX6675 needs time after power-up before the first read
}

// A MAX6675 with a detached probe either returns NaN (bit 2 set) or - if the
// data line floats low - a fixed reading near 0 C. Both must count as faults,
// otherwise a dropped probe can look like a valid cold roaster.
static bool implausible(float value) {
  return value < SENSOR_MIN_VALID_C || value > SENSOR_MAX_VALID_C;
}

static float readWithSanityCheck(MAX6675 &sensor, float &lastGood, int &faultCount, bool &faultOut) {
  float value = sensor.readCelsius();

  if (isnan(value) || implausible(value)) {
    faultCount++;
    faultOut = true;
    return lastGood;
  }

  if (!isnan(lastGood) && fabs(value - lastGood) > SENSOR_FAULT_MAX_JUMP_C) {
    faultCount++;
    faultOut = true;
    return lastGood;
  }

  faultCount = 0;
  faultOut = false;
  lastGood = value;
  return value;
}

SensorReading sensors_read() {
  SensorReading r;
  r.bt = readWithSanityCheck(thermoBT, lastGoodBT, consecutiveFaultsBT, r.btFault);
#if SAFETY_ET_ENABLED
  r.et = readWithSanityCheck(thermoET, lastGoodET, consecutiveFaultsET, r.etFault);
#else
  // BT-only build (SAFETY_ET_ENABLED 0, include/config.h): the ET converter is
  // not read at all - no SPI transfer, no ET value anywhere in the firmware.
  // A channel that is not read has no reading, so it reports NaN, which is
  // exactly what a dead ET module already reports over /api/status. There is no
  // fault flag either: there is nothing to fault, and a fault could only feed a
  // detector that is switched off. Everything downstream treats a NaN ET the
  // way it treats a missing probe (safety ignores it, RoR leaves it out of the
  // fit, the UI shows "--").
  r.et = NAN;
  r.etFault = false;
#endif

  // Offset applies AFTER the plausibility/jump checks, never before: an open
  // probe (NaN or 0 C) must stay a fault. The offset is applied exactly once
  // here, so everything downstream sees the same number: safety limits, the
  // BT/ET cross-check, RoR, MQTT/HA and the web UI.
  if (!r.btFault) r.bt += SENSOR_OFFSET_BT_C;
#if SAFETY_ET_ENABLED
  if (!r.etFault) r.et += SENSOR_OFFSET_ET_C;
#endif

  return r;
}
