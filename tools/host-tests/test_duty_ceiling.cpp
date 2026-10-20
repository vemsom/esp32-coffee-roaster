// Host-side test of the bean-weight duty ceiling (include/duty_ceiling.h) and
// of the one thing the PID needs in order to honour it (SimplePID::setOutMax).
// Pure logic over a stubbed clock - no Arduino, no hardware, no other
// translation unit.
//
// Why it exists at all: the element is the same 1.1 kW whatever sits in the
// drum, so a 20 g charge gets ~5x the heat per gram that a 100 g charge does.
// The first roast with 0.12.0 (20 g, profile ljus-1) swung +-15-20 C around the
// target curve at a 70-80 s period, which is what the ceiling is the first
// measure against. Two things therefore have to be pinned:
//
//   (a) the curve itself: 25-30 % at 20 g, 100 % at 100 g and above, linear in
//       between, floor 20 % below the reference weight - and 0/absent means NO
//       ceiling, because that is what every profile written before the field
//       existed says;
//   (b) that a lowered output limit actually reaches the controller. A ceiling
//       applied only after the fact would leave the integral growing against a
//       limit it cannot see, and the loop would pay for that with an overshoot
//       the moment the setpoint is reached - the very failure the ceiling is
//       there to remove. So the integral is checked against the CEILING, not
//       against 100 %.
#include <Arduino.h>
#include <cstdio>
#include <cmath>

#include "config.h"
#include "pid.h"
#include "duty_ceiling.h"

// ---- stubbed clock ----------------------------------------------------------
static unsigned long fakeMillis = 0;
unsigned long millis() { return fakeMillis; }

static const double kDt = SENSOR_READ_INTERVAL_MS / 1000.0;   // 0.25 s

// ---- test bookkeeping -------------------------------------------------------
static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what) {
  checks++;
  if (ok) {
    printf("ok   %s\n", what);
  } else {
    printf("FAIL %s\n", what);
    failures++;
  }
}

static void checkClose(float got, float want, float tol, const char *what) {
  const bool ok = std::fabs(got - want) <= tol;
  checks++;
  if (ok) {
    printf("ok   %s (%.2f, wanted %.2f)\n", what, (double)got, (double)want);
  } else {
    printf("FAIL %s (%.2f, wanted %.2f +-%.2f)\n", what, (double)got, (double)want,
           (double)tol);
    failures++;
  }
}

int main() {
  // ------------------------------------------------- (a) the curve -----------
  // No weight stated = no ceiling. This is the compatibility promise: every
  // profile written before the field existed has to behave as it did.
  checkClose(dutyCeilingPct(0), HEATER_MAX_DUTY_PCT, 0.001f,
             "0 g (no weight stated) means no ceiling");
  checkClose(dutyCeilingPct(-5), HEATER_MAX_DUTY_PCT, 0.001f,
             "a negative weight means no ceiling too");

  // 20 g: inside the accepted 25-30 % band, and exactly the configured point.
  const float at20 = dutyCeilingPct(20);
  check(at20 >= 25.0f && at20 <= 30.0f, "20 g lands inside the 25-30 % band");
  checkClose(at20, DUTY_CEIL_AT_REF_PCT, 0.001f,
             "20 g gives exactly DUTY_CEIL_AT_REF_PCT");

  // 100 g and above: today's behaviour, full range, no interpolation left.
  checkClose(dutyCeilingPct(100), HEATER_MAX_DUTY_PCT, 0.001f,
             "100 g gives 100 % (no cap)");
  checkClose(dutyCeilingPct(101), HEATER_MAX_DUTY_PCT, 0.001f,
             "anything above 100 g stays at 100 %");
  checkClose(dutyCeilingPct(BEAN_WEIGHT_MAX_G), HEATER_MAX_DUTY_PCT, 0.001f,
             "the heaviest accepted charge is uncapped");

  // Linear in between: 60 g is the midpoint of 20..100 g, so the ceiling is
  // the midpoint of 28 % and 100 %.
  checkClose(dutyCeilingPct(60),
             (DUTY_CEIL_AT_REF_PCT + HEATER_MAX_DUTY_PCT) / 2.0f, 0.01f,
             "60 g (midpoint of 20..100 g) is the midpoint of the ceiling");
  // And a quarter of the way: 40 g = 28 + 72 * 20/80 = 46 %.
  checkClose(dutyCeilingPct(40), DUTY_CEIL_AT_REF_PCT +
                                     (HEATER_MAX_DUTY_PCT - DUTY_CEIL_AT_REF_PCT) * 0.25f,
             0.01f, "40 g interpolates linearly");

  // Below the reference weight the same line continues downward - a 5 g charge
  // must get less heat than a 20 g one - until the floor takes over.
  check(dutyCeilingPct(15) < at20 && dutyCeilingPct(15) > DUTY_CEIL_FLOOR_PCT,
        "a charge below 20 g gets a lower ceiling than 20 g");
  checkClose(dutyCeilingPct(BEAN_WEIGHT_MIN_G), DUTY_CEIL_FLOOR_PCT, 0.001f,
             "the lightest accepted charge sits on the 20 % floor");
  check(dutyCeilingPct(BEAN_WEIGHT_MIN_G) >= DUTY_CEIL_FLOOR_PCT,
        "no accepted weight produces a ceiling below the floor");

  // Monotonic: more beans may never mean less allowed heat.
  bool monotonic = true;
  float previous = 0.0f;
  for (int g = 1; g <= BEAN_WEIGHT_MAX_G; g++) {
    const float pct = dutyCeilingPct(g);
    if (pct < previous - 0.0001f) monotonic = false;
    if (pct < DUTY_CEIL_FLOOR_PCT - 0.0001f || pct > HEATER_MAX_DUTY_PCT + 0.0001f) {
      monotonic = false;
    }
    previous = pct;
  }
  check(monotonic, "the ceiling never decreases and stays inside 20-100 %");

  // The shipped gains are untouched by this work - the ceiling is an output
  // limit, not a retune.
  check(PID_KP == 1.2 && PID_KI == 0.04 && PID_KD == 0.0,
        "the PID gains are unchanged (1.2 / 0.04 / 0)");

  // --------------------------------- (b) the limit reaches the PID ----------
  // Drive the loop hard into saturation against a cold probe (a 180 C error
  // with KP 1.2 asks for 216 %), first with the plain 0-100 % range.
  SimplePID pid(PID_KP, PID_KI, PID_KD, 0, HEATER_MAX_DUTY_PCT);
  pid.reset();
  const double setpoint = 200.0;
  const double coldInput = 20.0;
  for (int i = 0; i < 2000; i++) {   // 500 s of a permanent error
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    pid.compute(setpoint, coldInput);
  }
  checkClose((float)pid.output(), HEATER_MAX_DUTY_PCT, 0.01f,
             "without a ceiling the loop saturates at 100 %");
  check(pid.iTerm() <= HEATER_MAX_DUTY_PCT + 0.001,
        "and its I term is clamped in output units");
  check(pid.integral() <= HEATER_MAX_DUTY_PCT / PID_KI + 0.5,
        "with the integral state bounded at outMax / KI");

  // Now the ceiling: the same loop, told that this charge only gets 28 %.
  pid.setOutMax(dutyCeilingPct(20));
  checkClose((float)pid.outMax(), DUTY_CEIL_AT_REF_PCT, 0.001f,
             "setOutMax moves the output limit the controller works against");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  const double capped = pid.compute(setpoint, coldInput);
  checkClose((float)capped, DUTY_CEIL_AT_REF_PCT, 0.01f,
             "the very next compute() is capped at the ceiling, not 100 %");
  check(pid.iTerm() <= DUTY_CEIL_AT_REF_PCT + 0.001,
        "the I term is clamped to the ceiling as well");

  // Keep the error on: the integral may not creep back up against a limit it
  // cannot use. This is the difference between "the request is clipped" and
  // "the controller knows the limit".
  for (int i = 0; i < 400; i++) {    // 100 s
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    pid.compute(setpoint, coldInput);
  }
  checkClose((float)pid.output(), DUTY_CEIL_AT_REF_PCT, 0.01f,
             "the output stays on the ceiling while the error is positive");
  check(pid.iTerm() <= DUTY_CEIL_AT_REF_PCT + 0.001,
        "and the I term does not wind up past it");

  // The setpoint is reached and then passed: the loop must come off the heat
  // instead of paying for a store of integral it built behind the ceiling.
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  const double past = pid.compute(setpoint, setpoint + 5.0);
  check(past >= 0.0 && past <= DUTY_CEIL_AT_REF_PCT + 0.001,
        "past the setpoint the output leaves the ceiling downward");
  for (int i = 0; i < 200; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    pid.compute(setpoint, setpoint + 10.0);
  }
  checkClose((float)pid.output(), 0.0f, 0.01f,
             "and reaches 0 % instead of holding heat from a wound-up integral");

  // Removing the ceiling (a different profile, or a manual run) restores the
  // full range: the limit is per run, not a permanent change to the loop.
  pid.setOutMax(HEATER_MAX_DUTY_PCT);
  for (int i = 0; i < 800; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    pid.compute(setpoint, coldInput);
  }
  checkClose((float)pid.output(), HEATER_MAX_DUTY_PCT, 0.01f,
             "clearing the ceiling lets the loop use the full range again");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
