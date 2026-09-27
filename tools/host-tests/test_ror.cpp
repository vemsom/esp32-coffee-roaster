// Host-side test of the Rate of Rise maths (src/ror.cpp): the endpoint delta,
// the sliding window, the warm-up behaviour and the sign, driven at the real
// control-loop cadence (SENSOR_READ_INTERVAL_MS) exactly the way main.cpp
// drives it.
//
// Tolerances and why they are what they are:
//
//   ramp / cooling / frozen   the maths is exact over a known interval and the
//                             result is rounded to one decimal, so +-0.1
//                             C/min is the whole tolerance (the rounding).
//
//   quantised to 0.25 C       each endpoint can sit up to 0.25 C off the true
//                             temperature (the MAX6675 lsb truncates, it does
//                             not round), so the delta - and with it the rate -
//                             can be off by up to 0.25 C over a 60 s window.
//                             The suite therefore documents +-0.5 C/min for a
//                             quantised signal: twice the worst case the
//                             0.25 C ladder can produce.
//
// The ramp is run for minutes on end, which wraps the 64-entry history ring
// several times - a ring that only works until it is full would fail here.
#include <cmath>
#include <cstdio>

#include "config.h"
#include "ror.h"

static int checks = 0;
static int failures = 0;

static void pass(const char *what, const char *detail) {
  checks++;
  printf("ok   %s (%s)\n", what, detail);
}

static void fail(const char *what, const char *detail) {
  checks++;
  printf("FAIL %s (%s)\n", what, detail);
  failures++;
}

static void check(bool cond, const char *what) {
  checks++;
  if (cond) printf("ok   %s\n", what);
  else {
    printf("FAIL %s\n", what);
    failures++;
  }
}

static void checkClose(float got, float want, float tol, const char *what) {
  char detail[96];
  snprintf(detail, sizeof(detail), "got %.3f, want %.3f +-%.3f", got, want, tol);
  if (std::fabs(got - want) <= tol) pass(what, detail);
  else fail(what, detail);
}

static void checkAtMost(float value, float limit, const char *what) {
  char detail[96];
  snprintf(detail, sizeof(detail), "worst %.3f, limit %.3f", value, limit);
  if (value <= limit) pass(what, detail);
  else fail(what, detail);
}

// ---------------------------------------------------------------------------
// Feeder: one ror_update() per control-loop sample, clock advancing by
// SENSOR_READ_INTERVAL_MS, both probes on a straight ramp of rateBt / rateEt
// C/min starting at 20 C. quantise() drops the readings onto the MAX6675's
// 0.25 C ladder by truncation, the way the converter actually reports.
// ---------------------------------------------------------------------------
static unsigned long nowMs = 0;
static float rateBt = 0.0f;
static float rateEt = 0.0f;
static bool quantise = false;

static float at(unsigned long tMs, float rate) {
  float value = 20.0f + rate * (float)tMs / 60000.0f;
  if (quantise) value = std::floor(value / 0.25f) * 0.25f;
  return value;
}

static void sample() {
  ror_update(nowMs, at(nowMs, rateBt), at(nowMs, rateEt));
  nowMs += SENSOR_READ_INTERVAL_MS;
}

static void runMs(unsigned long ms) {
  unsigned long end = nowMs + ms;
  while (nowMs < end) sample();
}

// Feed up to and INCLUDING the given instant on the sample grid - the warm-up
// boundaries (14.75 s vs 15.00 s) are exactly one sample apart, so landing on
// the mark matters there.
static void runUntil(unsigned long absoluteMs) {
  while (nowMs <= absoluteMs) sample();
}

// Feed for `ms` and record the worst deviation from the expected rates, but
// only once `trackAfterMs` of history exists (before that the rate is not
// supposed to be a full-window rate yet).
static void runTracking(unsigned long ms, float wantBt, float wantEt,
                        unsigned long trackAfterMs,
                        float *worstBt, float *worstEt) {
  unsigned long end = nowMs + ms;
  while (nowMs < end) {
    sample();
    if (nowMs <= trackAfterMs) continue;
    float errBt = std::fabs(ror_get_bt() - wantBt);
    float errEt = std::fabs(ror_get_et() - wantEt);
    if (errBt > *worstBt) *worstBt = errBt;
    if (errEt > *worstEt) *worstEt = errEt;
  }
}

static void startFresh(float btRate, float etRate) {
  ror_reset();
  nowMs = 0;
  rateBt = btRate;
  rateEt = etRate;
  quantise = false;
}

// ---------------------------------------------------------------------------

static void testConstantRamp() {
  startFresh(10.0f, 6.5f);
  float worstBt = 0.0f, worstEt = 0.0f;
  // Three minutes = three full windows; the history ring wraps twice over.
  runTracking(180000, 10.0f, 6.5f, ROR_WINDOW_MS, &worstBt, &worstEt);
  checkAtMost(worstBt, 0.1f, "constant 10 C/min ramp holds 10.0 over several windows");
  checkAtMost(worstEt, 0.1f, "constant 6.5 C/min ET ramp holds 6.5 over the same windows");
  checkClose(ror_get_bt(), 10.0f, 0.1f, "BT rate at the end of the run");
  checkClose(ror_get_et(), 6.5f, 0.1f, "ET rate at the end of the run");
}

static void testCooling() {
  startFresh(-5.0f, -3.0f);
  float worstBt = 0.0f, worstEt = 0.0f;
  runTracking(120000, -5.0f, -3.0f, ROR_WINDOW_MS, &worstBt, &worstEt);
  check(ror_get_bt() < 0.0f, "cooling keeps its negative sign");
  check(ror_get_et() < 0.0f, "environment cooling keeps its negative sign");
  checkAtMost(worstBt, 0.1f, "constant -5 C/min cooling holds -5");
  checkAtMost(worstEt, 0.1f, "constant -3 C/min environment cooling holds -3");
}

static void testQuantisation() {
  startFresh(10.0f, 0.0f);
  quantise = true;
  float worstBt = 0.0f, worstEt = 0.0f;
  runTracking(180000, 10.0f, 0.0f, ROR_WINDOW_MS, &worstBt, &worstEt);
  // Documented tolerance for a quantised signal: +-0.5 C/min, twice the 0.25 C
  // ladder can contribute over a 60 s window.
  checkAtMost(worstBt, 0.5f, "0.25 C-quantised ramp holds the documented +-(0.5) C/min");
  checkAtMost(worstEt, 0.5f, "0.25 C-quantised flat ET stays inside the same tolerance");
}

static void testWarmUp() {
  // Under the minimum span: publish 0, not a wild number.
  startFresh(10.0f, 6.5f);
  runUntil(5000);
  checkClose(ror_get_bt(), 0.0f, 0.0f, "5 s of history publishes 0 (below ROR_MIN_SPAN_MS)");
  checkClose(ror_get_et(), 0.0f, 0.0f, "5 s of history publishes 0 for ET too");

  // One sample below the minimum span, then exactly at it.
  runUntil(14750);
  checkClose(ror_get_bt(), 0.0f, 0.0f, "14.75 s of history still publishes 0");
  runUntil(15000);
  checkClose(ror_get_bt(), 10.0f, 0.1f, "15 s of history (the minimum span) is measured over that span");

  // The case the spec calls out: a fraction of a window of data.
  startFresh(10.0f, 6.5f);
  runUntil(20000);
  checkClose(ror_get_bt(), 10.0f, 0.1f, "20 s of history is measured over the available span");
  checkClose(ror_get_et(), 6.5f, 0.1f, "20 s of history gives the right ET rate too");
}

static void testFrozenSensor() {
  startFresh(0.0f, 0.0f);
  runMs(ROR_WINDOW_MS + 5000);
  check(std::fabs(ror_get_bt()) < 0.1f, "frozen BT reads below 0.1 C/min after a window");
  check(std::fabs(ror_get_et()) < 0.1f, "frozen ET reads below 0.1 C/min after a window");
}

static void testNanHistory() {
  // A probe that has never read: NaN at both ends. That must not become a
  // rate - neither 0-as-if-measured while the reference is NaN, nor a spike.
  ror_reset();
  nowMs = 0;
  for (int i = 0; i < 8; i++) {          // 2 s of NaN
    ror_update(nowMs, NAN, NAN);
    nowMs += SENSOR_READ_INTERVAL_MS;
  }
  checkClose(ror_get_bt(), 0.0f, 0.0f, "NaN readings publish 0, not a number");
  rateBt = rateEt = 10.0f;
  runMs(28000);                          // 30 s total: reference is still NaN
  checkClose(ror_get_bt(), 0.0f, 0.0f, "a NaN reference does not turn into a fake rate");
  runMs(40000);                          // 70 s total: reference is valid now
  checkClose(ror_get_bt(), 10.0f, 0.1f, "once the reference is a real reading the ramp is measured");
}

int main() {
  printf("build constants: ROR_WINDOW_MS=%d ROR_MIN_SPAN_MS=%d "
         "ROR_SNAPSHOT_INTERVAL_MS=%d sample=%d ms\n",
         ROR_WINDOW_MS, ROR_MIN_SPAN_MS, ROR_SNAPSHOT_INTERVAL_MS,
         SENSOR_READ_INTERVAL_MS);

  testConstantRamp();
  testCooling();
  testQuantisation();
  testWarmUp();
  testFrozenSensor();
  testNanHistory();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
