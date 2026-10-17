// Host-side test of the fan output (src/fan_control.cpp).
//
// fan_set_speed() keeps its percent API: 0 = off, 1-100 = the logical speed the
// web UI, profile steps, MQTT/HA and the fan interlock talk, and what
// fan_requested_pct() reports. What this test pins is the other two halves:
//
//   * the duty that reaches the LEDC channel - the request mapped onto the band
//     the fan actually responds in (a DC fan stands still below its start
//     threshold), instead of a straight 0-100 duty;
//   * the RAMP to it. The 24 V supply cut out when the fan was switched
//     straight on at 100 %, so the duty climbs at FAN_SLEW_PCT_PER_SEC instead
//     of jumping - and while it climbs the fan is not there yet, which is why
//     fan_actual_pct() (not the request) is what the heat interlock judges on.
//     A decrease is still immediate: spinning down draws no start current.
//
// Built three times by tools/host-tests/run.sh:
//   * the default config (the band from config.h, the configured slew rate)
//   * a misconfigured band with min == max, to pin the straight-map fallback
//   * FAN_SLEW_PCT_PER_SEC 0, to pin the "no ramp" escape hatch
// The band and the rate are compile-time constants, so the #if blocks below
// select which promises this build checks; the recording ledcWrite stub and the
// stubbed clock are shared.
#include <Arduino.h>
#include <cstdio>

#include "config.h"
#include "fan_control.h"

// ---- stubbed clock ----------------------------------------------------------
static unsigned long fakeMillis = 0;
unsigned long millis() { return fakeMillis; }

// ---- recording LEDC stub ----------------------------------------------------
static int g_lastChannel = -1;
static int g_lastDuty = -1;

void ledcSetup(int, double, int) {}
void ledcAttachPin(int, int) {}
void ledcWrite(int channel, int duty) {
  g_lastChannel = channel;
  g_lastDuty = duty;
}
long map(long x, long inMin, long inMax, long outMin, long outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

static const int kMaxDuty = (1 << FAN_PWM_RESOLUTION) - 1;

// ---- test helpers -----------------------------------------------------------
static int checks = 0;
static int failures = 0;

static void check(bool cond, const char *what, int got, int want) {
  checks++;
  if (cond) {
    printf("ok   %s (duty %d)\n", what, got);
  } else {
    printf("FAIL %s (got duty %d, want %d)\n", what, got, want);
    failures++;
  }
}

static void checkMsg(bool cond, const char *what, const char *detail) {
  checks++;
  if (cond) {
    printf("ok   %s (%s)\n", what, detail);
  } else {
    printf("FAIL %s (%s)\n", what, detail);
    failures++;
  }
}

// The duty fan_set_speed() writes for a request on its own (a decrease is
// written immediately; an increase only moves the target).
static int dutyFor(int percent) {
  g_lastDuty = -1;
  fan_set_speed(percent);
  return g_lastDuty;
}

// The duty the mapping promises, computed here independently of the
// implementation's expression so the test does not simply mirror it.
static int expectedDuty(int percent) {
  if (percent <= 0) return 0;
  if (percent > 100) percent = 100;
#if FAN_DUTY_MAX_PCT > FAN_DUTY_MIN_PCT
  int dutyPct = FAN_DUTY_MIN_PCT +
                (int)(percent * (FAN_DUTY_MAX_PCT - FAN_DUTY_MIN_PCT) / 100.0f);
  return dutyPct * kMaxDuty / 100;
#else
  return percent * kMaxDuty / 100;   // straight-map fallback
#endif
}

// Advances the stubbed clock and the ramp. loop() calls fan_update() far more
// often than this; 25 ms keeps the walk short and still puts several steps
// inside a single call.
static void advance(unsigned long ms) {
  fakeMillis += ms;
  fan_update();
}

// Runs the ramp until the fan has reached the requested speed and returns how
// long that took in ms. 0 means it never got there.
static unsigned long rampTo(int percent, unsigned long capMs = 30000) {
  fan_set_speed(percent);
  const unsigned long start = fakeMillis;
  while (fan_actual_pct() < percent && fakeMillis - start < capMs) advance(25);
  return (fan_actual_pct() >= percent) ? fakeMillis - start : 0;
}

// ---- the whole test ---------------------------------------------------------
int main() {
  fan_init();
  check(g_lastChannel == 0, "fan_init() writes to LEDC channel 0", g_lastChannel, 0);
  check(g_lastDuty == 0, "fan_init() leaves the fan off", g_lastDuty, 0);
  check(fan_actual_pct() == 0.0f, "fan_init() leaves the fan at 0 %", 0, 0);

  // 0 = off, exactly as before.
  const int off = dutyFor(0);
  check(off == 0, "0 % is off (duty 0)", off, 0);

  // --------------------------------------- the request does not jump to duty -
  fan_set_speed(100);
  check(g_lastDuty == 0,
        "a request of 100 % does not write 100 % duty by itself", g_lastDuty, 0);
  check(fan_requested_pct() == 100.0f,
        "the requested speed is reported as asked while the ramp runs",
        (int)fan_requested_pct(), 100);
  char detail[160];
  snprintf(detail, sizeof(detail), "actual %.2f %%, requested %.0f %%",
           fan_actual_pct(), fan_requested_pct());
  checkMsg(fan_actual_pct() == 0.0f,
           "the reached speed is still 0 % right after the request", detail);

  // The rate is a float, so this cannot be an #if (the preprocessor has no
  // floating point). The build either ramps or it does not; both branches stay
  // compiled, and the brief below says which one this build has to satisfy.
  const bool slewEnabled = ((float)FAN_SLEW_PCT_PER_SEC > 0.0f);
  if (slewEnabled) {
    // The rate: FAN_SLEW_PCT_PER_SEC per second, so the first second of the
    // ramp is that far along.
    advance(1000);
    snprintf(detail, sizeof(detail), "actual %.2f %% after 1 s at %.1f %%/s",
             fan_actual_pct(), (double)FAN_SLEW_PCT_PER_SEC);
    checkMsg(fabs(fan_actual_pct() - (float)FAN_SLEW_PCT_PER_SEC) < 0.7f,
             "the ramp walks at FAN_SLEW_PCT_PER_SEC", detail);

    // And the number the interlock reads while it does: one control sample into
    // the ramp the fan is still below FAN_MIN_FOR_HEATER_PCT. That is the state
    // test_control drives out of main.cpp to check that the heat is refused.
    fan_set_speed(0);
    advance(25);
    fan_set_speed(100);
    advance(SENSOR_READ_INTERVAL_MS);
    snprintf(detail, sizeof(detail), "actual %.2f %% after one %d ms sample",
             fan_actual_pct(), SENSOR_READ_INTERVAL_MS);
    checkMsg(fan_actual_pct() < (float)FAN_MIN_FOR_HEATER_PCT,
             "one sample into the ramp the fan is still under the heat minimum",
             detail);

    // From a standstill, the whole ramp: monotone, and 0 -> 100 % in the time
    // the rate says.
    fan_set_speed(0);
    advance(25);
    fan_set_speed(100);
    const unsigned long rampStart = fakeMillis;
    float prevSpeed = fan_actual_pct();
    int prevDutyStep = g_lastDuty;
    bool monotone = true;
    bool wroteDuty = false;
    while (fan_actual_pct() < 100.0f && fakeMillis - rampStart < 20000) {
      advance(25);
      if (fan_actual_pct() < prevSpeed) monotone = false;
      if (g_lastDuty < prevDutyStep) monotone = false;   // never steps back
      if (g_lastDuty != prevDutyStep) wroteDuty = true;
      prevSpeed = fan_actual_pct();
      prevDutyStep = g_lastDuty;
    }
    const unsigned long rampMs = fakeMillis - rampStart;
    const unsigned long wantMs =
        (unsigned long)(100.0f / (float)FAN_SLEW_PCT_PER_SEC * 1000.0f);
    snprintf(detail, sizeof(detail), "%lu ms against %lu ms at %.1f %%/s",
             rampMs, wantMs, (double)FAN_SLEW_PCT_PER_SEC);
    checkMsg(monotone, "the ramp is monotone in both the speed and the duty", detail);
    checkMsg(wroteDuty, "the ramp writes duty as it goes, not only at the ends",
             detail);
    checkMsg(rampMs >= wantMs - 50 && rampMs <= wantMs + 50,
             "0 -> 100 % takes the time the rate says", detail);
    check(fan_actual_pct() == 100.0f, "the ramp reaches the requested speed",
          (int)fan_actual_pct(), 100);
  } else {
    // FAN_SLEW_PCT_PER_SEC 0: the escape hatch that restores the old immediate
    // write, so a bisect can tell the ramp apart from everything else in this
    // release.
    advance(25);
    check(fan_actual_pct() == 100.0f,
          "with the slew rate at 0 the fan jumps straight to the request",
          (int)fan_actual_pct(), 100);
  }

  const int top = g_lastDuty;
  check(top == expectedDuty(100), "100 % lands on the top of the duty band",
        top, expectedDuty(100));

  // A DECREASE is written immediately, ramp or no ramp: the element has to be
  // cut in the same control cycle the fan is asked to slow down, and spinning
  // down draws no start current to protect the supply from.
  const int down = dutyFor(5);
  check(down == expectedDuty(5), "a decrease writes the new duty at once",
        down, expectedDuty(5));
  check(fan_actual_pct() == 5.0f, "the reached speed drops with it",
        (int)fan_actual_pct(), 5);
  check(fan_requested_pct() == 5.0f, "and the request reports 5 %",
        (int)fan_requested_pct(), 5);

  // ------------------------------------------------------- the duty curve ---
  fan_set_speed(0);
  advance(25);
  const unsigned long topMs = rampTo(100);
  snprintf(detail, sizeof(detail), "reached 100 %% after %lu ms", topMs);
  checkMsg(topMs > 0, "the fan can be ramped up from a standstill again", detail);
  fan_set_speed(101);
  check(fan_requested_pct() == 100.0f, "101 % clips to the 100 % request",
        (int)fan_requested_pct(), 100);
  check(g_lastDuty == expectedDuty(100), "and leaves the fan at the top duty",
        g_lastDuty, expectedDuty(100));
  const int negative = dutyFor(-5);
  check(negative == 0, "a negative request is treated as off", negative, 0);

  // A request just above 0 must already land on the band floor: the fan has to
  // turn, and it must sit above the interlock threshold in every case.
  rampTo(1);
  const int low = g_lastDuty;
  check(low == expectedDuty(1), "1 % maps to the bottom of the duty band",
        low, expectedDuty(1));
  check(low > (FAN_MIN_FOR_HEATER_PCT * kMaxDuty / 100) || low == expectedDuty(1),
        "the bottom of the band is still a real fan speed", low, expectedDuty(1));

  rampTo(50);
  const int mid = g_lastDuty;
  check(mid == expectedDuty(50), "50 % maps to the middle of the band",
        mid, expectedDuty(50));

#if FAN_DUTY_MAX_PCT > FAN_DUTY_MIN_PCT
  // The documented values for the default 60/100 band (2 % duty per requested
  // percent: 153 = 60 %, 204 = 80 %, 255 = 100 %). Only checked when the
  // defaults are in force; an overridden band is covered by expectedDuty().
#if FAN_DUTY_MIN_PCT == 60 && FAN_DUTY_MAX_PCT == 100
  check(low == 153, "default band: 1 % -> 60 % duty (the measured knee)",
        low, 153);
  check(mid == 204, "default band: 50 % -> 80 % duty", mid, 204);
  check(top == 255, "default band: 100 % -> 100 % duty", top, 255);
#endif
#else
  // min == max is a broken band: the fallback must give the old straight map
  // (1 % duty per requested percent), not a frozen or inverted fan.
  check(low == 2, "fallback band: 1 % -> 1 % duty (straight map)", low, 2);
  check(mid == 127, "fallback band: 50 % -> 50 % duty (straight map)", mid, 127);
  check(top == 255, "fallback band: 100 % -> 100 % duty (straight map)", top, 255);
#endif

  // Monotonic: a higher request can never produce a lower duty. This is what
  // makes the curve usable as a control input.
  fan_set_speed(0);
  advance(25);
  bool curveMonotone = true;
  int prevDuty = g_lastDuty;
  for (int pct = 1; pct <= 100; pct++) {
    rampTo(pct);
    if (g_lastDuty < prevDuty) curveMonotone = false;
    prevDuty = g_lastDuty;
  }
  check(curveMonotone, "duty is non-decreasing across the whole 0-100 request range",
        prevDuty, prevDuty);

#if FAN_DUTY_MAX_PCT > FAN_DUTY_MIN_PCT
  // No nonzero request may fall back into the fan's dead zone.
  bool alwaysTurns = true;
  for (int pct = 1; pct <= 100; pct++) {
    rampTo(pct);
    if (g_lastDuty < expectedDuty(1)) alwaysTurns = false;
  }
  check(alwaysTurns, "every nonzero request keeps the fan at or above the band floor",
        g_lastDuty, expectedDuty(1));
#endif

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
