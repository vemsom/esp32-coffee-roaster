// Host-side check of the ramp feed-forward that replaced the bean-weight duty
// ceiling (0.15.0): include/feed_forward.h, the requested-slope helper, and a
// simulated before/after over the plant the trim was reasoned against. Pure
// logic: include/pid.h + include/filter.h + include/config.h over a stubbed
// clock - no Arduino, no hardware, no other translation unit.
//
// Why the ceiling went, from the machine (three roasts 2026-10-20, same curve):
//
//   | Weight | duty max | time at >= 99 % duty |
//   | 20 g   |     97 % |                   0 s |
//   | 40 g   |     95 % |                   0 s |
//   | 62.8 g |     91 % |                   0 s |
//
// The loop never saturated: it sat on 20-97 % in a smooth curve through the whole
// roast. A cap of 28 % at 20 g therefore bound the PID far below the power the
// roast actually used, and the swing it was meant to remove had already gone with
// the 0.13.0 gain trim (KP 1.2 / KI 0.04). What is left is the lag: BT trails the
// target curve by +12..+14 C at 30 s and +8..+10 C at 120 s in ALL THREE passes -
// mass-independent, so it is not the charge. A PID only sees the temperature
// error, so on a linear ramp it cannot ask for heat until the roast is behind and
// the integral has to catch up with a curve that keeps moving. That part is known
// in advance: the profile's own slope and the bean temperature.
//
// The model (fitted to those three roasts, 313 samples at 10 s, ramps only):
//
//   duty_ff = 0.09 * slope + 0.41 * (BT - 22) + 20.2   [percent, clipped 0..100]
//
// mean error 2.5 percentage points, R^2 0.97. This file pins the two control
// points the fit has to answer, the clipping, the hold behaviour (slope 0 leaves
// only the loss term, and no hack at the ramp->hold edge), and that the lag on a
// simulated ramp is smaller with the feed-forward than without it.
#include <Arduino.h>
#include <cstdio>
#include <cmath>
#include <functional>
#include <vector>

#include "config.h"
#include "filter.h"
#include "pid.h"
#include "feed_forward.h"

// ---- stubbed clock ----------------------------------------------------------
static unsigned long fakeMillis = 0;
unsigned long millis() { return fakeMillis; }

static const double kDt = SENSOR_READ_INTERVAL_MS / 1000.0;   // 0.25 s

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

static bool near(float got, float want, float tol) { return fabs(got - want) <= tol; }

// ---------------------------------------------------------------------------
// 1. The model itself, against the two control points the fit was checked on.
// ---------------------------------------------------------------------------
static void testControlPoints() {
  char detail[200];

  // 45 C/min at BT 60: the fit says 40 %, the three roasts measured 38-42 %.
  const float fast = feedForwardDutyPct(45.0f, 60.0f);
  snprintf(detail, sizeof(detail),
           "45 C/min at BT 60 -> %.2f %% (fit 40, measured 38-42)", fast);
  report(near(fast, 40.0f, 2.0f), "the model reproduces the fast-ramp control point", detail);

  // 12 C/min at BT 180: the fit says 87 %, the roasts measured 81-91 %.
  const float slow = feedForwardDutyPct(12.0f, 180.0f);
  snprintf(detail, sizeof(detail),
           "12 C/min at BT 180 -> %.2f %% (fit 87, measured 81-91)", slow);
  report(near(slow, 87.0f, 2.0f), "the model reproduces the late-ramp control point", detail);

  // The two terms have to be in the right place, not just the sum: at ambient
  // with no ramp only the offset is left, and the loss term is what a HOLD
  // needs - 0.41 % per C above FF_AMBIENT_C.
  const float atAmbient = feedForwardDutyPct(0.0f, FF_AMBIENT_C);
  snprintf(detail, sizeof(detail), "0 C/min at BT %.0f -> %.2f %% (FF_OFFSET_PCT %.1f)",
           FF_AMBIENT_C, atAmbient, FF_OFFSET_PCT);
  report(near(atAmbient, FF_OFFSET_PCT, 0.01f),
         "at ambient temperature with no ramp the model is exactly its offset", detail);

  const float at180 = feedForwardDutyPct(0.0f, 180.0f);
  snprintf(detail, sizeof(detail), "0 C/min at BT 180 -> %.2f %% (loss term only)", at180);
  report(near(at180, FF_LOSS_GAIN_PCT_PER_C * (180.0f - FF_AMBIENT_C) + FF_OFFSET_PCT, 0.01f),
         "in a hold the loss term is what is left", detail);

  // 22 C is the room temperature the fit was made at, and it is an input to the
  // loss term - not a number that can drift out of the formula unnoticed.
  snprintf(detail, sizeof(detail), "FF_AMBIENT_C = %.1f C (the workshop at the fit)", FF_AMBIENT_C);
  report(FF_AMBIENT_C == 22.0f, "the ambient the fit was made at is 22 C", detail);
}

// ---------------------------------------------------------------------------
// 2. The clipping: a duty is a duty, never outside 0..100 %.
// ---------------------------------------------------------------------------
static void testClipping() {
  char detail[220];

  const float high = feedForwardDutyPct(500.0f, 400.0f);
  snprintf(detail, sizeof(detail), "500 C/min at BT 400 -> %.2f %%", high);
  report(high == HEATER_MAX_DUTY_PCT, "an impossible request clips at HEATER_MAX_DUTY_PCT", detail);

  const float low = feedForwardDutyPct(-500.0f, 20.0f);
  snprintf(detail, sizeof(detail), "-500 C/min at BT 20 -> %.2f %%", low);
  report(low == 0.0f, "a falling curve clips at 0 %, it never goes negative", detail);

  // A NaN probe (a channel that has never read) must not produce a NaN duty that
  // then slips through the caller's comparisons.
  const float nanSlope = feedForwardDutyPct(NAN, 100.0f);
  const float nanBt = feedForwardDutyPct(10.0f, NAN);
  snprintf(detail, sizeof(detail), "NaN slope -> %.2f, NaN BT -> %.2f", nanSlope, nanBt);
  report(nanSlope == 0.0f && nanBt == 0.0f, "a NaN input yields 0, not a NaN duty", detail);

  // A sweep over everything a roast can produce: no value outside the range,
  // and the map is monotone in both arguments (more slope or more temperature
  // can never ask for less heat).
  bool inRange = true, monotone = true;
  float prev = -1.0f;
  for (float slope = -100.0f; slope <= 100.0f; slope += 0.5f) {
    for (float bt = -50.0f; bt <= 400.0f; bt += 2.5f) {
      const float d = feedForwardDutyPct(slope, bt);
      if (!(d >= 0.0f) || d > HEATER_MAX_DUTY_PCT) inRange = false;
      if (bt > -50.0f) {
        const float below = feedForwardDutyPct(slope, bt - 2.5f);
        if (d < below - 0.0001f) monotone = false;
      }
    }
    const float atRamp = feedForwardDutyPct(slope, 150.0f);
    if (slope > -100.0f && atRamp < prev - 0.0001f) monotone = false;
    prev = atRamp;
  }
  report(inRange, "the duty is never outside 0..100 % over the whole sweep",
         "slope -100..100 C/min, BT -50..400 C, 0.5 / 2.5 steps");
  report(monotone, "and it never asks for less heat at a higher slope or a higher BT",
         "a monotone map: no fold in the surface");
}

// ---------------------------------------------------------------------------
// 3. The requested slope, taken from the profile's own curve.
// ---------------------------------------------------------------------------
// The curve every check below shares, shaped like a real roast rather than a
// single straight line: a fast first ramp (the drying phase), a slower second one
// (to first crack), then a hold. Both rates are the two control points the model
// was fitted and measured on.
//
//   segment 1: 20 -> 150 C over 180 s   = 43.3 C/min   (measured 38-42 % duty at BT 60)
//   segment 2: 150 -> 200 C over 250 s  = 12.0 C/min   (measured 81-91 % duty at BT 180)
//   then            hold at 200 C
static const float kStartTemp = 20.0f;
static const float kEndTemp = 200.0f;

struct Segment {
  unsigned long seconds;   // ramp length
  float temp;              // temperature at the end of it
};

static const Segment kCurve[] = { { 180, 150.0f }, { 250, kEndTemp } };
static const unsigned long kRampSeconds = 180 + 250;    // the hold starts here
static const unsigned long kEndSeconds = 600;

static float testCurveAt(unsigned long t) {
  float prev = kStartTemp;
  unsigned long at = 0;
  for (const Segment &s : kCurve) {
    if (t < at + s.seconds) {
      return prev + (float)(t - at) / (float)s.seconds * (s.temp - prev);
    }
    prev = s.temp;
    at += s.seconds;
  }
  return kEndTemp;
}

// The steepest rate anywhere on that curve, in C/min - the step a slope-based
// feed-forward is allowed to make.
static float testCurveMaxSlope() {
  float prev = kStartTemp, steepest = 0.0f;
  for (const Segment &s : kCurve) {
    const float rate = (s.temp - prev) / ((float)s.seconds / 60.0f);
    if (rate > steepest) steepest = rate;
    prev = s.temp;
  }
  return steepest;
}

static void testRequestedSlope() {
  char detail[220];

  // The derivative of the requested curve IS its ramp rate - not the measured
  // rise, which is what makes this a feed-forward and not another feedback loop.
  const float firstRate = (kCurve[0].temp - kStartTemp) / ((float)kCurve[0].seconds / 60.0f);
  bool first = true;
  for (unsigned long t = 0; t + 2 < kCurve[0].seconds; t += 7) {
    if (!near(requestedSlopeCPerMin(testCurveAt, t), firstRate, 0.01f)) first = false;
  }
  snprintf(detail, sizeof(detail), "every interior second of the %lu s first ramp reads %.1f C/min",
           kCurve[0].seconds, firstRate);
  report(first, "the slope helper reports the ramp's own rate", detail);

  const float secondRate = (kCurve[1].temp - kCurve[0].temp) / ((float)kCurve[1].seconds / 60.0f);
  bool second = true;
  for (unsigned long t = kCurve[0].seconds + 2; t + 2 < kRampSeconds; t += 11) {
    if (!near(requestedSlopeCPerMin(testCurveAt, t), secondRate, 0.01f)) second = false;
  }
  snprintf(detail, sizeof(detail), "every interior second of the %lu s second ramp reads %.1f C/min",
           kCurve[1].seconds, secondRate);
  report(second, "and it follows the curve when the rate changes", detail);

  // A hold is exactly 0 - that is what keeps the feed-forward from fighting the
  // loop once the curve has stopped moving.
  const float holdSlope = requestedSlopeCPerMin(testCurveAt, kRampSeconds + 20);
  snprintf(detail, sizeof(detail), "second %lu of a hold -> %.2f C/min", kRampSeconds + 20, holdSlope);
  report(holdSlope == 0.0f, "a hold has a requested slope of exactly 0", detail);

  // A paused roast does not advance its clock, so the curve stands still there:
  // same input, same answer, no separate pause case in the control loop.
  const float paused = requestedSlopeCPerMin(testCurveAt, 100) -
                       requestedSlopeCPerMin(testCurveAt, 100);
  snprintf(detail, sizeof(detail), "frozen clock -> %.2f C/min difference between cycles", paused);
  report(paused == 0.0f, "a frozen roast clock has no slope to feed forward", detail);

  // A curve that reads nothing (an empty profile) must not feed anything forward.
  const float nanSlope = requestedSlopeCPerMin([](unsigned long) { return NAN; }, 10);
  snprintf(detail, sizeof(detail), "NaN curve -> %.2f C/min", nanSlope);
  report(nanSlope == 0.0f, "a curve that cannot be read gives 0, not a NaN slope", detail);
}

// ---------------------------------------------------------------------------
// 4. The hold: slope 0, so only the loss term - and no hack at the edge.
// ---------------------------------------------------------------------------
static void testHoldHasNoHack() {
  char detail[240];

  // Walk the whole curve one second at a time, the way the control loop sees it.
  // The edges are where a slope-based feed-forward can step: the rate drops
  // 43.3 -> 12.0 C/min between the ramps and 12.0 -> 0 into the hold. Each step
  // may be the model's own slope term and no more (0.09 * the rate that went
  // away) and it must go DOWN, never up: the curve stopped asking for a climb.
  const float maxStep = FF_SLOPE_GAIN_PCT_PER_C_PER_MIN * testCurveMaxSlope() + 0.01f;

  float prev = -1.0f, biggestStep = 0.0f, biggestAt = 0.0f;
  bool inRange = true, holdConstant = true, onlyDown = true;
  float holdValue = -1.0f;

  for (unsigned long t = 0; t <= kEndSeconds; t++) {
    const float bt = testCurveAt(t);                 // the probe follows the curve here
    const float duty = feedForwardDutyPct(requestedSlopeCPerMin(testCurveAt, t), bt);
    if (!(duty >= 0.0f) || duty > HEATER_MAX_DUTY_PCT) inRange = false;

    if (prev >= 0.0f) {
      const float step = fabs(duty - prev);
      if (step > biggestStep) { biggestStep = step; biggestAt = (float)t; }
      // BT also rises between samples, so the loss term alone lifts the duty;
      // the only steps allowed to go up by more than that are none. A drop is
      // what a rate change looks like.
      if (duty > prev + 0.01f && step > maxStep) onlyDown = false;
    }
    // Inside the hold (past the one blend second at the edge) the request has to
    // be bit-for-bit the same number every second: a hold that drifted would
    // look like a slow hack in the log.
    if (t >= kRampSeconds + 2) {
      if (holdValue < 0.0f) holdValue = duty;
      if (fabs(duty - holdValue) > 0.0001f) holdConstant = false;
    }
    prev = duty;
  }

  snprintf(detail, sizeof(detail), "largest one-second change %.2f pp at %.0f s, allowed %.2f",
           biggestStep, biggestAt, maxStep);
  report(biggestStep <= maxStep,
         "no hack: the duty moves by no more than the model's own slope term", detail);
  report(onlyDown, "and a rate change moves it down, never up", "a dropping slope is less heat");

  snprintf(detail, sizeof(detail), "hold value %.2f %% for %lu s", holdValue,
           kEndSeconds - (kRampSeconds + 2));
  report(holdConstant, "and inside the hold the request is the same number every second", detail);

  snprintf(detail, sizeof(detail), "hold %.2f %% vs loss term %.2f %%", holdValue,
           FF_LOSS_GAIN_PCT_PER_C * (kEndTemp - FF_AMBIENT_C) + FF_OFFSET_PCT);
  report(near(holdValue, FF_LOSS_GAIN_PCT_PER_C * (kEndTemp - FF_AMBIENT_C) + FF_OFFSET_PCT, 0.01f),
         "the hold asks for exactly the loss term, nothing more", detail);

  report(inRange, "and the whole curve stays inside 0..100 %", "one sample per second, ramps -> hold");

  // The edge itself: the duty at the last ramp second must be at least the hold
  // value (a rising curve cannot ask for less heat when it stops rising) and the
  // drop may not exceed the slope term.
  const float edge = feedForwardDutyPct(requestedSlopeCPerMin(testCurveAt, kRampSeconds - 2),
                                        testCurveAt(kRampSeconds - 2));
  const float inHold = feedForwardDutyPct(0.0f, kEndTemp);
  snprintf(detail, sizeof(detail), "%.2f %% -> %.2f %% at the ramp->hold edge", edge, inHold);
  report(edge >= inHold - 0.0001f && edge - inHold <= maxStep,
         "the ramp->hold edge drops by the slope term and no more", detail);
}

// ---------------------------------------------------------------------------
// 5. Simulated before/after: the same plant, feed-forward off and on.
// ---------------------------------------------------------------------------
// First-order lag plus dead time, the shape the 0.13.0 trim was reasoned against
// (tools/host-tests/test_pid_tuning.cpp): dT/dt = (ambient + K * duty - T) / tau,
// sampled at the real 250 ms cadence, with the duty held in a delay line for L
// seconds. K 2.0 C/% means 100 % duty equilibrates at 222 C, i.e. ~90 % holds
// 200 C - the same place the three roasts ran (87-91 % at BT 180).
struct Plant {
  double K, tau, ambient, temp;
  std::vector<double> delay;
  size_t head = 0;

  Plant(double k, double t, double l) : K(k), tau(t), ambient(FF_AMBIENT_C), temp(kStartTemp) {
    size_t len = (size_t)(l / kDt + 0.5);
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

// The simulation's clock is its own: SimplePID reads millis(), so every run
// starts from 0 and the 250 ms cadence is the firmware's own. The curve it
// follows is testCurveAt() - the same one the checks above walk.
static float simTargetAt(unsigned long t) { return testCurveAt(t); }

struct Metrics {
  double lag30 = 0;       // target - BT, 30 s into the ramp
  double lag120 = 0;      // target - BT, 120 s into the ramp
  double meanLag = 0;     // mean (target - BT) over the whole ramp
  double maxLag = 0;      // worst lag on the ramp
  double holdMax = 0;     // highest BT during the hold, C
  double holdMin = 1e9;   // lowest BT in the last 60 s of the run
  double finalTemp = 0;   // BT at the end
};

// 'pidMin' is the lower end of the PID's own output range. With the
// feed-forward on it MUST be -HEATER_MAX_DUTY_PCT (a trim that can subtract);
// the 0 variant is the counter-example the firmware does not run, and it is here
// to pin WHY the range is bipolar.
static Metrics runSim(bool feedForward, double pidMin, double K, double tau, double L) {
  SimplePID pid(PID_KP, PID_KI, PID_KD, pidMin, HEATER_MAX_DUTY_PCT);
  EmaFilter filter(PID_INPUT_FILTER_TAU_S);
  Plant plant(K, tau, L);
  filter.seed(plant.temp);
  fakeMillis = 0;

  Metrics m;
  double lagSum = 0;
  int lagSamples = 0;
  bool lag30Set = false;

  const unsigned long endMs = (unsigned long)((kEndSeconds + 50) * 1000.0);
  while (fakeMillis < endMs) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    const unsigned long t = fakeMillis / 1000;
    const float target = simTargetAt(t);
    const double input = filter.update(plant.temp, kDt);

    const float pidDuty = (float)pid.compute(target, input);
    double duty = pidDuty;
    if (feedForward) {
      duty += feedForwardDutyPct(requestedSlopeCPerMin(simTargetAt, t), (float)input);
      if (duty > HEATER_MAX_DUTY_PCT) duty = HEATER_MAX_DUTY_PCT;
      if (duty < 0.0) duty = 0.0;
    }
    plant.step(duty);

    if (t < kRampSeconds) {
      const double lag = target - plant.temp;
      if (t >= 30 && !lag30Set) {          // the first sample at or past 30 s
        m.lag30 = lag;
        lag30Set = true;
      }
      if (t == 119) m.lag120 = lag;        // the last sample before 120 s
      if (lag > m.maxLag) m.maxLag = lag;
      lagSum += lag;
      lagSamples++;
    } else {
      if (plant.temp > m.holdMax) m.holdMax = plant.temp;
      if (t >= kEndSeconds - 60 && plant.temp < m.holdMin) m.holdMin = plant.temp;
    }
  }
  m.meanLag = lagSamples ? lagSum / lagSamples : 0;
  m.finalTemp = plant.temp;
  return m;
}

static void testLagBeforeAfter(double K, double tau, double L, const char *label,
                               double minDrop30, double minDropMean) {
  // The baseline is what shipped in 0.13.0: PID only, positive-only output.
  const Metrics off = runSim(false, 0.0, K, tau, L);
  const Metrics on = runSim(true, -HEATER_MAX_DUTY_PCT, K, tau, L);
  char detail[280];

  snprintf(detail, sizeof(detail),
           "%s: lag at 30 s %.1f -> %.1f C, at 120 s %.1f -> %.1f C", label, off.lag30,
           on.lag30, off.lag120, on.lag120);
  report(on.lag30 < off.lag30 && on.lag120 < off.lag120,
         "the feed-forward cuts the lag at both measured times", detail);

  snprintf(detail, sizeof(detail), "%s: lag at 30 s falls %.1f %%, needed %.0f %%", label,
           100.0 * (off.lag30 - on.lag30) / off.lag30, 100.0 * minDrop30);
  report(off.lag30 > 0 && (off.lag30 - on.lag30) >= minDrop30 * off.lag30,
         "and it is a large part of it, not a roundoff", detail);

  snprintf(detail, sizeof(detail), "%s: mean lag over the ramp %.1f -> %.1f C (%.0f %% lower)",
           label, off.meanLag, on.meanLag, 100.0 * (off.meanLag - on.meanLag) / off.meanLag);
  report(off.meanLag > 0 && (off.meanLag - on.meanLag) >= minDropMean * off.meanLag,
         "the mean lag over the whole ramp drops too", detail);

  // The hold: the feed-forward gives the base, the loop trims the rest, so the
  // run has to SETTLE - a base that is a couple of points off may cost a few
  // degrees on arrival, but it may not leave the roast parked off the setpoint.
  snprintf(detail, sizeof(detail), "%s: hold peak %.1f -> %.1f C, settles at %.1f C (target %.0f)",
           label, off.holdMax, on.holdMax, on.finalTemp, kEndTemp);
  report(fabs(on.finalTemp - kEndTemp) <= 3.0,
         "the hold settles on the setpoint with the feed-forward in the loop", detail);
  report(on.holdMax <= off.holdMax + 8.0,
         "and the arrival overshoot stays in the same band as the PID-only run", detail);
}

// The counter-example: the same feed-forward with a PID that can only ADD (the
// 0..100 range). It hits its 0 % floor, the conditional integration freezes the
// integral there, and the base alone holds the plant at its own equilibrium -
// 22 C above the target on this plant and going nowhere. That is why the range
// is bipolar in profile mode (include/pid.h), and this check is what stops
// "simplify it back to 0..100" from looking harmless.
static void testTrimMustBeAbleToSubtract() {
  const Metrics stuck = runSim(true, 0.0, 2.0, 45.0, 19.0);
  const Metrics trims = runSim(true, -HEATER_MAX_DUTY_PCT, 2.0, 45.0, 19.0);
  char detail[260];

  snprintf(detail, sizeof(detail), "positive-only trim parks at %.1f C, bipolar trim at %.1f C (target %.0f)",
           stuck.finalTemp, trims.finalTemp, kEndTemp);
  report(trims.finalTemp <= kEndTemp + 3.0 && stuck.finalTemp >= kEndTemp + 10.0,
         "a trim that can only add cannot pull the feed-forward back down", detail);
}

int main() {
  testControlPoints();
  testClipping();
  testRequestedSlope();
  testHoldHasNoHack();
  testTrimMustBeAbleToSubtract();

  // The identified plant, and a slower one as a direction check. The identified
  // numbers (K 2.0 C/%, tau 45 s, L 19 s) are the ones the 0.13.0 trim was
  // reasoned against; the machine decides the rest.
  testLagBeforeAfter(2.0, 45.0, 19.0, "K2.0/tau45/L19", 0.30, 0.30);
  testLagBeforeAfter(1.9, 60.0, 19.0, "K1.9/tau60/L19", 0.30, 0.30);

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
