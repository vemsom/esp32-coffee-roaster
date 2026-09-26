// Host-side test of the safety latch and the heater interlock.
// Build (see the compile command in the project docs):
//   g++ -std=c++17 -I stub -I <project>/include test_safety.cpp
//       <project>/src/safety.cpp <project>/src/heater_control.cpp -o test_safety
#include <Arduino.h>
#include <cstdio>

#include "safety.h"
#include "heater_control.h"
#include "config.h"
#include <Preferences.h>

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

// Whether the element is asking for power. It is what arms the stuck-probe
// check, so every scenario that is not about that check leaves it off - which
// is also what a real bench test looks like: probes frozen, heater idle.
static bool g_heatActive = false;

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
    safety_update(r, g_heatActive);
  }
}

// Mirrors what main.cpp does every sample.
static void applyMainLoopPolicy() {
  if (safety_faulted()) heater_emergency_off();
}

// Boots the safety layer from scratch. The simulated flash is wiped first so
// a latch persisted by an earlier scenario cannot leak into this one - every
// scenario that is not about persistence starts from a clean NVS.
static void freshBoot() {
  Preferences prefs;
  prefs.begin("safety");
  prefs.clear();
  prefs.end();
  safety_init();
}

// Reboots the safety layer *without* wiping NVS: this is what a power cycle
// looks like to the firmware.
static void rebootKeepingNvs() { safety_init(); }

int main() {
  // ---------------------------------------------------- healthy baseline ---
  freshBoot();
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
  safety_update(good(259.9, 30), g_heatActive);
  check(!safety_faulted(), "259.9 C stays below the 260 C limit");

  fakeMillis += 10;
  safety_update(good(260.0, 30), g_heatActive);
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
  safety_update(good(240, 30), g_heatActive);
  check(!safety_faulted(), "clears on the 10th healthy sample below the margin");
  check(safety_code() == SAFETY_OK, "fault code resets to none");

  heater_clear_emergency();
  heater_set_duty(80);
  fakeMillis += 100;
  heater_update();
  check(ssrState == HIGH, "heater conducts again after the alarm cleared");

  // --------------------------------------------------- sensor fault (BT) ---
  freshBoot();
  heater_init();
  feed(good(180, 40), 5);

  feed(btBroken(180, 40), SENSOR_FAULT_MAX_COUNT - 1);
  check(!safety_faulted(), "four faulted samples in a row do not trip");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(btBroken(180, 40), g_heatActive);
  check(safety_faulted(), "the 5th consecutive faulted sample trips");
  check(safety_code() == SAFETY_SENSOR_BT, "trip reason is bt sensor fault");
  applyMainLoopPolicy();
  fakeMillis += 100;
  heater_update();
  check(ssrState == LOW, "heater off on sensor fault");

  // a single healthy sample in between resets the fault streak
  freshBoot();
  feed(good(180, 40), 3);
  for (int i = 0; i < 8; i++) {
    feed(btBroken(180, 40), SENSOR_FAULT_MAX_COUNT - 1);
    feed(good(180, 40), 1);
  }
  check(!safety_faulted(), "alternating faults never reach the trip streak");

  // a sensor fault also needs a clean streak before it clears
  freshBoot();
  feed(good(180, 40), 1);
  feed(btBroken(180, 40), SENSOR_FAULT_MAX_COUNT);
  check(safety_faulted(), "sustained fault trips");
  for (int i = 0; i < SAFETY_CLEAR_STREAK; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    safety_update(btBroken(180, 40), g_heatActive);
  }
  check(safety_faulted(), "a still-broken probe cannot clear the latch");
  feed(good(180, 40), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "clears once the probe reads plausibly again");

  // ------------------------------------------------ hard over-temp (ET) ----
  freshBoot();
  feed(good(200, 299), 3);
  check(!safety_faulted(), "ET below its own limit does not trip");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(good(200, SAFETY_MAX_ET_TEMP_C), g_heatActive);
  check(safety_faulted() && safety_code() == SAFETY_OVER_TEMP_ET, "ET limit trips with its own code");
  feed(good(200, 250), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "ET alarm clears below its margin");

  // --------------------------------- probes disagreeing while still cold ----
  // Both are plausible on their own (inside the -10..400 style window) but
  // they cannot sit 20 C apart in the same chamber at rest: one of them is
  // wrong and there is no way to tell which.
  freshBoot();
  feed(good(20, 40), SENSOR_FAULT_MAX_COUNT - 1);
  check(!safety_faulted(), "four disagreeing samples do not trip");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(good(20, 40), g_heatActive);
  check(safety_faulted() && safety_code() == SAFETY_SPREAD,
        "cold probes that disagree trip the cross-check");
  check(strcmp(safety_code_text(), "bt/et disagree") == 0,
        "spread has its own reason text");
  feed(good(20, 21), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "probes agreeing again clear the spread alarm");

  // Once a probe is hot the difference is real physics (air vs beans) and
  // must never trip - a real roast runs ET and BT tens of degrees apart.
  freshBoot();
  feed(good(180, 40), 20);
  check(!safety_faulted(), "hot and cold probes are not compared");

  // NaN readings (nothing has ever been read successfully)
  freshBoot();
  SensorReading nan;
  nan.bt = NAN;
  nan.et = NAN;
  nan.btFault = true;
  nan.etFault = true;
  feed(nan, SENSOR_FAULT_MAX_COUNT);
  check(safety_faulted(), "NaN from a never-connected probe trips");

  // ---------------------------------------------- latch survives a reboot ---
  freshBoot();
  feed(good(260, 30), 1);
  check(safety_faulted(), "over-temp trips before the power cycle");

  rebootKeepingNvs();
  check(safety_faulted(), "the latch is still held after the power cycle");
  check(safety_code() == SAFETY_OVER_TEMP_BT, "the fault reason survives the power cycle too");
  heater_init();
  applyMainLoopPolicy();
  fakeMillis += 100;
  heater_update();
  check(ssrState == LOW, "heater is held off when booting with a restored alarm");

  // A restored latch follows the normal clear rule - it is the same alarm
  // that was already running, not a permanent lock-out.
  feed(good(240, 30), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "restored latch clears on the healthy streak");
  rebootKeepingNvs();
  check(!safety_faulted(), "the cleared state is persisted as well");

  freshBoot();
  check(!safety_faulted(), "an empty store boots into the cleared state");

  // A record that cannot be trusted is healed instead of obeyed.
  {
    Preferences corrupted;
    corrupted.begin("safety");
    corrupted.putBool("latched", true);
    corrupted.putInt("code", 4242);
    corrupted.end();
  }
  rebootKeepingNvs();
  check(!safety_faulted(), "an out-of-range stored reason is ignored, not obeyed");

  // NVS that will not open: the alarm still trips, it just lives in RAM only.
  // Degraded and logged, never silent.
  freshBoot();
  preferencesFailBegin() = true;
  rebootKeepingNvs();
  check(!safety_faulted(), "with NVS unavailable a boot starts clear");
  feed(good(260, 30), 1);
  check(safety_faulted(), "the latch still trips when NVS is unavailable");
  rebootKeepingNvs();
  check(!safety_faulted(), "without NVS the latch does not survive a reboot");
  preferencesFailBegin() = false;
  freshBoot();

  // ------------------------------------------------------- stuck probe -------
  // Idle machine: both probes frozen on the same value for two minutes. That
  // is exactly what a healthy cold roaster looks like, and the element is off.
  freshBoot();
  g_heatActive = false;
  feed(good(20, 20), 480);
  check(!safety_faulted(), "frozen probes with the element off never trip");

  // Heat on, probes moving: a real roast never sits still for a minute.
  g_heatActive = true;
  for (int i = 0; i < 240; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    safety_update(good(150.0f + (i % 4) * 0.25f, 120.0f + (i % 4) * 0.25f), true);
  }
  check(!safety_faulted(), "probes that keep moving with heat on never trip");

  // Heat on, BT frozen: trips exactly at the window, not a sample earlier.
  // First sample only starts the timer, so it takes
  // SENSOR_STUCK_MAX_MS / SENSOR_READ_INTERVAL_MS + 1 samples to trip.
  freshBoot();
  g_heatActive = true;
  feed(good(180, 40), SENSOR_STUCK_MAX_MS / SENSOR_READ_INTERVAL_MS);
  check(!safety_faulted(), "identical readings just under the window do not trip");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(good(180, 40), true);
  check(safety_faulted(), "identical readings for the full window with heat on trip");
  check(safety_code() == SAFETY_STUCK_BT, "trip reason is bt sensor stuck");
  check(strcmp(safety_code_text(), "bt sensor stuck") == 0,
        "stuck has its own reason text");

  // The latch does not release by itself. Every one of these samples looks
  // perfectly healthy - that is what "stuck" means - so the ordinary clear
  // streak would otherwise silence the alarm 2.5 s after it tripped.
  applyMainLoopPolicy();
  g_heatActive = false;   // the trip aborts the run, so the element is off
  feed(good(180, 40), SAFETY_CLEAR_STREAK * 4);
  check(safety_faulted(), "healthy-looking samples alone do not release a stuck alarm");
  check(safety_code() == SAFETY_STUCK_BT, "the reason stays the stuck probe");

  // Proof of movement is what releases it.
  feed(good(180.25f, 40), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "a different value from the frozen channel clears it");

  // Cooling: the reading stalls while the element has been off for minutes.
  // This is the tail of every cool cycle and must never false-alarm.
  freshBoot();
  g_heatActive = true;
  feed(good(60, 55), 2);      // heat on, value recorded
  g_heatActive = false;
  feed(good(60, 55), 960);    // four minutes of identical readings, no heat
  check(!safety_faulted(), "a stalled reading while cooling does not trip");

  // The other channel must not mask a frozen one: BT keeps moving the whole
  // time while ET sits still (both stay inside the cold-spread window).
  freshBoot();
  g_heatActive = true;
  for (int i = 0; i < SENSOR_STUCK_MAX_MS / SENSOR_READ_INTERVAL_MS; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    safety_update(good(30.0f + (i % 20) * 0.25f, 40.0f), true);
  }
  check(!safety_faulted(), "a moving BT keeps the pair healthy for the whole window");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(good(30.0f + (240 % 20) * 0.25f, 40.0f), true);
  check(safety_faulted() && safety_code() == SAFETY_STUCK_ET,
        "a frozen ET trips even while BT keeps moving");

  // Power cycle during a stuck alarm - the case the NVS latch and the
  // proof rule exist for together.
  freshBoot();
  g_heatActive = true;
  feed(good(180, 40), SENSOR_STUCK_MAX_MS / SENSOR_READ_INTERVAL_MS + 1);
  check(safety_faulted() && safety_code() == SAFETY_STUCK_BT,
        "stuck probe trips before the power cycle");
  g_heatActive = false;
  rebootKeepingNvs();
  check(safety_faulted() && safety_code() == SAFETY_STUCK_BT,
        "a stuck alarm survives the power cycle");
  feed(good(180, 40), SAFETY_CLEAR_STREAK * 4);
  check(safety_faulted(), "and is not cleared by healthy-looking samples after the reboot");
  feed(good(179.75f, 40), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "it clears once the probe shows a different value");

  g_heatActive = false;
  freshBoot();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
