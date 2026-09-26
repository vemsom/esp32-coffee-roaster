// Host-side test of the control path in src/main.cpp. The other two tests
// cover the safety latch and the MQTT payloads in isolation; this one runs the
// real setup()/loop() with stubbed hardware so the path the web UI takes -
// manual mode -> PID -> heater - is the code that actually ships.
//
// Build: see the third compile line in run.sh. main.cpp has no main(), so this
// file supplies it, the callbacks main.cpp registers (captured, so the test
// calls exactly what the HTTP handlers call) and a canned RoastProfile.
//
// Covered:
//   * the bench case: probes disconnected, every reading 0 C
//   * manual start denied while the latch is held
//   * the fan interlock in manual AND profile mode
//   * a probe pulled mid-run aborting a running manual heat
//   * the MQTT status payload carrying both fault flags
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <max6675.h>
#include <PubSubClient.h>
#include <cstdio>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

#include "config.h"
#include "safety.h"
#include "heater_control.h"
#include "web_server.h"
#include "mqtt_client.h"
#include "roast_profile.h"
#include "state_lock.h"

void setup();
void loop();

// ---- Arduino / runtime stubs ------------------------------------------------
// The clock is atomic because the concurrency stress test runs loop() on this
// thread while the writer thread calls millis() through the web callbacks.
static std::atomic<unsigned long> fakeMillis{0};
unsigned long millis() { return fakeMillis.load(); }
// Advances the clock: the WiFi service retries on a timer, and a no-op delay
// would spin forever on a fixed clock.
void delay(unsigned long ms) { fakeMillis += ms; }
void delayMicroseconds(unsigned int) {}

static int ssrState = -1;
static bool ssrEverHigh = false;   // latched: was the heater pin ever driven high?
void pinMode(int, int) {}
void digitalWrite(int pin, int value) {
  if (pin != PIN_SSR_HEATER) return;
  ssrState = value;
  if (value == HIGH) ssrEverHigh = true;
}
int digitalRead(int) { return 0; }
long map(long x, long inMin, long inMax, long outMin, long outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}
void ledcSetup(int, double, int) {}
void ledcAttachPin(int, int) {}
void ledcWrite(int, int) {}
SerialStub Serial;
WiFiClass WiFi;
LittleFSClass LittleFS;

// ---- probe readings injected into the MAX6675 stubs ------------------------
float g_probeBT = 20.0f;
float g_probeET = 20.0f;

// ---- recording globals required by the PubSubClient stub -------------------
std::vector<CapturedPublish> g_published;
std::vector<std::string> g_subscriptions;
std::string g_connectUser, g_connectPass, g_connectClientId, g_connectWillTopic;
PubSubCallback g_callback = nullptr;
uint16_t g_bufferSize = 256;
bool g_connected = false;

// ---- callbacks captured from setup() ---------------------------------------
static WebServerCallbacks web;
static bool webCaptured = false;
void web_server_init(WebServerCallbacks callbacks) {
  web = callbacks;
  webCaptured = true;
}

// ---- canned RoastProfile (the real one reads LittleFS) ---------------------
static float g_profileFan = 80.0f;
bool RoastProfile::loadFromFile(const String &) {
  _hasFan = true;
  _startTemp = 20;
  _steps = { ProfileStep{ 0, 600, 200, g_profileFan } };
  return true;
}
bool RoastProfile::saveToFile(const String &) const { return true; }
void RoastProfile::addStep(unsigned long rampSeconds, unsigned long holdSeconds,
                           float temp, float fan) {
  _steps.push_back(ProfileStep{ rampSeconds, holdSeconds, temp, fan });
  _hasFan = true;
}
void RoastProfile::clear() { _steps.clear(); _hasFan = false; }
float RoastProfile::targetAt(unsigned long) const { return 200; }
float RoastProfile::fanAt(unsigned long) const { return g_profileFan; }

// ---- helpers ----------------------------------------------------------------
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

// Runs the real loop() at the real sample cadence (SENSOR_READ_INTERVAL_MS).
static void runLoops(int samples) {
  for (int i = 0; i < samples; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
}

// Same, with both probes drifting upwards together: real probes never sit on
// an identical value forever, and the stuck-sensor check (safety.cpp) would
// notice if they did. Used by the concurrency stress test.
static void runLoopsWithMovingProbes(int samples) {
  for (int i = 0; i < samples; i++) {
    g_probeBT = g_probeET = 20.0f + i * 0.25f;
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
}

static std::string lastStatusPayload() {
  for (auto it = g_published.rbegin(); it != g_published.rend(); ++it) {
    if (it->topic == MQTT_BASE_TOPIC "/status") return it->payload;
  }
  return "";
}

static bool statusHas(const char *needle) {
  return lastStatusPayload().find(needle) != std::string::npos;
}

int main() {
  // ------------------------------------------------------------- bench case -
  // Nothing is wired to the ESP32: both probes report a steady 0 C, which is
  // what a floating MAX6675 data line does. That used to sit inside the
  // -10..400 C plausibility window, so the alarm never fired and a manual run
  // kept heating on a fabricated reading.
  g_probeBT = 0.0f;
  g_probeET = 0.0f;

  // WiFi is down at boot on purpose. The old setup() spun for up to 15 s
  // waiting for an association that never comes, so a roaster with a missing
  // AP took 15 s to react to anything at all. It must return promptly now.
  WiFi.statusValue = WL_DISCONNECTED;
  const unsigned long bootStart = fakeMillis;
  setup();
  check(fakeMillis - bootStart < 2000, "setup() returns without waiting for WiFi");
  check(webCaptured, "main.cpp registers its web callbacks");
  check(!web.getWifiConnected(), "status reports WiFi down while the link is down");
  const int connectAttempts = WiFi.beginCount;
  check(connectAttempts >= 1, "setup() starts one connect attempt and moves on");

  // Boot safety, asserted before a single control cycle runs: setup() drives
  // the heater pin low as its very first act and never drives it high while
  // bringing the rest of the system up. This covers what the code controls;
  // the window between reset and setup() is a hardware question (pull-down on
  // SSR IN+), which no test can answer.
  check(ssrState == LOW, "heater pin is driven low by setup() before the first loop");
  check(!ssrEverHigh, "setup() never drives the heater pin high while initialising");
  check(!heater_emergency_active(), "heater starts out of its emergency latch");

  runLoops(24);   // also long enough for the first MQTT connect + status

  check(safety_faulted(), "0 C probes trip the latch");
  check(safety_code() == SAFETY_SENSOR_BT, "trip reason is bt sensor fault");
  check(!web.getManualActive(), "no manual run is running");
  check(web.getHeaterDuty() == 0, "reported duty is 0, not 100 %");
  check(ssrState == LOW, "heater pin is low");

  // The control loop has kept running with no network at all: the safety
  // layer tripped, the heater is held down, nothing waited on an AP.
  // The retry kick fires after WIFI_RETRY_INTERVAL_MS with the link still down.
  fakeMillis += WIFI_RETRY_INTERVAL_MS;
  loop();
  check(WiFi.beginCount == connectAttempts + 1,
        "a fresh connect attempt is kicked off while the link stays down");

  // Link comes up: the status follows, and no control state was disturbed.
  WiFi.statusValue = WL_CONNECTED;
  runLoops(1);
  check(web.getWifiConnected(), "status reports WiFi connected once the link is up");
  check(safety_faulted(), "the latch held across the WiFi outage");

  // The required path: manual mode must be denied while the alarm is held.
  check(!web.startManual(200, 600, false, 0, 0), "manual start is denied in alarm state");
  runLoops(2);
  check(!web.getManualActive(), "denied start does not activate manual mode");

  // ...and the latch is deeper than the mode: a direct duty command cannot
  // re-energise the element even though no run is active.
  heater_set_duty(100);
  fakeMillis += 100;
  heater_update();
  check(ssrState == LOW, "heater_set_duty(100) cannot override the alarm");
  check(heater_emergency_active(), "heater reports the emergency latch");
  check(web.getSafetyFault(), "status reports the safety fault");

  // ------------------------------------------------- probes come back alive -
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  runLoops(SAFETY_CLEAR_STREAK + 4);
  check(!safety_faulted(), "latch clears once the probes read plausibly again");
  check(!heater_emergency_active(), "heater is re-armed after the clear streak");

  // ------------------------------------------- fan interlock, manual mode ----
  // Fan at 0 %: the run may be armed but nothing may heat.
  check(web.startManual(200, 600, false, 0, 0), "manual run starts with a healthy state");
  runLoops(4);
  check(web.getManualActive(), "manual run is active");
  check(web.getFanFault(), "fan below minimum with heat requested flags the interlock");
  check(web.getHeaterDuty() == 0, "no heat is allowed without airflow");
  check(ssrState == LOW, "element stays off at 0 % fan");
  check(!web.getSafetyFault(), "the interlock is not reported as a sensor alarm");
  runLoops(10);   // long enough for an MQTT status tick while blocked
  check(statusHas("\"fanFault\":true"), "mqtt status carried fanFault while blocked");
  check(statusHas("\"safetyFault\":false"), "mqtt status does not report a sensor alarm");

  // Fan up to 100 %: heat is allowed.
  web.setFanSpeed(100);
  runLoops(4);
  check(!web.getFanFault(), "interlock clears once the fan reaches the minimum");
  check(web.getHeaterDuty() > 0, "heat is allowed with airflow");
  check(ssrState == HIGH, "element conducts with airflow");

  // Fan drops below 10 % while heating: cut on the very next sample.
  web.setFanSpeed(5);
  runLoops(2);
  check(web.getFanFault(), "fan dropping below the minimum flags the interlock");
  check(web.getHeaterDuty() == 0, "heat is cut the cycle the fan drops");
  check(ssrState == LOW, "element off while the fan is too slow");
  check(!web.getSafetyFault(), "still not the sensor alarm");

  // Fan back: the interlock is self-clearing.
  web.setFanSpeed(100);
  runLoops(4);
  check(!web.getFanFault(), "interlock clears when the fan comes back");
  check(web.getHeaterDuty() > 0 && ssrState == HIGH, "heating resumes with airflow");

  // -------------------------------------------- fan interlock, auto mode -----
  web.stopManual();
  runLoops(2);
  g_profileFan = 5.0f;
  check(web.startRoast(String("test")), "profile run starts");
  runLoops(4);
  check(web.getFanFault(), "profile mode is interlocked too");
  check(web.getHeaterDuty() == 0, "auto mode heats nothing without airflow");
  check(ssrState == LOW, "element off in auto mode below the fan minimum");

  g_profileFan = 80.0f;
  runLoops(4);
  check(!web.getFanFault() && web.getHeaterDuty() > 0, "auto mode heats with airflow");
  check(ssrState == HIGH, "element conducts in auto mode");
  web.stopRoast();
  runLoops(2);

  // --------------------------------- probe pulled during a manual run --------
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  runLoops(4);
  check(web.startManual(200, 600, false, 0, 0), "manual run starts again");
  runLoops(4);
  check(web.getManualActive() && web.getHeaterDuty() > 0, "manual run is heating");

  g_probeBT = 0.0f;   // thermocouple comes loose mid-run
  runLoops(SENSOR_FAULT_MAX_COUNT + 3);
  check(safety_faulted(), "a probe pulled mid-run trips the latch");
  check(!web.getManualActive(), "the running manual heat is aborted");
  check(web.getHeaterDuty() == 0, "duty drops to 0 on abort");
  check(ssrState == LOW, "element off after the abort");
  check(!web.startManual(200, 600, false, 0, 0), "restart stays denied while latched");
  runLoops(8);
  check(statusHas("\"safetyFault\":true"), "mqtt status carried the safety fault");

  // ----------------------------------------------- concurrency (state lock) --
  // The lock has to be taken by the web callbacks, and from a single-threaded
  // test the acquisition counter is the only way to see that.
  const unsigned long lockBefore = state_lock_acquisitions();
  (void)web.getBT();
  (void)web.getFanSpeed();
  (void)web.getSafetyFault();
  check(state_lock_acquisitions() == lockBefore + 3,
        "every status getter takes the state lock");

  const unsigned long cmdBefore = state_lock_acquisitions();
  web.setFanSpeed(45);
  check(state_lock_acquisitions() > cmdBefore,
        "command callbacks take the state lock");

  // The real thing: one thread plays the AsyncTCP task and hammers the
  // callbacks while this thread runs the control loop. Every shared field is
  // guarded in main.cpp, so this must simply finish - and the state it leaves
  // behind must still make sense. Without the lock this is the exact
  // interleaving that produced clipped values and refused starts.
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  runLoops(SAFETY_CLEAR_STREAK + 4);
  check(!safety_faulted(), "latch cleared before the concurrency stress");

  std::atomic<bool> writerDone(false);
  std::thread writer([&writerDone]() {
    int i = 0;
    while (!writerDone.load()) {
      web.setFanSpeed(30 + (i % 70));
      web.startCool(50, 300);
      (void)web.getCoolActive();
      (void)web.getCoolRemainingSeconds();
      (void)web.getBT();
      (void)web.getET();
      (void)web.getHeaterDuty();
      (void)web.getSafetyFault();
      (void)web.getFanFault();
      (void)web.getManualActive();
      if (i % 3 == 0) web.startManual(150, 600, false, 0, 0);
      if (i % 3 == 1) web.stopManual();
      if (i % 5 == 0) web.startRoast(String("stress"));
      if (i % 5 == 2) web.stopRoast();
      web.stopCool();
      i++;
    }
  });

  runLoopsWithMovingProbes(80);   // 20 s of control time against the writer
  writerDone.store(true);
  writer.join();

  check(true, "the async-task stand-in and loop() finish without deadlocking");
  check(web.getFanSpeed() >= 0 && web.getFanSpeed() <= 100,
        "fan speed is still a valid percentage after the concurrent run");
  check(web.getHeaterDuty() >= 0 && web.getHeaterDuty() <= 100,
        "heater duty is still within 0..100 after the concurrent run");
  check(web.getManualRemainingSeconds() <= 600,
        "manual remaining time is consistent after the concurrent run");
  check(web.getCoolRemainingSeconds() <= 300,
        "cool remaining time is consistent after the concurrent run");

  // And the system still answers commands once the other task has stopped.
  web.stopManual();
  web.stopRoast();
  web.stopCool();
  web.setFanSpeed(100);
  runLoops(4);
  check(web.getFanSpeed() == 100, "commands still take effect after the stress");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
