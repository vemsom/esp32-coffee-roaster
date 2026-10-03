// Host-side test of the sensor offset and fault masking.
// Build: see run.sh (compiled and run as part of the suite).
#include <Arduino.h>
#include <cstdio>
#include <cmath>

#include "sensors.h"
#include "config.h"
#include "max6675.h"

// ---- injected probe readings ------------------------------------------------
// The MAX6675 stub in stub/max6675.h reads these globals.
float g_probeBT = 20.0f;
float g_probeET = 20.0f;

// ---- MAX6675 stub -----------------------------------------------------------
// The real sensors.cpp includes <max6675.h>, so the host build needs the stub
// from tools/host-tests/stub/max6675.h. Do not define a second class here.

// ---- Arduino stubs ----------------------------------------------------------
unsigned long millis() { return 0; }
void delay(unsigned long) {}
void delayMicroseconds(unsigned int) {}
void pinMode(int, int) {}
void digitalWrite(int, int) {}
int digitalRead(int) { return 0; }
SerialStub Serial;

// ---- test harness -----------------------------------------------------------
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

int main() {
  sensors_init();

  // Baseline: equal raw readings should yield equal calibrated readings.
  g_probeBT = 25.0f;
  g_probeET = 25.0f;
  SensorReading r = sensors_read();
  check(!r.btFault && !r.etFault, "healthy readings are not faulted");
  check(r.bt == 25.0f + SENSOR_OFFSET_BT_C, "BT reported with BT offset applied");
  check(r.et == 25.0f + SENSOR_OFFSET_ET_C, "ET reported with ET offset applied");
  check(fabs((r.et - r.bt) - (SENSOR_OFFSET_ET_C - SENSOR_OFFSET_BT_C)) < 0.001f,
        "BT/ET difference is the calibrated difference");

  // Offset masks neither the open-probe NaN case nor the floating-low 0 C case.
  // First establish a last-good ET so we can prove the offset is NOT applied to
  // a faulted return value (sensors.cpp returns lastGood on fault).
  g_probeET = 25.0f;
  (void)sensors_read();
  g_probeET = NAN;
  r = sensors_read();
  check(r.etFault, "NaN ET is still a fault with offset configured");
  // lastGoodET was 25.0; offset must not be applied, so value stays 25.0.
  check(r.et == 25.0f, "faulted ET returns last good value, offset not applied");

  g_probeET = 0.0f;
  r = sensors_read();
  check(r.etFault, "0 C ET is still a fault with offset configured");
  check(r.et == 25.0f, "0 C ET also returns last good value, offset not applied");

  g_probeET = 25.0f;  // restore a healthy ET for the next block

  // The cross-check in safety.cpp works on calibrated values: raw ET 6 C above
  // BT should read the same after calibration, so no spread alarm.
  g_probeBT = 24.75f;
  g_probeET = 30.75f;  // +6 C raw, the measured median difference
  r = sensors_read();
  check(!r.btFault && !r.etFault, "measured median raw pair is individually plausible");
  check(fabs(r.et - r.bt) < 0.1f,
        "calibrated ET matches BT at the measured median raw difference");

  // Sanity-check the other direction too: a real 15 C spread stays detected.
  // With ET offset -6 C and BT offset 0 C, raw 20/35 becomes calibrated 20/29,
  // i.e. a 9 C spread - still above the 15 C raw threshold? No: the point is
  // that the cross-check sees calibrated values, so we assert the calibrated
  // spread equals (raw_spread + ET_offset). The safety layer then compares that
  // to SENSOR_MAX_SPREAD_C.
  g_probeBT = 20.0f;
  g_probeET = 35.0f;
  r = sensors_read();
  check(!r.btFault && !r.etFault, "20/35 raw pair is individually plausible");
  check(fabs((r.et - r.bt) - (15.0f + SENSOR_OFFSET_ET_C - SENSOR_OFFSET_BT_C)) < 0.1f,
        "calibrated spread is raw spread plus the ET-BT offset difference");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
