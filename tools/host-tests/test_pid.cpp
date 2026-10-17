// Host-side test of the PID (include/pid.h) and the control-input filter
// (include/filter.h). Pure logic over a stubbed clock - no Arduino, no
// hardware, no other translation unit.
//
// Why these four things are checked here, in this order:
//
//   (a) With a constant load below the element's maximum the error has to go to
//       ZERO. That is the whole point of the I term, and it is exactly what did
//       not happen on the machine: duty 82-86 % with BT parked ~20 C under its
//       setpoint is the steady-state offset of a P-only loop (duty / KP), so
//       the test pins that a P-only loop would keep that offset and this one
//       does not.
//   (b) No windup: while the output is saturated the integral may not keep
//       growing, and there may be no stored overshoot when the setpoint is
//       reached. Plus the clamp itself, which has to be in output units -
//       clamping the C*s state against the 0-100 % range caps the I term at
//       KI * 100 and is what made the loop P-only.
//   (c) A dithering input (the MAX6675 chain is quantised to 0.25 C and jitters
//       by degrees at 250 ms) may not switch the duty between 0 and 100. The
//       test runs the OLD gains/derivative against the same signal to show that
//       the failure is real and not hypothetical, then the shipped configuration
//       (KD 0 + filtered input) against a plant that holds a real load.
//
// (d) - the safety layer sees the UNFILTERED sample - cannot be checked here,
// because the filter only matters in the path that runs loop(). It lives in
// test_control.cpp, which injects a jump large enough to trip and asserts the
// latch still does.
#include <Arduino.h>
#include <cstdio>
#include <cmath>

#include "config.h"
#include "filter.h"
#include "pid.h"

// ---- stubbed clock ----------------------------------------------------------
static unsigned long fakeMillis = 0;
unsigned long millis() { return fakeMillis; }

static const double kDt = SENSOR_READ_INTERVAL_MS / 1000.0;   // 0.25 s

// The values the shipped loop used before 0.11.1, kept here as the control
// group for (c): KP 4.0 / KI 0.05 / KD 2.0 and no filter on the input.
static const double kLegacyKp = 4.0;
static const double kLegacyKi = 0.05;
static const double kLegacyKd = 2.0;

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

static void checkNear(double got, double want, double tol, const char *what) {
  char detail[128];
  snprintf(detail, sizeof(detail), "got %.3f, want %.3f +- %.3f", got, want, tol);
  report(fabs(got - want) <= tol, what, detail);
}

static void checkRange(double got, double lo, double hi, const char *what) {
  char detail[128];
  snprintf(detail, sizeof(detail), "got %.3f, allowed %.3f..%.3f", got, lo, hi);
  report(got >= lo && got <= hi, what, detail);
}

// ---- the plant --------------------------------------------------------------
// A first-order thermal model with a real time constant and a real load: 80 %
// duty is what holds 190 C (the loss in the model), and 100 % is the most the
// element can give. That is the shape of the machine the loop has to hold a
// setpoint on, and the load is what a P-only loop cannot remove - it stops
// where KP * error = duty.
struct Plant {
  double temp;
  explicit Plant(double start) : temp(start) {}

  void step(double duty, double dt) {
    const double ambient = 20.0;
    const double heatGain = 25.0;   // C/s at 100 % duty from cold
    const double loss =
        (0.80 * heatGain) / (190.0 - ambient);   // 80 % duty holds 190 C
    temp += dt * (duty / 100.0 * heatGain - (temp - ambient) * loss);
  }
};

// ---- (a) no residual offset under load -------------------------------------
static void testNoOffset() {
  SimplePID pid(PID_KP, PID_KI, PID_KD, 0, 100);
  Plant plant(20.0);
  const double setpoint = 190.0;

  fakeMillis = 1000;
  double duty = 0.0;
  const int samples = 4 * 300;          // 300 s at the real 4 Hz sample rate
  for (int i = 0; i < samples; i++) {
    fakeMillis += 250;
    duty = pid.compute(setpoint, plant.temp);
    plant.step(duty, kDt);
  }

  char detail[128];
  snprintf(detail, sizeof(detail),
           "bt %.2f C, setpoint %.1f, duty %.1f %%, P %.1f, I %.1f",
           plant.temp, setpoint, duty, pid.pTerm(), pid.iTerm());
  report(fabs(setpoint - plant.temp) < 0.5, "a constant load leaves no steady-state offset",
         detail);

  // The duty that holds the setpoint is the model's load (80 %), and the I term
  // is what carries it: the P term has gone to ~0, which is the difference
  // between this loop and the P-only one it replaces.
  checkRange(duty, 75.0, 85.0, "the loop settles at the duty the load actually needs");
  checkNear(pid.pTerm(), 0.0, 5.0, "the P term has gone quiet at the setpoint");
  checkRange(pid.iTerm(), 70.0, 90.0, "the I term is what holds the element up");

  // And the same signal through a P-only loop (KI 0, the behaviour the old
  // clamp produced in practice) parks at duty / KP under the setpoint - the
  // ~20 C this change was made for. Committed as a regression: if the I term
  // ever stops working again, this is the number it will look like.
  SimplePID pOnly(kLegacyKp, 0.0, 0.0, 0, 100);
  Plant pOnlyPlant(20.0);
  fakeMillis += 250;
  pOnly.reset();
  for (int i = 0; i < samples; i++) {
    fakeMillis += 250;
    duty = pOnly.compute(setpoint, pOnlyPlant.temp);
    pOnlyPlant.step(duty, kDt);
  }
  snprintf(detail, sizeof(detail), "P-only settles at %.2f C (%.1f C under %.1f)",
           pOnlyPlant.temp, setpoint - pOnlyPlant.temp, setpoint);
  report(setpoint - pOnlyPlant.temp > 10.0,
         "a P-only loop really does park well under the setpoint", detail);
}

// ---- (b) the integral clamp and anti-windup ---------------------------------
static void testIntegralClamp() {
  // KI * integral has to be clamped to the OUTPUT range. With this construct
  // the raw state reaches 100 C*s in the first five seconds, which the old
  // clamp (on the C*s state itself) read as "not clamped at all" and turned
  // into an I term of 1000 %.
  SimplePID pid(0.0, 10.0, 0.0, 0, 100);
  fakeMillis = 2000;
  double duty = 0.0;
  for (int i = 0; i < 20; i++) {
    fakeMillis += 250;
    duty = pid.compute(30.0, 10.0);   // constant 20 C error, nothing else
  }
  char detail[128];
  snprintf(detail, sizeof(detail), "I term %.3f %%, integral %.3f C*s (cap %.1f)",
           pid.iTerm(), pid.integral(), 100.0 / 10.0);
  checkNear(pid.iTerm(), 100.0, 1e-9, "the I term is clamped in output units");
  checkNear(pid.integral(), 10.0, 1e-9, "the stored integral follows the clamp, not the raw sum");
  checkNear(duty, 100.0, 1e-9, "the clamped I term is the whole output here");

  // Anti-windup, part 1: saturated output, error pushing further out. The
  // integral must stand still.
  SimplePID windup(PID_KP, PID_KI, PID_KD, 0, 100);
  fakeMillis = 3000;
  for (int i = 0; i < 400; i++) {
    fakeMillis += 250;
    duty = windup.compute(220.0, 20.0);   // 200 C of error, output pinned at max
  }
  snprintf(detail, sizeof(detail), "duty %.1f %%, I term %.6f %%", duty, windup.iTerm());
  checkNear(duty, 100.0, 1e-9, "200 C of error saturates the output");
  checkNear(windup.iTerm(), 0.0, 1e-9,
            "the integral does not grow while the output is saturated");

  // Anti-windup, part 2: the machine then arrives at the setpoint. A wound-up
  // integrator would hold the element on through the arrival; this one has
  // nothing stored and gives the duty straight back.
  fakeMillis += 250;
  duty = windup.compute(220.0, 220.0);
  snprintf(detail, sizeof(detail), "duty %.3f %% on arrival", duty);
  checkRange(duty, -1e-9, 1.0, "no stored windup means no overshoot on arrival");

  // Anti-windup, part 3: the state must also not be pinned where the error
  // points back INSIDE the range - that case has to be committed, or a loop
  // that once saturated could never recover.
  SimplePID recover(PID_KP, PID_KI, PID_KD, 0, 100);
  fakeMillis = 4000;
  for (int i = 0; i < 40; i++) {   // wind toward the ceiling
    fakeMillis += 250;
    recover.compute(220.0, 180.0);
  }
  const double iAtCeiling = recover.iTerm();
  for (int i = 0; i < 20; i++) {   // then approach from below the setpoint
    fakeMillis += 250;
    recover.compute(220.0, 215.0);
  }
  snprintf(detail, sizeof(detail), "I term %.3f -> %.3f %%", iAtCeiling, recover.iTerm());
  report(recover.iTerm() >= iAtCeiling - 1e-9,
         "a saturated integrator keeps integrating when the error turns back inside",
         detail);
}

// ---- (c) a dithering input must not switch the duty -------------------------
struct DutyStats {
  double minDuty;
  double maxDuty;
};

// Runs one closed loop against the Plant, with the input dithering +-10 C about
// its true value (what the thermocouple chain does to the signal at 250 ms
// sampling), and collects duty statistics over the LAST second half of the run
// - the first half is the loop's approach to the setpoint, where it is allowed
// to sit at either rail.
static DutyStats runDithering(double kp, double ki, double kd, bool filtered) {
  SimplePID pid(kp, ki, kd, 0, 100);
  EmaFilter filter(PID_INPUT_FILTER_TAU_S);
  Plant plant(190.0);
  const double setpoint = 190.0;

  DutyStats stats = { 1e9, -1e9 };
  const int samples = 4 * 60;   // 60 s
  for (int i = 0; i < samples; i++) {
    const double noise = (i % 2) ? -10.0 : 10.0;
    const double raw = plant.temp + noise;
    const double input = filtered ? filter.update(raw, kDt) : raw;
    fakeMillis += 250;
    const double duty = pid.compute(setpoint, input);
    plant.step(duty, kDt);
    if (i >= samples / 2) {
      if (duty < stats.minDuty) stats.minDuty = duty;
      if (duty > stats.maxDuty) stats.maxDuty = duty;
    }
  }
  return stats;
}

static void testNoisyInput() {
  const DutyStats legacy = runDithering(kLegacyKp, kLegacyKi, kLegacyKd, false);
  char detail[128];
  snprintf(detail, sizeof(detail), "duty %.1f..%.1f %% (swing %.1f)",
           legacy.minDuty, legacy.maxDuty, legacy.maxDuty - legacy.minDuty);
  report(legacy.maxDuty - legacy.minDuty >= 80.0,
         "the OLD gains + raw input swing the duty over 80 points sample to sample",
         detail);

  const DutyStats now = runDithering(PID_KP, PID_KI, PID_KD, true);
  snprintf(detail, sizeof(detail), "duty %.1f..%.1f %% (swing %.1f)",
           now.minDuty, now.maxDuty, now.maxDuty - now.minDuty);
  report(now.maxDuty - now.minDuty <= 20.0,
         "KD 0 + a filtered input holds the duty inside a usable band", detail);
  report((legacy.maxDuty - legacy.minDuty) > 4.0 * (now.maxDuty - now.minDuty),
         "and that band is a fraction of what the same signal did before", detail);
  checkRange(now.minDuty, 60.0, 100.0,
             "and the loop keeps real heat on instead of chattering around 0");

  // The derivative term, with KD at 0, contributes exactly nothing - it is a
  // configuration and not a quietly-trimmed gain.
  SimplePID noD(PID_KP, PID_KI, PID_KD, 0, 100);
  fakeMillis += 250;
  noD.reset();
  fakeMillis += 250;
  noD.compute(190.0, 180.0);
  fakeMillis += 250;
  noD.compute(190.0, 185.0);      // a real step, in one sample
  checkNear(noD.dTerm(), 0.0, 1e-12, "KD 0 contributes exactly nothing");
}

// ---- the filter itself ------------------------------------------------------
static void testFilter() {
  EmaFilter filter(PID_INPUT_FILTER_TAU_S);

  // The first sample seeds the filter: a run that starts on a 190 C machine
  // must not make the loop spend a second pulling its input out of 0.
  const double seeded = filter.update(190.0, kDt);
  checkNear(seeded, 190.0, 1e-9, "the first sample is taken as-is (no ramp from 0)");

  // One time constant is one time constant: after PID_INPUT_FILTER_TAU_S the
  // step has covered 1 - 1/e of the way. Seeded first, because the first sample
  // after a reset is taken as-is by design (see the check above).
  EmaFilter step(PID_INPUT_FILTER_TAU_S);
  step.seed(0.0);
  double value = 0.0;
  const int n = (int)(PID_INPUT_FILTER_TAU_S / kDt + 0.5);
  for (int i = 0; i < n; i++) value = step.update(100.0, kDt);
  checkNear(value, 100.0 * (1.0 - std::exp(-1.0)), 0.5,
            "a step is ~63 % covered after one time constant");

  // And it does what it is there for: the +-10 C dither of the thermocouple
  // chain comes through at a fraction of its amplitude, so the P and D terms
  // never see the quantisation.
  EmaFilter dither(PID_INPUT_FILTER_TAU_S);
  double minV = 1e9, maxV = -1e9;
  for (int i = 0; i < 200; i++) {
    const double v = dither.update(190.0 + ((i % 2) ? -10.0 : 10.0), kDt);
    if (i < 20) continue;   // let the first sample wash out
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  char detail[128];
  snprintf(detail, sizeof(detail), "+-10 C in, %.2f C out", (maxV - minV) / 2.0);
  report((maxV - minV) / 2.0 < 2.5, "the filter takes the dither down by roughly 8x", detail);
}

int main() {
  testNoOffset();
  testIntegralClamp();
  testNoisyInput();
  testFilter();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
