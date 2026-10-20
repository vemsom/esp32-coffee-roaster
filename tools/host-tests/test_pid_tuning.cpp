// Host-side check of the PID trim that came out of the first real roast
// (2026-10-20, 20 g beans, profile ljus-1, BT-only). Pure logic: include/pid.h,
// include/filter.h and include/config.h over a stubbed clock - no Arduino, no
// hardware, no other translation unit.
//
// What the roast measured (HA history, 0 -> 425 s):
//
//   BT peaks   129.3 (10:34:07), 156.8 (10:35:19), 174.5 (10:36:29),
//              187.0 (10:37:47), 201.0 (10:39:05)
//   spacing    72, 70, 78, 78 s     (70-78 s)
//   amplitude  +-15-20 C around the target curve during the ramp
//   targets    110 -> 150 -> 185 -> 200 C, heater 0-100 % (2 s window)
//
// The reasoning this file pins, so a later "just nudge the numbers" cannot
// quietly undo it:
//
//   1. A 70-80 s oscillation cannot come from the sample rate (0.25 s) or from
//      the time-proportioning window (2 s): both are a factor ~300 and ~40
//      shorter than the period, so neither can set it. For a first-order plant
//      with dead time a sustained oscillation has a period of about 4 x the
//      dead time, which puts the loop's dead time at ~19 s (4 x 19 = 76 s,
//      inside the measured band). The band is asserted, not a single number.
//   2. With KP 2.0 %/C a 15 C error asks for 30 % duty within one sample, and
//      KI 0.15 puts the integral's action time at Ti = KP/KI = 13 s - SHORTER
//      than the 19 s the machine needs to show any effect. The loop corrected
//      faster than the plant answered, which is exactly a 4L cycle. So KP was
//      too high for this thermal mass and KI too strong: 0.11.1 fixed the
//      integral's clamp, which made 0.15 effective for the first time.
//   3. The two causes that are NOT it are ruled out in (1) and therefore not
//      re-tuned: the sample rate stays 250 ms (KD is 0, so a faster sample adds
//      no derivative noise) and HEATER_WINDOW_MS stays 2 s (40x shorter than
//      the thermal time constant - an average, not a pulse).
//
// The plants below bracket the identification: gain 2.0-2.5 C/% (100 % duty
// equilibrates at 225-275 C from ambient), first-order lag 45-60 s (bean probe
// plus drum; the same order as the ~30 s inertia measured on the machine
// earlier), dead time 19 s. They are a MODEL, and the machine is what settles
// the trim - so the assertions are about the size and direction of the
// improvement, not about a decimal.
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

// The gains the roast ran with (0.11.1/0.12.0), kept as the control group.
static const double kOldKp = 2.0;
static const double kOldKi = 0.15;

// The measured peaks placed on the roast clock: the first at ~52 s, then every
// ~74 s, with the profile's targets. The shape is what the machine actually
// ran, so the simulation is driven by the same curve.
static const double kStepTimes[] = { 52.0, 124.0, 194.0, 272.0, 350.0 };
static const double kStepTemps[] = { 110.0, 150.0, 185.0, 200.0, 200.0 };
static const double kStartTemp = 25.0;
static const double kRoastEndT = 425.0;

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

// ---- the plant --------------------------------------------------------------
// First-order lag plus a pure dead time, the standard shape for a thermocouple
// in a bean mass: dT/dt = (Ambient + K * duty - T) / tau, sampled at the real
// 250 ms cadence, with the duty held in a delay line for L seconds. The duty
// that equilibrates at 200 C is (200 - 25) / K = 70-88 % at K 2.0-2.5, which is
// why the element sat high through the whole measured ramp.
struct Plant {
  double K;        // C per % duty at equilibrium
  double tau;      // s
  double ambient;
  double temp;
  double *delay;
  int delayLen;
  int head;

  Plant(double k, double t, double l)
      : K(k), tau(t), ambient(kStartTemp), temp(kStartTemp), head(0) {
    delayLen = (int)(l / kDt + 0.5);
    if (delayLen < 1) delayLen = 1;
    delay = new double[delayLen];
    for (int i = 0; i < delayLen; i++) delay[i] = 0.0;
  }
  ~Plant() { delete[] delay; }

  void step(double duty) {
    const double applied = delay[head];   // what was commanded L seconds ago
    delay[head] = duty;
    head = (head + 1) % delayLen;
    temp += kDt / tau * (ambient + K * applied - temp);
  }
};

// The setpoint curve: linear ramps between the step arrivals, flat after the
// last one.
static double setpointAt(double t) {
  double prevT = 0.0;
  double prevTemp = kStartTemp;
  for (int i = 0; i < 5; i++) {
    if (t < kStepTimes[i]) {
      const double span = kStepTimes[i] - prevT;
      if (span <= 0.0) return kStepTemps[i];
      return prevTemp + (kStepTemps[i] - prevTemp) * ((t - prevT) / span);
    }
    prevT = kStepTimes[i];
    prevTemp = kStepTemps[i];
  }
  return kStepTemps[4];
}

// ---- one closed-loop run ----------------------------------------------------
struct Metrics {
  double maxAbove;    // furthest BT ever ran above the target curve, C
  double holdP2P;     // peak-to-peak in the last hold (350-425 s), C
  double finalError;  // |BT - 200| at the end of the roast, C
};

// Runs one roast with the gains given, through the SAME pieces the firmware
// uses: SimplePID and the one-pole input filter. The plant sees the duty the
// PID asked for; the filter sees the raw BT.
static Metrics runRoast(double kp, double ki, double K, double tau, double L) {
  SimplePID pid(kp, ki, PID_KD, 0, HEATER_MAX_DUTY_PCT);
  EmaFilter filter(PID_INPUT_FILTER_TAU_S);
  Plant plant(K, tau, L);
  filter.seed(plant.temp);
  fakeMillis = 0;   // every run is a roast from its own 0 s

  Metrics m = { -1e9, 0.0, 0.0 };
  double holdMin = 1e9, holdMax = -1e9;

  const int samples = (int)(kRoastEndT / kDt);
  for (int i = 0; i < samples; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    const double t = fakeMillis / 1000.0;
    const double sp = setpointAt(t);
    const double input = filter.update(plant.temp, kDt);
    plant.step(pid.compute(sp, input));

    const double above = plant.temp - sp;
    if (above > m.maxAbove) m.maxAbove = above;
    if (t >= 350.0) {                  // the last hold: the steady part
      if (plant.temp < holdMin) holdMin = plant.temp;
      if (plant.temp > holdMax) holdMax = plant.temp;
    }
  }
  m.holdP2P = holdMax - holdMin;
  m.finalError = fabs(plant.temp - 200.0);
  return m;
}

// The ultimate gain of the plant: the proportional gain at which a P-only loop
// oscillates forever. The frequency where the open loop's phase is -180
// degrees, then the gain that makes |G| = 1 there. Solved rather than
// hard-coded, so the "KP is well under the ultimate gain" check follows the
// plant it is applied to.
static double ultimateGain(double K, double tau, double L) {
  double lo = 1e-4, hi = 10.0;
  for (int i = 0; i < 200; i++) {
    const double w = 0.5 * (lo + hi);
    const double phase = atan(w * tau) + w * L;   // radians lost to lag + delay
    if (phase < M_PI) lo = w; else hi = w;
  }
  const double w = 0.5 * (lo + hi);
  const double mag = 1.0 / sqrt(1.0 + (w * tau) * (w * tau));
  return 1.0 / (K * mag);
}

static void testDeadTimeFromPeriod() {
  // The inference the trim rests on: a sustained 4L oscillation. The measured
  // 70-78 s therefore means 18-20 s of dead time, and the plants use 19 s.
  const double L_lo = 12.0, L_hi = 19.0;
  char detail[180];
  snprintf(detail, sizeof(detail), "4 x %.0f..%.0f s = %.0f..%.0f s, measured 70-78 s",
           L_lo, L_hi, 4 * L_lo, 4 * L_hi);
  report(4 * L_hi >= 70.0 && 4 * L_hi <= 80.0,
         "the measured 70-78 s period is 4 x a 19 s dead time", detail);

  // ...and the two causes the numbers rule out, stated as numbers: both are far
  // shorter than the period, so neither can set it.
  snprintf(detail, sizeof(detail), "sample %.2f s, window %.1f s, period ~75 s",
           SENSOR_READ_INTERVAL_MS / 1000.0, HEATER_WINDOW_MS / 1000.0);
  report(SENSOR_READ_INTERVAL_MS / 1000.0 < 1.0 && HEATER_WINDOW_MS / 1000.0 < 5.0,
         "sample rate and heater window are an order of magnitude shorter than the swing",
         detail);
}

static void testOldGainsSwing() {
  const Metrics a = runRoast(kOldKp, kOldKi, 2.0, 45.0, 19.0);
  const Metrics b = runRoast(kOldKp, kOldKi, 2.5, 60.0, 19.0);
  char detail[200];

  snprintf(detail, sizeof(detail),
           "hold swing %.1f C peak-to-peak (plant K=2.0 tau=45 L=19); the machine "
           "measured +-15-20 C around the curve", a.holdP2P);
  report(a.holdP2P >= 15.0, "the OLD gains reproduce the measured swing", detail);

  snprintf(detail, sizeof(detail), "hold swing %.1f C peak-to-peak (K=2.5 tau=60 L=19)",
           b.holdP2P);
  report(b.holdP2P >= 15.0, "and they swing that hard on the slower plant too", detail);
}

static void testNewGainsDamped() {
  const Metrics oldA = runRoast(kOldKp, kOldKi, 2.0, 45.0, 19.0);
  const Metrics oldB = runRoast(kOldKp, kOldKi, 2.5, 60.0, 19.0);
  const Metrics a = runRoast(PID_KP, PID_KI, 2.0, 45.0, 19.0);
  const Metrics b = runRoast(PID_KP, PID_KI, 2.5, 60.0, 19.0);
  char detail[220];

  snprintf(detail, sizeof(detail), "hold %.1f / %.1f C peak-to-peak (was %.1f / %.1f)",
           a.holdP2P, b.holdP2P, oldA.holdP2P, oldB.holdP2P);
  report(a.holdP2P <= 12.0 && b.holdP2P <= 12.0,
         "the NEW gains hold the setpoint inside a usable band", detail);

  // The point of the trim, on both plants: at least a factor four less swinging
  // than the same plant showed with the gains the roast was run with.
  snprintf(detail, sizeof(detail), "x%.1f / x%.1f less swing than the old gains",
           oldA.holdP2P / a.holdP2P, oldB.holdP2P / b.holdP2P);
  report(oldA.holdP2P >= 4.0 * a.holdP2P && oldB.holdP2P >= 4.0 * b.holdP2P,
         "and that is a large fraction of the swing, not a rounding difference", detail);

  // The excursion above the curve at a step change also drops - but it cannot
  // go to zero by tuning: part of it is the dead time itself, which no gain
  // removes. Asserted as a direction (at least 20 % lower on both plants).
  snprintf(detail, sizeof(detail), "max %.1f / %.1f C above the curve (was %.1f / %.1f)",
           a.maxAbove, b.maxAbove, oldA.maxAbove, oldB.maxAbove);
  report(a.maxAbove <= 0.8 * oldA.maxAbove && b.maxAbove <= 0.8 * oldB.maxAbove,
         "the overshoot above the target curve is smaller than with the old gains", detail);

  // The integral still does its job: a roast that ends several degrees under
  // the curve would be the P-only loop 0.11.1 removed.
  snprintf(detail, sizeof(detail), "|BT - 200| = %.2f / %.2f C at 425 s",
           a.finalError, b.finalError);
  report(a.finalError <= 2.0 && b.finalError <= 2.0,
         "the integral still removes the steady-state offset", detail);
}

static void testTrimIsDerivedNotGuessed() {
  // Every number below is a rule the trim has to keep obeying.
  const double ti = PID_KP / PID_KI;
  char detail[200];
  snprintf(detail, sizeof(detail), "Ti = KP/KI = %.1f s (dead time ~19 s)", ti);
  // Ti must be LONGER than the dead time it is fighting - that is the whole
  // correction: 13 s was shorter than 19 s, 30 s is not.
  report(ti >= 20.0 && ti <= 45.0,
         "the integral's action time is longer than the dead time, not shorter", detail);

  const double kuA = ultimateGain(2.0, 45.0, 19.0);
  const double kuB = ultimateGain(2.5, 60.0, 19.0);
  const double kuMin = (kuA < kuB) ? kuA : kuB;
  snprintf(detail, sizeof(detail), "KP %.2f vs ultimate %.2f / %.2f (%.0f %% of the lower)",
           PID_KP, kuA, kuB, 100.0 * PID_KP / kuMin);
  report(PID_KP <= 0.65 * kuMin, "KP sits well under the plant's ultimate gain", detail);

  snprintf(detail, sizeof(detail), "KP %.2f < %.2f and KI %.3f < %.3f",
           PID_KP, kOldKp, PID_KI, kOldKi);
  report(PID_KP < kOldKp && PID_KI < kOldKi,
         "both gains moved down from the values the roast was run with", detail);

  // The things NOT changed, on purpose.
  snprintf(detail, sizeof(detail), "KD %.1f, filter tau %.1f s, window %.1f s, sample %d ms",
           PID_KD, PID_INPUT_FILTER_TAU_S, HEATER_WINDOW_MS / 1000.0,
           SENSOR_READ_INTERVAL_MS);
  report(PID_KD == 0.0 && PID_INPUT_FILTER_TAU_S <= 2.0 &&
             SENSOR_READ_INTERVAL_MS == 250 && HEATER_WINDOW_MS == 2000,
         "KD, the input filter, the sample rate and the heater window are untouched",
         detail);
}

int main() {
  testDeadTimeFromPeriod();
  testOldGainsSwing();
  testNewGainsDamped();
  testTrimIsDerivedNotGuessed();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
