// Host-side test of the safety latch and the heater interlock.
// Build (see the compile command in the project docs):
//   g++ -std=c++17 -I stub -I <project>/include test_safety.cpp
//       <project>/src/safety.cpp <project>/src/heater_control.cpp -o test_safety
#include <Arduino.h>
#include <cstdio>

#include "safety.h"
#include "heater_control.h"
#include "config.h"

// ---- Arduino stubs ----
static unsigned long fakeMillis = 0;
unsigned long millis() { return fakeMillis; }
void delay(unsigned long) {}
void delayMicroseconds(unsigned int) {}

static int ssrState = -1;
void pinMode(int, int) {}
void digitalWrite(int pin, int value) {
  if (pin == PIN_SSR_HEATER) ssrState = value;
}
int digitalRead(int) { return 0; }

SerialStub Serial;

// ---- test helpers ----
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

static SensorReading good(float bt, float et) {
  SensorReading r;
  r.bt = bt;
  r.et = et;
  r.btFault = false;
  r.etFault = false;
  return r;
}

static SensorReading btBroken(float lastGood, float et) {
  SensorReading r;
  r.bt = lastGood;  // sensors.cpp hands back the last good value on a fault
  r.et = et;
  r.btFault = true;
  r.etFault = false;
  return r;
}

static void feed(const SensorReading &r, int times) {
  for (int i = 0; i < times; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    safety_update(r);
  }
}

// Mirrors what main.cpp does every sample.
static void applyMainLoopPolicy() {
  if (safety_faulted()) heater_emergency_off();
}

int main() {
  // ---------------------------------------------------- healthy baseline ---
  safety_init();
  heater_init();
  fakeMillis = 0;

  feed(good(20, 20), 20);
  check(!safety_faulted(), "healthy readings do not trip");

  heater_set_duty(80);
  fakeMillis += 100;
  heater_update();
  check(ssrState == HIGH, "heater conducts when commanded and not latched");

  // ------------------------------------------------- hard over-temp (BT) ---
  fakeMillis += 10;
  safety_update(good(259.9, 30));
  check(!safety_faulted(), "259.9 C stays below the 260 C limit");

  fakeMillis += 10;
  safety_update(good(260.0, 30));
  check(safety_faulted(), "260.0 C trips the latch");
  check(safety_code() == SAFETY_OVER_TEMP_BT, "trip reason is bt over temp");

  applyMainLoopPolicy();
  fakeMillis += 100;
  heater_update();
  check(ssrState == LOW, "heater is off while latched");

  heater_set_duty(100);  // a command must not get through the latch
  fakeMillis += 100;
  heater_update();
  check(ssrState == LOW, "heater_set_duty(100) cannot override the latch");
  check(heater_emergency_active(), "heater reports emergency active");

  // no command may silence it: only the condition going away
  feed(good(255, 30), 10);  // below the limit, but inside the clear margin
  check(safety_faulted(), "latched until the clear margin (250 C) is reached");
  feed(good(240, 30), 9);
  check(safety_faulted(), "still latched on sample 9 of the clear streak");

  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(good(240, 30));
  check(!safety_faulted(), "clears on the 10th healthy sample below the margin");
  check(safety_code() == SAFETY_OK, "fault code resets to none");

  heater_clear_emergency();
  heater_set_duty(80);
  fakeMillis += 100;
  heater_update();
  check(ssrState == HIGH, "heater conducts again after the alarm cleared");

  // --------------------------------------------------- sensor fault (BT) ---
  safety_init();
  heater_init();
  feed(good(180, 40), 5);

  feed(btBroken(180, 40), SENSOR_FAULT_MAX_COUNT - 1);
  check(!safety_faulted(), "four faulted samples in a row do not trip");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(btBroken(180, 40));
  check(safety_faulted(), "the 5th consecutive faulted sample trips");
  check(safety_code() == SAFETY_SENSOR_BT, "trip reason is bt sensor fault");
  applyMainLoopPolicy();
  fakeMillis += 100;
  heater_update();
  check(ssrState == LOW, "heater off on sensor fault");

  // a single healthy sample in between resets the fault streak
  safety_init();
  feed(good(180, 40), 3);
  for (int i = 0; i < 8; i++) {
    feed(btBroken(180, 40), SENSOR_FAULT_MAX_COUNT - 1);
    feed(good(180, 40), 1);
  }
  check(!safety_faulted(), "alternating faults never reach the trip streak");

  // a sensor fault also needs a clean streak before it clears
  safety_init();
  feed(good(180, 40), 1);
  feed(btBroken(180, 40), SENSOR_FAULT_MAX_COUNT);
  check(safety_faulted(), "sustained fault trips");
  for (int i = 0; i < SAFETY_CLEAR_STREAK; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    safety_update(btBroken(180, 40));
  }
  check(safety_faulted(), "a still-broken probe cannot clear the latch");
  feed(good(180, 40), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "clears once the probe reads plausibly again");

  // ------------------------------------------------ hard over-temp (ET) ----
  safety_init();
  feed(good(200, 299), 3);
  check(!safety_faulted(), "ET below its own limit does not trip");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(good(200, SAFETY_MAX_ET_TEMP_C));
  check(safety_faulted() && safety_code() == SAFETY_OVER_TEMP_ET, "ET limit trips with its own code");
  feed(good(200, 250), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "ET alarm clears below its margin");

  // NaN readings (nothing has ever been read successfully)
  safety_init();
  SensorReading nan;
  nan.bt = NAN;
  nan.et = NAN;
  nan.btFault = true;
  nan.etFault = true;
  feed(nan, SENSOR_FAULT_MAX_COUNT);
  check(safety_faulted(), "NaN from a never-connected probe trips");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
