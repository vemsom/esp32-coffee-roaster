#include "sensors.h"
#include "config.h"
#include <Arduino.h>
#include <max6675.h>

MAX6675 thermoBT(PIN_MAX6675_CLK, PIN_MAX6675_CS_BT, PIN_MAX6675_MISO);
MAX6675 thermoET(PIN_MAX6675_CLK, PIN_MAX6675_CS_ET, PIN_MAX6675_MISO);

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

// The calibration offset is added to the raw reading BEFORE anything judges
// it. The window and the jump check exist to catch a broken probe, and a
// broken probe is broken on the corrected scale too: judging the raw number
// instead would let an offset drag a healthy reading out of the window, or
// smuggle a dead one into it, while the rest of the firmware only ever sees
// the corrected value - the fault logic has to agree with what it will act
// on. NaN survives the addition (IEEE 754), so a detached probe still reads
// as NaN here.
static float readWithSanityCheck(MAX6675 &sensor, float offsetC, float &lastGood,
                                 int &faultCount, bool &faultOut) {
  float value = sensor.readCelsius() + offsetC;

  if (isnan(value) || implausible(value)) {
    faultCount++;
    faultOut = true;
    return lastGood;
  }

  // lastGood holds corrected values too, so this compares like with like -
  // a constant offset cancels out of the difference either way.
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
  r.bt = readWithSanityCheck(thermoBT, SENSOR_BT_OFFSET_C, lastGoodBT,
                             consecutiveFaultsBT, r.btFault);
  r.et = readWithSanityCheck(thermoET, SENSOR_ET_OFFSET_C, lastGoodET,
                             consecutiveFaultsET, r.etFault);
  return r;
}
