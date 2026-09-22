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
    delay(300);
}

static float readWithSanityCheck(MAX6675 &sensor, float &lastGood, int &faultCount, bool &faultOut) {
    float value = sensor.readCelsius();

  if (isnan(value)) {
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
  return r;
}

bool sensors_safety_triggered() {
    return consecutiveFaultsBT >= SENSOR_FAULT_MAX_COUNT ||
               consecutiveFaultsET >= SENSOR_FAULT_MAX_COUNT;
}
