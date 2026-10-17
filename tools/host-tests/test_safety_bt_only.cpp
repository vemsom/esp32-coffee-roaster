// Host-side test of the BT-only build: SAFETY_ET_ENABLED 0 in include/config.h.
// Build: see the BT-only pass in run.sh (compiled with -DSAFETY_ET_ENABLED=0).
//
// The other safety test (test_safety.cpp) pins what the DEFAULT build does,
// including every ET trip. This file exists for the opposite claim: with the
// ET channel switched off, an ET reading - missing, faulted, absurdly high,
// cold and in disagreement with BT, or frozen while the element is on - can
// never latch the alarm, and the latch a BT fault does set clears on a streak
// of healthy *BT* samples even though ET is dead the whole time. That is the
// point of the mode: a broken ET module must not be able to hold the machine
// off, and it must not leave a hole where BT's own checks used to have a
// backstop.
//
// It reads SensorReading structs directly, the way src/sensors.cpp hands them
// over, so it tests the safety layer on its own. What sensors.cpp does with the
// flag (report NaN for ET, never read the converter) is asserted in
// test_sensors.cpp and, end to end, in test_control.cpp.
#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "safety.h"
#include "heater_control.h"
#include "config.h"
#include <Preferences.h>

// Building this file without the flag would test the wrong firmware: the ET
// trips it asserts the absence of are back, and the ET readings it feeds would
// largely trip. Fail loudly instead of testing something else.
#if SAFETY_ET_ENABLED
#error "this file pins the BT-only build - compile it with -DSAFETY_ET_ENABLED=0 (see run.sh)"
#endif

// ---- Arduino stubs (same set as test_safety.cpp) ----
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

// Whether the element is asking for power right now. It is what arms the
// stuck-probe check, so every scenario that is not about that check leaves it
// off - which is also what a real bench test looks like: probes frozen, heater
// idle.
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

// A sample exactly as src/sensors.cpp produces one: the value plus the fault
// flag its plausibility/jump checks set. In this build sensors.cpp always
// hands over et = NaN with etFault = false ("not read"), and the readings below
// deliberately are not that: they are values and faults that WOULD trip the
// full build, so that "they cannot trip here" is a claim about the flag and not
// about the fixture.
static SensorReading sample(float bt, bool btFault, float et, bool etFault) {
  SensorReading r;
  r.bt = bt;
  r.btFault = btFault;
  r.et = et;
  r.etFault = etFault;
  return r;
}

static SensorReading btLive(float bt) { return sample(bt, false, NAN, false); }
static SensorReading btLiveEtWild(float bt, float et) { return sample(bt, false, et, false); }
static SensorReading btLiveEtFault(float bt, float lastGoodEt) {
  return sample(bt, false, lastGoodEt, true);
}
static SensorReading btFaulted(float lastGoodBt) { return sample(lastGoodBt, true, NAN, false); }

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

// Boots the safety layer from scratch, with the simulated flash wiped first:
// a latch persisted by an earlier scenario must not leak into this one.
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

// Writes an alarm record the way a full build would have left it behind - the
// case that matters when this image is flashed over a machine that is currently
// latched on an ET reason.
static void storeAlarm(SafetyFaultCode code) {
  Preferences prefs;
  prefs.begin("safety");
  prefs.putInt("code", (int32_t)code);
  prefs.putBool("latched", true);
  prefs.end();
}

int main() {
  // ================= 1. no ET reading can latch anything =====================
  freshBoot();
  heater_init();
  fakeMillis = 0;

  // The situation the build exists for: BT reads live, ET reports nothing.
  feed(btLive(24.5f), 40);
  check(!safety_faulted(), "an ET that never reads (NaN) never trips the latch");
  check(safety_code() == SAFETY_OK, "and leaves no fault code behind");
  check(strcmp(safety_code_text(), "et disabled - bt only") == 0,
        "a clear latch names the disabled channel instead of reporting 'none'");

  // An ET that is faulted for far longer than any streak would need.
  feed(btLiveEtFault(24.5f, 25.0f), SENSOR_FAULT_MAX_COUNT * 10);
  check(!safety_faulted(), "a sustained ET sensor fault cannot latch here");

  // An ET that reports impossible values: above its own ceiling, at 0 C (what
  // a floating data line gives), and faulted with no good value ever read.
  feed(btLiveEtWild(24.5f, SAFETY_MAX_ET_TEMP_C + 100.0f), 40);
  check(!safety_faulted(), "an ET above SAFETY_MAX_ET_TEMP_C cannot latch");
  feed(btLiveEtWild(24.5f, 0.0f), 40);
  check(!safety_faulted(), "a 0 C ET (a floating module) cannot latch");
  feed(btLiveEtFault(24.5f, NAN), 40);
  check(!safety_faulted(), "an ET fault with no last good value cannot latch");

  // The cross-check: two cold channels 25 C apart. In a full build this is the
  // trip - it is the one check that sees a probe that is wrong but individually
  // plausible - and with one channel there is nothing to compare against.
  feed(btLiveEtWild(20.0f, 45.0f), SENSOR_FAULT_MAX_COUNT * 4);
  check(!safety_faulted(),
        "a cold BT/ET disagreement cannot trip: the cross-check cannot be evaluated");

  // A frozen ET for the full stuck window, with the element asking for power
  // the whole time. BT keeps moving, so nothing else can trip on this run.
  freshBoot();
  g_heatActive = true;
  for (int i = 0; i < SENSOR_STUCK_MAX_MS / SENSOR_READ_INTERVAL_MS + 4; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    safety_update(btLiveEtWild(30.0f + (i % 20) * 0.25f, 40.0f), true);
  }
  check(!safety_faulted(),
        "an ET frozen for the whole window with heat on cannot latch: only BT is watched");
  g_heatActive = false;

  // ================= 2. BT's own checks still carry the alarm ================
  // The bench case the control test uses: nothing is wired, the data line
  // floats and reads 0 C, which sensors.cpp turns into a BT fault. What trips
  // here is BT's plausibility floor and nothing else - there is no ET backstop
  // left, and the machine must still refuse to heat.
  freshBoot();
  g_heatActive = false;
  feed(btFaulted(NAN), SENSOR_FAULT_MAX_COUNT - 1);
  check(!safety_faulted(), "four faulted BT samples do not trip yet");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(btFaulted(NAN), false);
  check(safety_faulted(), "a BT that never read trips on the 5th faulted sample");
  check(safety_code() == SAFETY_SENSOR_BT, "the reason is bt sensor fault");
  check(strcmp(safety_code_text(), "bt sensor fault") == 0,
        "the BT-only alarm keeps the BT reason text");

  applyMainLoopPolicy();
  fakeMillis += 100;
  heater_update();
  check(ssrState == LOW, "heater is off while the BT alarm is latched");
  heater_set_duty(100);
  fakeMillis += 100;
  heater_update();
  check(ssrState == LOW, "heater_set_duty(100) cannot override it");
  check(heater_emergency_active(), "heater reports the emergency latch");

  // ================= 3. it clears on BT's streak, and only BT's =============
  feed(btFaulted(NAN), SAFETY_CLEAR_STREAK * 4);
  check(safety_faulted(), "a still-broken BT cannot clear the latch");

  feed(btLive(24.0f), SAFETY_CLEAR_STREAK - 1);
  check(safety_faulted(), "still latched on sample 9 of the BT clear streak");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(btLive(24.0f), false);
  check(!safety_faulted(), "clears on the 10th healthy BT sample");
  check(safety_code() == SAFETY_OK, "the fault code resets to none");
  check(strcmp(safety_code_text(), "et disabled - bt only") == 0,
        "the cleared reason still names the disabled channel");

  heater_clear_emergency();
  heater_set_duty(80);
  fakeMillis += 100;
  heater_update();
  check(ssrState == HIGH, "heater conducts again after the alarm cleared");

  // The ET channel keeps being dead - that is the whole point - and it may not
  // re-latch what BT just cleared.
  feed(btLiveEtFault(24.0f, 25.0f), SENSOR_FAULT_MAX_COUNT * 4);
  check(!safety_faulted(), "an ET fault cannot re-trip a cleared BT-only latch");

  // ================= 4. BT's remaining checks, one by one ===================
  // Hard ceiling.
  freshBoot();
  feed(btLive(259.9f), 3);
  check(!safety_faulted(), "259.9 C stays below the 260 C limit");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(btLive(260.0f), false);
  check(safety_faulted() && safety_code() == SAFETY_OVER_TEMP_BT,
        "260 C still trips bt over temp");
  feed(btLive(240.0f), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "and clears below the margin on the healthy streak");

  // A BT fault streak from any cause (an open probe, a jump) is what the sensor
  // layer hands over; safety has to trip on it. The jump check itself lives in
  // sensors.cpp and is asserted in test_sensors, which runs in this build too.
  freshBoot();
  feed(btLive(180.0f), 3);
  feed(btFaulted(180.0f), SENSOR_FAULT_MAX_COUNT);
  check(safety_faulted() && safety_code() == SAFETY_SENSOR_BT,
        "a BT fault streak (an open probe, a jump) still trips");
  freshBoot();

  // The stuck-BT detector: the only stuck trip left in this build, and it has
  // to work, because a frozen BT under heat is the case with no backstop at all
  // (SAFETY_MAX_ET_TEMP_C used to be it).
  g_heatActive = true;
  feed(btLive(180.0f), SENSOR_STUCK_MAX_MS / SENSOR_READ_INTERVAL_MS);
  check(!safety_faulted(), "identical BT readings just under the window do not trip");
  fakeMillis += SENSOR_READ_INTERVAL_MS;
  safety_update(btLive(180.0f), true);
  check(safety_faulted() && safety_code() == SAFETY_STUCK_BT,
        "a frozen BT with heat on still trips");
  g_heatActive = false;
  feed(btLive(180.0f), SAFETY_CLEAR_STREAK * 4);
  check(safety_faulted(), "healthy-looking samples alone do not release a stuck alarm");
  feed(btLive(180.25f), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "a value that differs from the frozen one clears it");
  freshBoot();

  // ================= 5. a stored ET alarm cannot lock the machine out =======
  // The machine can be latched on an ET reason when this image is flashed over
  // it: the latch lives in NVS, and the channel that tripped it is gone. It
  // must still clear - on the BT streak - and the restored stuck-ET case in
  // particular must not be re-armed into the proof rule, whose evidence (a
  // value that differs from the frozen one) can never arrive when ET is not
  // read. Getting that wrong would leave the heater locked out for good.
  freshBoot();
  storeAlarm(SAFETY_SENSOR_ET);
  rebootKeepingNvs();
  check(safety_faulted(), "a stored ET sensor alarm is restored");
  check(strcmp(safety_code_text(), "et sensor fault") == 0,
        "and keeps its own reason text while it is held");
  heater_init();
  applyMainLoopPolicy();
  fakeMillis += 100;
  heater_update();
  check(ssrState == LOW, "heater is held off while that stored alarm is latched");
  feed(btLive(24.0f), SAFETY_CLEAR_STREAK);
  check(!safety_faulted(), "the BT clear streak releases a stored ET alarm");

  freshBoot();
  storeAlarm(SAFETY_STUCK_ET);
  rebootKeepingNvs();
  check(safety_faulted() && safety_code() == SAFETY_STUCK_ET,
        "a stored stuck-ET alarm is restored as well");
  feed(btLive(24.0f), SAFETY_CLEAR_STREAK * 4);
  check(!safety_faulted(),
        "a stored stuck-ET alarm clears on the BT streak instead of waiting for an ET value");
  check(strcmp(safety_code_text(), "et disabled - bt only") == 0,
        "and the reason falls back to naming the disabled channel");

  freshBoot();
  check(!safety_faulted(), "an empty store boots into the cleared state");

  g_heatActive = false;
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
