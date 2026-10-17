// Host-side test of the sensor layer: the offset, the fault masking, and which
// converters are read at all.
// Build: see run.sh. It is compiled and run in BOTH build modes
// (SAFETY_ET_ENABLED 1 and 0). The BT checks are mode-independent and have to
// hold either way; the ET half is the default build's behaviour (ET is read,
// calibrated and faulted as documented), and the flag-0 branch asserts what
// replaces it - the ET converter is not read, so the channel carries no value
// and no fault, whatever the module does.
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

  // ================= the BT channel: identical in both builds ==============
  // Baseline: equal raw readings with equal calibration on BT.
  g_probeBT = 25.0f;
  g_probeET = 25.0f;
  SensorReading r = sensors_read();
  check(!r.btFault, "a plausible BT reading is not faulted");
  check(r.bt == 25.0f + SENSOR_OFFSET_BT_C, "BT reported with BT offset applied");

  // 0 C on BT is a fault, not a cold roaster: a floating data line reads a
  // steady zero, which is exactly what a bench machine with nothing wired does.
  // The faulted channel hands back its last good value - with the offset NOT
  // applied to it.
  g_probeBT = 0.0f;
  r = sensors_read();
  check(r.btFault, "0 C BT is a fault (the floating-data-line case)");
  check(r.bt == 25.0f, "a faulted BT returns last good value, offset not applied");

  // A step over the jump limit is a fault; the step is pinned to the constant,
  // so the boundary cannot drift unnoticed.
  g_probeBT = 25.0f;
  (void)sensors_read();  // re-establish the last good value
  g_probeBT = 25.0f + SENSOR_FAULT_MAX_JUMP_C + 0.25f;
  r = sensors_read();
  check(r.btFault, "a BT step above SENSOR_FAULT_MAX_JUMP_C is a fault");
  g_probeBT = 25.0f;
  r = sensors_read();
  check(!r.btFault, "and a plausible value right after it is accepted again");

  // Which converters are read at all. This is the one property that separates a
  // BT-only build from a build that merely has a dead ET module: a dead module
  // is still read and reports a fault, a switched-off channel is not read. It
  // cannot be seen in any value - both end up as null over the API - so it is
  // counted in the stub instead.
  const int etReadsBefore = g_max6675EtReads;
  (void)sensors_read();
  (void)sensors_read();
#if SAFETY_ET_ENABLED
  check(g_max6675EtReads == etReadsBefore + 2,
        "the default build reads the ET converter on every sample");
#else
  check(g_max6675EtReads == etReadsBefore,
        "a BT-only build never reads the ET converter");
#endif

#if SAFETY_ET_ENABLED
  // ================= default build: ET is read and calibrated ==============
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

#else
  // ================= BT-only build: ET carries nothing =====================
  // What the ET module does is now irrelevant: the same sample is produced
  // whether it reads a sane value, an absurd one, or nothing at all. The
  // channel reports NaN - which is what a dead ET module already reports over
  // /api/status - and no fault, because there is no detector left that a fault
  // could feed.
  g_probeBT = 25.0f;
  g_probeET = 0.0f;  // what a floating ET data line gives
  r = sensors_read();
  check(!r.btFault, "BT stays healthy while the ET channel is off");
  check(!r.etFault, "the ET channel reports no fault: it is not watched");
  check(std::isnan(r.et), "and no value either (NaN), like a dead module");
  check(r.bt == 25.0f + SENSOR_OFFSET_BT_C, "the BT offset is unaffected by the flag");

  g_probeET = -99.0f;
  r = sensors_read();
  check(std::isnan(r.et) && !r.etFault,
        "an absurd ET reading changes nothing: the converter is not read");

  g_probeET = NAN;
  r = sensors_read();
  check(std::isnan(r.et) && !r.etFault, "nor does an open ET probe");

  // The module could also be disconnected at power-up, which a full build sees
  // as a steady 0 C with nothing to jump from - the case the 2 C floor exists
  // for. It cannot reach the reading here at all.
  g_probeET = 0.0f;
  for (int i = 0; i < SENSOR_FAULT_MAX_COUNT * 2; i++) (void)sensors_read();
  r = sensors_read();
  check(!r.btFault && !r.etFault && std::isnan(r.et),
        "a steady 0 C from the ET module cannot fault the sample either");
#endif

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
