#include "sensors.h"
#include "config.h"
#include <Arduino.h>
#include <max6675.h>

// Each module now has its own SO and its own CS; only SCLK is shared.
MAX6675 thermoBT(PIN_MAX6675_CLK, PIN_MAX6675_CS_BT, PIN_MAX6675_MISO_BT);
MAX6675 thermoET(PIN_MAX6675_CLK, PIN_MAX6675_CS_ET, PIN_MAX6675_MISO_ET);

static float lastGoodBT = NAN;
static float lastGoodET = NAN;
static int consecutiveFaultsBT = 0;
static int consecutiveFaultsET = 0;

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
  r.et = readWithSanityCheck(thermoET, lastGoodET, consecutiveFaultsET, r.etFault);

  // Offset applies AFTER the plausibility/jump checks, never before: an open
  // probe (NaN or 0 C) must stay a fault. The offset is applied exactly once
  // here, so everything downstream sees the same number: safety limits, the
  // BT/ET cross-check, RoR, MQTT/HA and the web UI.
  if (!r.btFault) r.bt += SENSOR_OFFSET_BT_C;
  if (!r.etFault) r.et += SENSOR_OFFSET_ET_C;

  return r;
}
