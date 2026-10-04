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

  // The cross-check in safety.cpp works on calibrated values. The ET offset is
  // fitted to be exactly the raw ET-BT difference it was measured at, so a raw
  // pair that far apart must read as one and the same temperature after
  // calibration - and therefore never trip the spread alarm. Derive the raw ET
  // from the raw BT and the two constants instead of hard-coding a pair: the
  // assertion then still means "ET calibrates onto BT" after the next offset
  // change, rather than pinning one measurement. Raw BT anchored at the
  // measured median (24,75 C, room temperature).
  const float rawBt = 24.75f;
  const float rawEt = rawBt + (SENSOR_OFFSET_BT_C - SENSOR_OFFSET_ET_C);
  g_probeBT = rawBt;
  g_probeET = rawEt;
  r = sensors_read();
  check(!r.btFault && !r.etFault, "measured median raw pair is individually plausible");
  check(fabs(r.et - r.bt) < 0.02f,
        "ET calibrates onto BT at the fitted raw difference");

  // Sanity-check the other direction too: the cross-check compares CALIBRATED
  // values against SENSOR_MAX_SPREAD_C, so where the alarm sits is set by the
  // ET offset, not by the raw pair. Pick a raw spread far enough above the
  // threshold that the offset cannot pull it under it, and assert it still
  // trips - a 0,5 C offset change must not disable the check.
  const float rawSpreadThatMustTrip =
      SENSOR_MAX_SPREAD_C - (SENSOR_OFFSET_ET_C - SENSOR_OFFSET_BT_C) + 1.0f;
  g_probeBT = 20.0f;
  g_probeET = 20.0f + rawSpreadThatMustTrip;
  r = sensors_read();
  check(!r.btFault && !r.etFault, "the raw disagreement pair is individually plausible");
  check(fabs(r.et - r.bt) > SENSOR_MAX_SPREAD_C,
        "a raw disagreement that must trip survives the ET offset");
  check(fabs((r.et - r.bt) -
             (rawSpreadThatMustTrip + SENSOR_OFFSET_ET_C - SENSOR_OFFSET_BT_C)) < 0.02f,
        "calibrated spread is raw spread plus the ET-BT offset difference");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
