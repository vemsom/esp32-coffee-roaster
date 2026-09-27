// Host test for the calibration offset in src/sensors.cpp.
//
// The offsets are compile-time constants in include/config.h, so this binary
// is built with -DSENSOR_BT_OFFSET_C=-1.2 -DSENSOR_ET_OFFSET_C=1.6 and those
// defines are passed to BOTH translation units - the test file and
// sensors.cpp - because macros do not cross between them (run.sh passes the
// MQTT defines twice for the same reason). The two channels deliberately
// point in opposite directions: BT pulls the reading down, ET pushes it up,
// which is what lets one run cover both ways an offset can interact with the
// plausibility window.
//
// Proves the rule "judge the corrected value":
//   * a raw 24.3 with offset -1.2 reports 23.1
//   * a raw reading that is only invalid AFTER the offset is caught
//     (raw 2.5 -> 1.3, below SENSOR_MIN_VALID_C)
//   * a raw reading that would be rejected on its own but is fine corrected
//     is accepted (raw 0.7 -> 2.3), because the window has to judge the
//     number the firmware will act on
//   * NaN still trips after the offset is added (IEEE 754 propagates it)
//   * a fault holds the last CORRECTED value, never the raw one
//
// Readings are stepped toward their targets instead of jumped to: a real
// probe drifts, and a jump beyond SENSOR_FAULT_MAX_JUMP_C would make every
// scenario trip the jump detector on the way in and test the wrong thing.
// That bit the first version of this file, which is why the stepping exists.
#include <Arduino.h>
#include <max6675.h>
#include <cmath>
#include <cstdio>

#include "config.h"
#include "sensors.h"

// ---- probe readings injected into the MAX6675 stubs -------------------------
float g_probeBT = 20.0f;
float g_probeET = 20.0f;

// ---- minimal Arduino stubs the sensors module links against -----------------
void pinMode(int, int) {}
void digitalWrite(int, int) {}
int digitalRead(int) { return 0; }
unsigned long millis() { static unsigned long t = 0; return t += 100; }
void delay(unsigned long) {}
void delayMicroseconds(unsigned int) {}
long map(long x, long inMin, long inMax, long outMin, long outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}
void ledcSetup(int, double, int) {}
void ledcAttachPin(int, int) {}
void ledcWrite(int, int) {}
SerialStub Serial;

static int failures = 0;
static int checks = 0;

static void check(bool cond, const char *what) {
  checks++;
  if (cond) {
    printf("ok   %s\n", what);
  } else {
    printf("FAIL %s\n", what);
    failures++;
  }
}

static bool closeTo(float value, float expected) {
  return std::fabs(value - expected) < 0.01f;
}

// Steps a raw reading toward the target in increments the jump check accepts,
// calling sensors_read() for every step so lastGood follows along. `seed` is
// where to resume when the current reading is NaN - a detached probe hands
// over no number to step from.
static void stepTo(float &raw, float seed, float target) {
  if (isnan(raw)) raw = seed;
  for (int i = 0; i < 80 && !closeTo(raw, target); i++) {
    float d = target - raw;
    raw += (d > 15.0f ? 15.0f : (d < -15.0f ? -15.0f : d));
    sensors_read();
  }
}

int main() {
  // The compile-time contract itself: without the override on this binary
  // both would be 0.0 and every assertion below would collapse into a no-op.
  check(!closeTo(SENSOR_BT_OFFSET_C, 0.0f), "test build carries a non-zero BT offset");
  check(!closeTo(SENSOR_ET_OFFSET_C, 0.0f), "test build carries a non-zero ET offset");

  // --- the calibration arithmetic -------------------------------------------
  // Reference thermometer reads 23.1 C where the probe reports 24.3 raw.
  stepTo(g_probeBT, 20.0f, 24.3f);
  stepTo(g_probeET, 20.0f, 60.0f);
  SensorReading r = sensors_read();
  check(!r.btFault, "a raw reading inside the window after offset is not a fault");
  check(closeTo(r.bt, 23.1f), "offset -1.2 on raw 24.3 reports 23.1");

  // --- an offset must not be able to drag a healthy reading out -------------
  // Raw 2.5 is plausible on its own; with the BT offset of -1.2 it is 1.3,
  // below SENSOR_MIN_VALID_C. Judging the raw number would let this through,
  // and the rest of the firmware would never see it again.
  g_probeBT = 2.5f;
  r = sensors_read();
  check(r.btFault, "a reading that is only invalid AFTER the offset still trips");
  check(closeTo(r.bt, 23.1f), "the fault holds the last corrected value");

  // --- ...and an offset must not smuggle a dead one in ----------------------
  // Raw 0.7 is what a floating-low data line hands over - below the 2 C floor
  // on its own - and with the ET offset of +1.6 it reads 2.3, a legitimate
  // corrected value that is accepted. Same rule seen from the other side: the
  // window judges reality, not the raw number.
  g_probeBT = 24.3f;   // back to the calibrated reading; jump from lastGood is 0
  stepTo(g_probeET, 60.0f, 0.7f);
  r = sensors_read();
  check(!r.etFault, "a raw reading below the floor that is fine corrected passes");
  check(closeTo(r.et, 2.3f), "the accepted value is the corrected one, not the raw");

  // --- the window still applies on top of the offset ------------------------
  // Raw 405 is outside SENSOR_MAX_VALID_C even after +1.6, so the upper bound
  // is untouched by the correction - and the fault still reports a corrected
  // value that was good, never the one that failed.
  g_probeET = 405.0f;
  r = sensors_read();
  check(r.etFault, "the upper limit is still judged after the offset");
  check(closeTo(r.et, 2.3f), "upper-limit fault also holds the corrected value");

  // --- NaN must survive the arithmetic --------------------------------------
  // NaN + offset is NaN under IEEE 754, which is what keeps a detached probe
  // detected even now that the value is touched before it is judged.
  g_probeBT = NAN;
  r = sensors_read();
  check(r.btFault, "NaN still trips after the offset is added");

  // --- recovery ---------------------------------------------------------------
  g_probeBT = 24.3f;   // jump 0 from the held value, so the fault clears here
  r = sensors_read();
  check(!r.btFault, "a healthy reading clears the fault again");
  stepTo(g_probeBT, 24.3f, 100.0f);
  r = sensors_read();
  check(!r.btFault && closeTo(r.bt, 98.8f), "recovered reading is corrected too");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
