// Host-side test of the Rate of Rise estimate (src/ror.cpp): the least-squares
// fit over the window, the warm-up span, the sign and the quantised tolerance,
// driven at the real control-loop cadence (SENSOR_READ_INTERVAL_MS) exactly
// the way main.cpp drives it.
//
// Tolerances and why they are what they are:
//
//   ramp / cooling / frozen   the samples lie exactly on a line, so the fit
//                             recovers that line and the only error left is
//                             float noise plus the one-decimal rounding of the
//                             published value: +-0.1 C/min.
//
//   quantised to 0.25 C       every reading is truncated onto the MAX6675's
//                             0.25 C ladder, so the series is a stair-case
//                             instead of a line. The documented tolerance for a
//                             30 s regression is +-0.3 C/min: as a single
//                             endpoint difference the ladder alone can cost
//                             +-0.25 C/min, and fitting ~120 samples instead of
//                             differencing two of them is what keeps that from
//                             reaching the answer. The test measures the worst
//                             case it can produce over minutes of ramp and
//                             asserts against +-0.3.
//
// The ramp runs for minutes on end, which wraps the 128-entry sample ring
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

// Feed up to and INCLUDING the given instant on the sample grid - the warm-up
// boundaries (9.75 s vs 10.00 s) are exactly one sample apart, so landing on
// the mark matters there.
static void runUntil(unsigned long absoluteMs) {
  while (nowMs <= absoluteMs) sample();
}

// Feed for `ms` and record the worst deviation from the expected rates, but
// only once `trackAfterMs` of history exists (before that the estimate is not
// supposed to be a full-window one yet).
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
  // Three windows of ramp; the 128-entry ring wraps twice over.
  runTracking(90000, 10.0f, 6.5f, ROR_WINDOW_MS, &worstBt, &worstEt);
  checkAtMost(worstBt, 0.1f, "constant 10 C/min ramp holds 10.0 over several windows");
  checkAtMost(worstEt, 0.1f, "constant 6.5 C/min ET ramp holds 6.5 over the same windows");
  checkClose(ror_get_bt(), 10.0f, 0.1f, "BT rate at the end of the run");
  checkClose(ror_get_et(), 6.5f, 0.1f, "ET rate at the end of the run");
}

static void testCooling() {
  startFresh(-5.0f, -3.0f);
  float worstBt = 0.0f, worstEt = 0.0f;
  runTracking(60000, -5.0f, -3.0f, ROR_WINDOW_MS, &worstBt, &worstEt);
  check(ror_get_bt() < 0.0f, "cooling keeps its negative sign");
  check(ror_get_et() < 0.0f, "environment cooling keeps its negative sign");
  checkAtMost(worstBt, 0.1f, "constant -5 C/min cooling holds -5");
  checkAtMost(worstEt, 0.1f, "constant -3 C/min environment cooling holds -3");
}

static void testQuantisation() {
  startFresh(10.0f, 0.0f);
  quantise = true;

  // The simulation has to be a stair-case and not a rounding: prove that the
  // ladder really truncates before trusting any tolerance it produces.
  float worstLadder = 0.0f;
  for (unsigned long t = 0; t < ROR_WINDOW_MS; t += SENSOR_READ_INTERVAL_MS) {
    float ideal = 20.0f + 10.0f * (float)t / 60000.0f;
    float off = ideal - std::floor(ideal / 0.25f) * 0.25f;
    if (off > worstLadder) worstLadder = off;
  }
  {
    char detail[64];
    snprintf(detail, sizeof(detail), "max %.3f C off the ideal line", worstLadder);
    if (worstLadder > 0.2f && worstLadder <= 0.25f) {
      pass("the simulated 0.25 C ladder really truncates the readings", detail);
    } else {
      fail("the simulated 0.25 C ladder really truncates the readings", detail);
    }
  }

  float worstBt = 0.0f, worstEt = 0.0f;
  runTracking(90000, 10.0f, 0.0f, ROR_WINDOW_MS, &worstBt, &worstEt);
  // Documented tolerance for a quantised signal over a 30 s regression:
  // +-0.3 C/min (see the header comment). The flat ET channel is the control:
  // the same ladder with nothing behind it must stay at 0.
  checkAtMost(worstBt, 0.3f, "0.25 C-quantised ramp holds the documented +-(0.3) C/min");
  checkAtMost(worstEt, 0.3f, "0.25 C-quantised flat ET stays inside the same tolerance");

  // A slow ramp is the hard case for the ladder: fewer steps per window, so
  // the stair-case has less of a say in a fit over the same 30 s. The
  // tolerance must hold there too - that is where a roast actually spends its
  // first minutes.
  startFresh(2.0f, 0.0f);
  quantise = true;
  worstBt = 0.0f;
  worstEt = 0.0f;
  runTracking(90000, 2.0f, 0.0f, ROR_WINDOW_MS, &worstBt, &worstEt);
  checkAtMost(worstBt, 0.3f, "0.25 C-quantised slow 2 C/min ramp holds the same tolerance");
}

static void testWarmUp() {
  // Under the minimum span: publish 0, not a wild number.
  startFresh(10.0f, 6.5f);
  runUntil(5000);
  checkClose(ror_get_bt(), 0.0f, 0.0f, "5 s of history publishes 0 (below ROR_MIN_SPAN_MS)");
  checkClose(ror_get_et(), 0.0f, 0.0f, "5 s of history publishes 0 for ET too");

  // One sample below the minimum span, then exactly at it.
  runUntil(9750);
  checkClose(ror_get_bt(), 0.0f, 0.0f, "9.75 s of history still publishes 0");
  runUntil(10000);
  checkClose(ror_get_bt(), 10.0f, 0.1f, "10 s of history (the minimum span) is fitted over that span");

  // Short of a full window: fit the span that exists, which is already C/min.
  startFresh(10.0f, 6.5f);
  runUntil(20000);
  checkClose(ror_get_bt(), 10.0f, 0.1f, "20 s of history is fitted over the available span");
  checkClose(ror_get_et(), 6.5f, 0.1f, "20 s of history gives the right ET rate too");
}

static void testFrozenSensor() {
  startFresh(0.0f, 0.0f);
  runUntil(ROR_WINDOW_MS + 5000);
  check(std::fabs(ror_get_bt()) < 0.1f, "frozen BT reads below 0.1 C/min after a window");
  check(std::fabs(ror_get_et()) < 0.1f, "frozen ET reads below 0.1 C/min after a window");
}

static void testNanHistory() {
  // A probe that has never read: NaN samples. They are left out of the fit -
  // never fitted as if the temperature were 0 - and the series starts
  // reporting as soon as there is enough REAL data behind it.
  ror_reset();
  nowMs = 0;
  for (int i = 0; i < 8; i++) {          // 2 s of NaN
    ror_update(nowMs, NAN, NAN);
    nowMs += SENSOR_READ_INTERVAL_MS;
  }
  checkClose(ror_get_bt(), 0.0f, 0.0f, "NaN readings publish 0, not a number");
  rateBt = rateEt = 10.0f;
  runUntil(14000);                       // 12 s of real ramp behind it
  checkClose(ror_get_bt(), 10.0f, 0.1f,
             "NaN samples are left out of the fit, not fitted as zero");
}

int main() {
  printf("build constants: ROR_WINDOW_MS=%d ROR_MIN_SPAN_MS=%d ROR_BUFFER_LEN=%d "
         "sample=%d ms (%d samples per window)\n",
         ROR_WINDOW_MS, ROR_MIN_SPAN_MS, ROR_BUFFER_LEN, SENSOR_READ_INTERVAL_MS,
         ROR_WINDOW_MS / SENSOR_READ_INTERVAL_MS + 1);

  testConstantRamp();
  testCooling();
  testQuantisation();
  testWarmUp();
  testFrozenSensor();
  testNanHistory();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
