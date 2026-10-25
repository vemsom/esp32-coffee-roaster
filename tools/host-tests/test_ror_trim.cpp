// Host-side check of the RoR guidance as a SETPOINT trim (0.16.0):
// include/ror_trim.h, its dead band, clamp and slew limit, and a simulated
// before/after over a plant, fed by the error sequence measured on a real roast.
// Pure logic: include/pid.h + include/filter.h + include/feed_forward.h +
// include/config.h + the real src/ror.cpp estimator over a stubbed clock - no
// Arduino hardware, no other translation unit.
//
// WHY THE OUTPUT MOVED (from the machine, 2026-10-25 10:36-10:47, 60 g, profile
// `monsun-choklad`, rorTarget 36.5 -> 2.4 C/min):
//
//   | t (s) |  BT   | profile target | deviation |
//   |    40 |  60.2 |         48.3   |     +11.9 |
//   |   200 | 107.3 |        134.3   |     -27.0 |
//   |   480 | 209.5 |        195.5   |     +14.0 |
//   |   612 | 219.0 |        ~206    |     +13   |
//
// duty spent 5 % of the roast at 0 % and 11 % at >= 99 %, the 30 s slope swung
// -36..+53 C/min, and the panel showed guidance 22.6 / measured 60.9 / heat 0 %
// at 2:47 against guidance 14.4 / measured -52.4 / heat 100 % at 4:09.
//
// The old law was correction = (rorTarget - rorActual) * 3.0 %/(C/min), clipped
// to +-100 % and slewed 15 %/s, ADDED to the PID's duty. Every error past
// 33 C/min therefore saturated it, and this roast ran at -38 and +67 C/min: two
// regulators with integral action on one actuator, the outer one fed a
// derivative. The new law (include/ror_trim.h) makes the same error a setpoint
// trim of at most +-3 C, which the PID absorbs.
//
// WHAT THIS FILE DOES NOT CLAIM. The plant below is the first-order-plus-dead-
// time model identified in test_pid_tuning.cpp / test_feed_forward.cpp
// (K 2.0 C/%, tau 45 s, L 19 s), not a re-identification from the 2026-10-25
// roast: the duty trace of that roast is not in the log, only the temperatures
// and the 30 s slope. What the simulation buys is the SHAPE of the two
// controllers on one plant - the before leg reproducing the 0 %/100 % slamming,
// the after leg not - with the real PID, the real feed-forward and the real RoR
// estimator.
#include <Arduino.h>
#include <cstdio>
#include <cmath>
#include <vector>

#include "config.h"
#include "filter.h"
#include "pid.h"
#include "feed_forward.h"
#include "ror.h"
#include "ror_trim.h"

// ---- stubbed clock ----------------------------------------------------------
static unsigned long fakeMillis = 0;
unsigned long millis() { return fakeMillis; }

static const double kDt = SENSOR_READ_INTERVAL_MS / 1000.0;  // 0.25 s

// ---- test bookkeeping -------------------------------------------------------
static int checks = 0;
static int failures = 0;

static void report(bool ok, const char *what, const char *detail) {
  checks++;
  if (ok) {
    printf("ok   %s (%s)\n", what, detail);
  } else {
    printf("FAIL %s (%s)\n", what, detail);
    failures++;
  }
}

static bool near(float got, float want, float tol) { return fabsf(got - want) <= tol; }

// ---------------------------------------------------------------------------
// 1. The law itself: dead band, gain, clamp, sign, and the NaN/disabled cases.
// ---------------------------------------------------------------------------
static void testTrimLaw() {
  char detail[200];

  // Inside the dead band the trim is EXACTLY 0 - not a small number. Both edges
  // and the middle, both signs, and a zero error.
  report(rorTrimRequestC(5.0f, 5.0f) == 0.0f, "err 0 -> trim 0", "5.0 vs 5.0");
  report(rorTrimRequestC(5.0f, 3.5f) == 0.0f, "err +1.5 (dead-band edge) -> trim 0", "5.0 vs 3.5");
  report(rorTrimRequestC(5.0f, 6.5f) == 0.0f, "err -1.5 (dead-band edge) -> trim 0", "5.0 vs 6.5");
  report(rorTrimRequestC(5.0f, 4.0f) == 0.0f, "err +1.0 (inside) -> trim 0", "5.0 vs 4.0");
  report(rorTrimRequestC(5.0f, 6.0f) == 0.0f, "err -1.0 (inside) -> trim 0", "5.0 vs 6.0");

  // 0.1 C per (C/min). err 20 (the task's case) -> 2.0 C.
  const float at20 = rorTrimRequestC(22.6f, 2.6f);
  snprintf(detail, sizeof(detail), "err %.1f -> trim %+.3f C (want %+.1f)",
           20.0f, at20, ROR_TRIM_GAIN_C_PER_C_PER_MIN * 20.0f);
  report(near(at20, 2.0f, 0.001f), "err 20 C/min -> 2.0 C of trim", detail);

  // Symmetry: a too-fast roast pulls the setpoint down by the same amount.
  const float slowed = rorTrimRequestC(14.4f, 14.4f + 20.0f);
  snprintf(detail, sizeof(detail), "err %.1f -> trim %+.3f C", -20.0f, slowed);
  report(near(slowed, -2.0f, 0.001f), "err -20 C/min -> -2.0 C of trim", detail);

  // The clamp, both directions, with the errors this roast actually ran:
  // -38 C/min (would be -3.8 C unclamped) and +67 C/min (+6.7 C unclamped).
  const float at38 = rorTrimRequestC(22.6f, 60.9f);
  const float at67 = rorTrimRequestC(14.4f, -52.4f);
  snprintf(detail, sizeof(detail), "err -38.3 -> %+.3f C, err +66.8 -> %+.3f C (max %.1f)",
           at38, at67, ROR_TRIM_MAX_C);
  report(near(at38, -ROR_TRIM_MAX_C, 0.001f) && near(at67, ROR_TRIM_MAX_C, 0.001f),
         "the two measured errors both clip to +-ROR_TRIM_MAX_C", detail);

  // An error of 1000 C/min clips too - there is no way to ask for more.
  report(near(rorTrimRequestC(1000.0f, 0.0f), ROR_TRIM_MAX_C, 0.001f),
         "even a 1000 C/min error requests only ROR_TRIM_MAX_C", "1000 -> +3.0 C");

  // "Off" is 0, not a default: rorTarget <= 0 means the step asked for no
  // guidance at all.
  report(rorTrimRequestC(0.0f, -20.0f) == 0.0f, "rorTarget 0 -> trim 0", "0 vs -20");
  report(rorTrimRequestC(-5.0f, 20.0f) == 0.0f, "negative rorTarget -> trim 0", "-5 vs 20");

  // A NaN on either side (no reading, an estimate that has not warmed up) is a
  // trim of 0, never a NaN setpoint.
  const float nan = rorTrimRequestC(NAN, 10.0f);
  const float nan2 = rorTrimRequestC(10.0f, NAN);
  report(nan == 0.0f && nan2 == 0.0f, "NaN on either side -> trim 0, never a NaN setpoint",
         "NaN vs 10, 10 vs NaN");
}

// ---------------------------------------------------------------------------
// 2. The slew limit: 0.5 C/s on a 250 ms cadence is 0.125 C per cycle.
// ---------------------------------------------------------------------------
static void testSlew() {
  const float perCycle = ROR_TRIM_SLEW_C_PER_S * (SENSOR_READ_INTERVAL_MS / 1000.0f);
  char detail[200];

  snprintf(detail, sizeof(detail), "%.4f C per %d ms cycle (0.5 C/s)",
           perCycle, (int)SENSOR_READ_INTERVAL_MS);
  report(near(slewLimitedStep(3.0f, SENSOR_READ_INTERVAL_MS, ROR_TRIM_SLEW_C_PER_S),
              perCycle, 0.0001f),
         "a full +3 C step is limited to one cycle's worth", detail);

  snprintf(detail, sizeof(detail), "%.4f C for a -.02 C request",
           slewLimitedStep(-0.02f, SENSOR_READ_INTERVAL_MS, ROR_TRIM_SLEW_C_PER_S));
  report(near(slewLimitedStep(-0.02f, SENSOR_READ_INTERVAL_MS, ROR_TRIM_SLEW_C_PER_S),
              -0.02f, 0.0001f),
         "a request inside the limit passes through unchanged", detail);

  report(slewLimitedStep(-3.0f, SENSOR_READ_INTERVAL_MS, ROR_TRIM_SLEW_C_PER_S) < 0.0f,
         "a negative request is limited in the other direction too", "-3 C -> -0.125 C");
  report(slewLimitedStep(3.0f, 0, ROR_TRIM_SLEW_C_PER_S) == 0.0f,
         "a zero dt cannot unlock a step", "dt 0 -> 0");

  // A full +-3 C swing therefore takes 6 s, which is the "soft start" the
  // constant is for: 24 cycles at 0.125 C.
  float trim = 0.0f;
  int cycles = 0;
  while (trim < ROR_TRIM_MAX_C - 0.0001f && cycles < 1000) {
    trim += slewLimitedStep(ROR_TRIM_MAX_C - trim, SENSOR_READ_INTERVAL_MS, ROR_TRIM_SLEW_C_PER_S);
    cycles++;
  }
  snprintf(detail, sizeof(detail), "%d cycles = %.1f s for the full %.1f C",
           cycles, cycles * kDt, ROR_TRIM_MAX_C);
  report(near((float)(cycles * kDt), ROR_TRIM_MAX_C / ROR_TRIM_SLEW_C_PER_S, 0.3f),
         "the full swing takes ROR_TRIM_MAX_C / ROR_TRIM_SLEW_C_PER_S seconds", detail);
}

// ---------------------------------------------------------------------------
// 3. The roast the measurements came from, as a curve.
// ---------------------------------------------------------------------------
// Profile target, reconstructed from the logged points (0 s assumed at the 20 C
// charge; the four measured ones are exact): a hard first ramp through the
// drying phase, then a slower one, then the approach to first crack.
struct CurvePoint {
  unsigned long seconds;
  float temp;
};
static const CurvePoint kCurve[] = {
  { 0, 20.0f }, { 40, 48.3f }, { 200, 134.3f }, { 480, 195.5f }, { 612, 206.0f },
};
static const unsigned long kEndSeconds = 612;

static float measuredCurveAt(unsigned long t) {
  if (t <= kCurve[0].seconds) return kCurve[0].temp;
  const int n = (int)(sizeof(kCurve) / sizeof(kCurve[0]));
  for (int i = 1; i < n; i++) {
    if (t <= kCurve[i].seconds) {
      const float span = (float)(kCurve[i].seconds - kCurve[i - 1].seconds);
      const float frac = (float)(t - kCurve[i - 1].seconds) / span;
      return kCurve[i - 1].temp + (kCurve[i].temp - kCurve[i - 1].temp) * frac;
    }
  }
  return kCurve[n - 1].temp;
}

// The profile's RoR target for that roast: 36.5 C/min at the start, 2.4 C/min at
// the end. The steps' own interpolation is not in the log, so the simulation
// uses a straight line between the two ends - the worst case for the trim, since
// the early target is the one the roast misses by the most.
static float rorTargetAt(unsigned long t) {
  if (t >= kEndSeconds) return 2.4f;
  const float frac = (float)t / (float)kEndSeconds;
  return 36.5f + (2.4f - 36.5f) * frac;
}

// ---------------------------------------------------------------------------
// 4. Simulated before/after: one plant, the old duty offset against the new
//    setpoint trim.
// ---------------------------------------------------------------------------
// First-order lag plus dead time, the identified plant (see the file header).
struct Plant {
  double K, tau, ambient, temp;
  std::vector<double> delay;
  size_t head = 0;

  Plant(double k, double tau_s, double deadTimeS)
      : K(k), tau(tau_s), ambient(FF_AMBIENT_C), temp(kCurve[0].temp) {
    size_t len = (size_t)(deadTimeS / kDt + 0.5);
    if (len < 1) len = 1;
    delay.assign(len, 0.0);
  }

  void step(double duty) {
    const double applied = delay[head];
    delay[head] = duty;
    head = (head + 1) % delay.size();
    temp += kDt / tau * (ambient + K * applied - temp);
  }
};

// The OLD law, verbatim from src/main.cpp before 0.16.0 (commit ae58ce6):
//
//   float rawCorrection = error * ROR_GUIDANCE_GAIN;                  // 3.0
//   delta = rateLimitDelta(rawCorrection - correction, dtMs);         // 15 %/s
//   correction = correction + delta;  clipped to +-HEATER_MAX_DUTY_PCT
//   duty = ffDuty + pidDuty + correction;  clipped to 0..100
//
// It is here as the control group only - nothing in the firmware runs it.
struct OldGuidance {
  static constexpr float kGain = 3.0f;          // ROR_GUIDANCE_GAIN, pre-0.16.0
  static constexpr float kStepPerSec = 15.0f;   // ROR_GUIDANCE_MAX_STEP_PCT_PER_S
  float correction = 0.0f;

  float apply(float error, bool enabled) {
    if (!enabled) {
      correction = 0.0f;
      return 0.0f;
    }
    const float raw = error * kGain;
    const float limit = kStepPerSec * (SENSOR_READ_INTERVAL_MS / 1000.0f);
    float delta = raw - correction;
    if (delta > limit) delta = limit;
    if (delta < -limit) delta = -limit;
    correction += delta;
    if (correction > HEATER_MAX_DUTY_PCT) correction = HEATER_MAX_DUTY_PCT;
    if (correction < -HEATER_MAX_DUTY_PCT) correction = -HEATER_MAX_DUTY_PCT;
    return correction;
  }
};

// The NEW law: the same error through include/ror_trim.h, slewed into a setpoint
// trim, and folded into the setpoint the PID chases - exactly what
// updateRorGuidance()/updateControl() do in src/main.cpp.
struct NewGuidance {
  float trim = 0.0f;

  float apply(float rorTarget, float rorActual, bool enabled) {
    if (!enabled) {
      trim = 0.0f;
      return 0.0f;
    }
    const float request = rorTrimRequestC(rorTarget, rorActual);
    trim += slewLimitedStep(request - trim, SENSOR_READ_INTERVAL_MS, ROR_TRIM_SLEW_C_PER_S);
    return trim;
  }
};

struct Metrics {
  int samples = 0;
  int atZero = 0;        // duty <= 0.5 %
  int atFull = 0;        // duty >= 99.5 %
  double dutyMin = 1e9, dutyMax = -1e9;
  double maxDutyStep = 0;   // biggest change between two cycles, pp
  double maxAbsTrim = 0;    // C for the new law, pp for the old one
  double worstDev = 0;      // worst |BT - profile target|, C
  double meanAbsDev = 0;
  double rorMin = 1e9, rorMax = -1e9;
  double finalTemp = 0;
};

// The two runs' full traces, for the side-by-side table. Filled by runSim()
// itself, so the printed rows come from the same code path as the assertions.
struct Trace {
  std::vector<double> duty, trim, bt, target;
};

// The simulation's clock is its own: SimplePID reads millis(), and every run
// restarts it. Both legs share the plant, the PID, the feed-forward, the filter
// and the RoR estimator - only the guidance's output differs.
static Metrics runSim(bool oldLaw, Trace *traceOut = nullptr) {
  fakeMillis = 0;
  ror_reset();

  SimplePID pid(PID_KP, PID_KI, PID_KD, -HEATER_MAX_DUTY_PCT, HEATER_MAX_DUTY_PCT);
  EmaFilter filter(PID_INPUT_FILTER_TAU_S);
  Plant plant(2.0, 45.0, 19.0);
  filter.seed(plant.temp);

  OldGuidance oldGuidance;
  NewGuidance newGuidance;
  Metrics m;
  double devSum = 0;
  double prevDuty = 0;
  bool first = true;

  const unsigned long endMs = (kEndSeconds + 60) * 1000UL;
  while (fakeMillis < endMs) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    const unsigned long t = fakeMillis / 1000;
    const float profileTarget = measuredCurveAt(t);
    const double input = filter.update(plant.temp, kDt);
    ror_update(fakeMillis, (float)plant.temp, NAN);

    // The guidance is off until the estimate has warmed up and the step asks
    // for it - the same gates the firmware applies.
    const float rorTarget = rorTargetAt(t);
    const bool enabled = ror_valid() && rorTarget > 0.0f;
    const float rorActual = ror_get_bt();

    double duty;
    double trimOrOffset;
    if (oldLaw) {
      trimOrOffset = oldGuidance.apply(rorTarget - rorActual, enabled);
      const double pidDuty = pid.compute(profileTarget, input);
      duty = pidDuty + feedForwardDutyPct(requestedSlopeCPerMin(measuredCurveAt, t), (float)input) +
             trimOrOffset;
    } else {
      trimOrOffset = newGuidance.apply(rorTarget, rorActual, enabled);
      const double pidDuty = pid.compute(profileTarget + trimOrOffset, input);
      duty = pidDuty + feedForwardDutyPct(requestedSlopeCPerMin(measuredCurveAt, t), (float)input);
    }
    if (duty > HEATER_MAX_DUTY_PCT) duty = HEATER_MAX_DUTY_PCT;
    if (duty < 0.0) duty = 0.0;
    plant.step(duty);

    if (traceOut) {
      traceOut->duty.push_back(duty);
      traceOut->trim.push_back(trimOrOffset);
      traceOut->bt.push_back(plant.temp);
      traceOut->target.push_back(profileTarget);
    }

    // Metrics start once the RoR estimate is valid, so both legs are judged over
    // the same part of the roast.
    if (ror_valid()) {
      m.samples++;
      if (duty <= 0.5) m.atZero++;
      if (duty >= 99.5) m.atFull++;
      if (duty < m.dutyMin) m.dutyMin = duty;
      if (duty > m.dutyMax) m.dutyMax = duty;
      if (!first && fabs(duty - prevDuty) > m.maxDutyStep) m.maxDutyStep = fabs(duty - prevDuty);
      if (fabs(trimOrOffset) > m.maxAbsTrim) m.maxAbsTrim = fabs(trimOrOffset);
      const double dev = plant.temp - profileTarget;
      if (fabs(dev) > m.worstDev) m.worstDev = fabs(dev);
      devSum += fabs(dev);
      if (rorActual < m.rorMin) m.rorMin = rorActual;
      if (rorActual > m.rorMax) m.rorMax = rorActual;
    }
    prevDuty = duty;
    first = false;
  }
  m.meanAbsDev = m.samples ? devSum / m.samples : 0;
  m.finalTemp = plant.temp;
  return m;
}

// One row every 60 s, both legs side by side: the table the log would show.
// Both traces come from runSim(), i.e. the same runs the assertions above use.
static void printSideBySide() {
  Trace before, after;
  runSim(true, &before);
  runSim(false, &after);

  printf("\n  t(s)   BT    target | duty_before  offset_before | duty_after  trim_after\n");
  printf("  ------------------------------------------------------------------------\n");
  for (size_t r = 0; r * 60UL * 1000UL / SENSOR_READ_INTERVAL_MS < before.duty.size(); r++) {
    const size_t i = (size_t)(r * 60UL * 1000UL / SENSOR_READ_INTERVAL_MS);
    if (i >= before.duty.size() || i >= after.duty.size()) break;
    printf("  %4d  %6.1f %6.1f |  %7.1f %%   %+8.1f %% |  %6.1f %%   %+6.2f C\n",
           (int)(i * SENSOR_READ_INTERVAL_MS / 1000), after.bt[i], after.target[i],
           before.duty[i], before.trim[i], after.duty[i], after.trim[i]);
  }
  printf("\n");
}

static void testBeforeAfter() {
  const Metrics before = runSim(true);
  const Metrics after = runSim(false);
  char detail[260];

  const double beforeZeroPct = 100.0 * before.atZero / before.samples;
  const double beforeFullPct = 100.0 * before.atFull / before.samples;
  const double afterZeroPct = 100.0 * after.atZero / after.samples;
  const double afterFullPct = 100.0 * after.atFull / after.samples;

  snprintf(detail, sizeof(detail),
           "before: %.1f %% of the samples at 0 %%, %.1f %% at >= 99 %% | after: %.1f %% / %.1f %%",
           beforeZeroPct, beforeFullPct, afterZeroPct, afterFullPct);
  report(beforeZeroPct > 2.0 && beforeFullPct > 2.0,
         "the old law reproduces the measured 0 %/100 % slamming", detail);

  snprintf(detail, sizeof(detail),
           "both shares fall: 0 %% point %.1f -> %.1f %%, >= 99 %% point %.1f -> %.1f %%",
           beforeZeroPct, afterZeroPct, beforeFullPct, afterFullPct);
  report(afterZeroPct + afterFullPct < 0.25 * (beforeZeroPct + beforeFullPct),
         "the setpoint trim removes most of the time spent at the duty rails", detail);

  snprintf(detail, sizeof(detail), "biggest cycle-to-cycle duty step %.1f -> %.1f pp",
           before.maxDutyStep, after.maxDutyStep);
  report(after.maxDutyStep < 0.5 * before.maxDutyStep,
         "and the duty no longer jumps between the rails from one cycle to the next", detail);

  snprintf(detail, sizeof(detail), "duty band %.0f-%.0f %% -> %.0f-%.0f %%",
           before.dutyMin, before.dutyMax, after.dutyMin, after.dutyMax);
  report(after.dutyMax - after.dutyMin < before.dutyMax - before.dutyMin,
         "the duty band itself is narrower with the trim on the setpoint", detail);

  snprintf(detail, sizeof(detail), "peak |guidance| %.1f pp -> %.2f C (limit %.1f C)",
           before.maxAbsTrim, after.maxAbsTrim, ROR_TRIM_MAX_C);
  report(after.maxAbsTrim <= ROR_TRIM_MAX_C + 0.001,
         "and what the guidance asks for never leaves +-ROR_TRIM_MAX_C", detail);

  snprintf(detail, sizeof(detail), "30 s RoR swing %.0f..%.0f -> %.0f..%.0f C/min",
           before.rorMin, before.rorMax, after.rorMin, after.rorMax);
  report(after.rorMax - after.rorMin < before.rorMax - before.rorMin,
         "the measured rise is steadier too", detail);

  snprintf(detail, sizeof(detail), "worst |BT - target| %.1f -> %.1f C, mean %.1f -> %.1f C",
           before.worstDev, after.worstDev, before.meanAbsDev, after.meanAbsDev);
  report(after.meanAbsDev <= before.meanAbsDev,
         "and the roast tracks the curve at least as well as before", detail);
}

// The two readings from the control panel during that roast, straight from the
// log: what each controller asks for at exactly those errors. This is the
// concrete before/after for the measured error sequence - not a simulation.
static void printMeasuredPoints() {
  struct Point {
    const char *clock;
    float target;    // guidance value shown in the panel, C/min
    float measured;  // measured 30 s rise, C/min
  };
  static const Point kPoints[] = {
    { "2:47", 22.6f, 60.9f },
    { "4:09", 14.4f, -52.4f },
  };

  printf("\n  measured RoR error sequence (60 g, monsun-choklad, 2026-10-25)\n");
  printf("  time   target  measured     err | old duty offset | new setpoint trim\n");
  printf("  ---------------------------------------------------------------------\n");
  char detail[240];
  for (const Point &p : kPoints) {
    const float err = p.target - p.measured;
    const float oldOffset = err * 3.0f;  // pre-0.16.0: gain 3 %/(C/min)
    const float oldClipped = oldOffset > 100.0f ? 100.0f : (oldOffset < -100.0f ? -100.0f : oldOffset);
    const float trim = rorTrimRequestC(p.target, p.measured);
    printf("  %s   %5.1f    %+7.1f  %+6.1f |       %+6.1f %% |        %+5.2f C\n",
           p.clock, p.target, p.measured, err, oldClipped, trim);

    snprintf(detail, sizeof(detail), "%s: err %+.1f C/min -> %+.1f %% before, %+.2f C after",
             p.clock, err, oldClipped, trim);
    report(fabsf(oldClipped) >= 100.0f && fabsf(trim) <= ROR_TRIM_MAX_C,
           "the logged error saturated the old duty offset and clips the trim", detail);
  }
  printf("\n");
}

int main() {
  testTrimLaw();
  testSlew();
  printMeasuredPoints();
  testBeforeAfter();
  printSideBySide();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
