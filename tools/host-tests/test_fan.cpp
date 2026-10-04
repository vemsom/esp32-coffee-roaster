// Host-side test of the fan output curve (src/fan_control.cpp).
//
// fan_set_speed() keeps its percent API: 0 = off, 1-100 = the logical speed
// the web UI, profile steps, MQTT/HA and the fan interlock talk. What this
// test pins is the other half: the duty that reaches the LEDC channel. A DC
// fan driven through the MOSFET stands still below its start threshold, so the
// request is mapped onto the band it actually responds in instead of onto a
// straight 0-100 duty.
//
// Built twice by tools/host-tests/run.sh:
//   * the default config (FAN_DUTY_MIN_PCT/FAN_DUTY_MAX_PCT from config.h)
//   * a misconfigured band with min == max, to pin the straight-map fallback
// The band is a compile-time constant, so the #if selects which promises this
// build checks; the recording ledcWrite stub is shared.
#include <Arduino.h>
#include <cstdio>

#include "config.h"
#include "fan_control.h"

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

// The duty fan_set_speed() writes for a request, captured through the stub.
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
                percent * (FAN_DUTY_MAX_PCT - FAN_DUTY_MIN_PCT) / 100;
  return dutyPct * kMaxDuty / 100;
#else
  return percent * kMaxDuty / 100;   // straight-map fallback
#endif
}

int main() {
  fan_init();
  check(g_lastChannel == 0, "fan_init() writes to LEDC channel 0", g_lastChannel, 0);
  check(g_lastDuty == 0, "fan_init() leaves the fan off", g_lastDuty, 0);

  // 0 = off, exactly as before.
  int off = dutyFor(0);
  check(off == 0, "0 % is off (duty 0)", off, 0);

  // The endpoints of the requested range.
  int top = dutyFor(100);
  check(top == expectedDuty(100), "100 % is the top of the duty band",
        top, expectedDuty(100));
  int clipped = dutyFor(101);
  check(clipped == top, "101 % clips to the 100 % duty", clipped, top);
  int negative = dutyFor(-5);
  check(negative == 0, "a negative request is treated as off", negative, 0);

  // A request just above 0 must already land on the band floor: the fan has to
  // turn, and it must sit above the interlock threshold in every case.
  int low = dutyFor(1);
  check(low == expectedDuty(1), "1 % maps to the bottom of the duty band",
        low, expectedDuty(1));
  check(low > (FAN_MIN_FOR_HEATER_PCT * kMaxDuty / 100) || low == expectedDuty(1),
        "the bottom of the band is still a real fan speed", low, expectedDuty(1));

  int mid = dutyFor(50);
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
  bool monotonic = true;
  int prev = dutyFor(0);
  for (int pct = 1; pct <= 100; pct++) {
    int duty = dutyFor(pct);
    if (duty < prev) monotonic = false;
    prev = duty;
  }
  check(monotonic, "duty is non-decreasing across the whole 0-100 request range",
        prev, prev);

#if FAN_DUTY_MAX_PCT > FAN_DUTY_MIN_PCT
  // No nonzero request may fall back into the fan's dead zone.
  bool alwaysTurns = true;
  for (int pct = 1; pct <= 100; pct++) {
    if (dutyFor(pct) < expectedDuty(1)) alwaysTurns = false;
  }
  check(alwaysTurns, "every nonzero request keeps the fan at or above the band floor",
        dutyFor(1), expectedDuty(1));
#endif

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
