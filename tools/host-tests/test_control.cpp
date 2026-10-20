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
//
// It runs in BOTH build modes (see run.sh): with SAFETY_ET_ENABLED 0 (a
// BT-only build) the bench case above still has to trip - on BT's own checks,
// since the ET backstop is gone - and a garbage ET reading must reach nothing.
// The assertions that need an ET channel are guarded by the flag.
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <max6675.h>
#include <PubSubClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

#include "config.h"
#include "safety.h"
#include "ror.h"
#include "heater_control.h"
#include "web_server.h"
#include "ota_push.h"
#include "mqtt_client.h"
#include "roast_profile.h"
#include "state_lock.h"
#include "ota_push.h"

void setup();
void loop();

// ---- push-OTA helpers (same route-search the OTA test uses) -----------------
static const AsyncWebServer::Route *findRoute(const char *uri,
                                              WebRequestMethodComposite method) {
  for (AsyncWebServer *server : asyncWebServers())
    for (const AsyncWebServer::Route &r : server->routes())
      if (r.uri == uri && r.method == method) return &r;
  return nullptr;
}

static std::string sha256hex(const uint8_t *data, size_t len) {
  mbedtls_sha256_context ctx;
  uint8_t digest[32];
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, data, len);
  mbedtls_sha256_finish(&ctx, digest);
  char out[65];
  for (size_t i = 0; i < sizeof(digest); i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
  return std::string(out, 64);
}

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
ArduinoOTAClass ArduinoOTA;
EspClass ESP;
UpdateClass Update;
void EspClass::restart() { restartCount++; }

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
// web_server.cpp is linked for real (main.cpp registers it), so its route table
// lives in the real server; what this test needs is only the captured callback
// structs, which the init functions do not expose. main.cpp therefore keeps
// them where the test can reach them.
static WebServerCallbacks web;
static bool webCaptured = false;
extern OtaPushCallbacks g_otaCallbacks;

// main.cpp keeps its callback structs where the tests can reach them: it
// exposes the one it just registered, and the test asserts that it is the
// struct it asked for. A test-only fallback keeps a build without those hooks
// (an older review copy of main.cpp) from failing this check.
void test_web_server_cb_captured(WebServerCallbacks callbacks) __attribute__((weak));
void test_web_server_cb_captured(WebServerCallbacks callbacks) {
  web = callbacks;
  webCaptured = true;
}

// ---- canned RoastProfile (the real one reads LittleFS) ---------------------
// The step the canned profile loads is configurable because totalSeconds() now
// ends a run: a test that wants the end of the profile inside its own wall
// clock shrinks it, and every other section leaves it at 10 minutes so nothing
// reaches the end by accident. Same for the profile's cool-down, which is 0
// (= none, today's behaviour) unless a section asks for one.
static float g_profileFan = 80.0f;
static unsigned long g_profileRampSeconds = 0;
static unsigned long g_profileHoldSeconds = 600;
static float g_profileCoolFan = 0.0f;
static float g_profileCoolMinutes = 0.0f;
// The charge the canned profile states, in grams. 0 (the default) = no duty
// ceiling, so no other section is affected by the ceiling test.
static int g_profileBeanWeight = 0;

bool RoastProfile::loadFromFile(const String &) {
  _hasFan = true;
  _startTemp = 20;
  _beanWeight = g_profileBeanWeight;
  _steps = { ProfileStep{ g_profileRampSeconds, g_profileHoldSeconds, 200, g_profileFan,
                          0.0f, 0.0f, 0.0f } };
  setCool(g_profileCoolFan, g_profileCoolMinutes);
  return true;
}
bool RoastProfile::saveToFile(const String &) const { return true; }
void RoastProfile::addStep(unsigned long rampSeconds, unsigned long holdSeconds,
                           float temp, float fan) {
  _steps.push_back(ProfileStep{ rampSeconds, holdSeconds, temp, fan, 0.0f, 0.0f, 0.0f });
  _hasFan = true;
}
void RoastProfile::addStep(unsigned long rampSeconds, unsigned long holdSeconds,
                           float temp, float fan,
                           float rorTarget, float rorStart, float rorEnd) {
  _steps.push_back(ProfileStep{ rampSeconds, holdSeconds, temp, fan, rorTarget, rorStart, rorEnd });
  _hasFan = true;
}
void RoastProfile::clear() {
  _steps.clear();
  _hasFan = false;
  _coolFanPct = 0;
  _coolMinutes = 0;
  _beanWeight = 0;
}
// The canned profile is the test's own knob: it takes the pair as given, and
// the 0-means-off rule is the one the real setCool() documents. The clamping of
// out-of-range numbers is exercised against the REAL implementation in
// test_web_server, where a profile really is written and read back.
void RoastProfile::setCool(float fanPct, float minutes) {
  if (!(fanPct > 0.0f) || !(minutes > 0.0f)) {
    _coolFanPct = 0;
    _coolMinutes = 0;
    return;
  }
  _coolFanPct = fanPct;
  _coolMinutes = minutes;
}
// The canned profile takes the stated weight as given (0 = none). The clamping
// of out-of-range numbers is exercised against the REAL implementation in
// test_web_server, where a profile really is written and read back.
void RoastProfile::setBeanWeight(int grams) {
  _beanWeight = grams > 0 ? grams : 0;
}
unsigned long RoastProfile::totalSeconds() const {
  unsigned long total = 0;
  for (const auto &s : _steps) total += s.rampSeconds + s.holdSeconds;
  return total;
}
float RoastProfile::targetAt(unsigned long) const { return 200; }
float RoastProfile::fanAt(unsigned long) const { return g_profileFan; }
float RoastProfile::rorTargetAt(unsigned long) const { return 0.0f; }
int RoastProfile::stepIndexAt(unsigned long) const { return 0; }

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

#if !SAFETY_ET_ENABLED
  // BT-only build, end to end through the real loop(). The ET converter is not
  // read at all - counted in the stub, because no value can show it: a dead and
  // a switched-off ET both report null. And a reading that would trip a full
  // build reaches nothing: 0 C on an ET data line is exactly what the firmware
  // saw on the bench with a floating module.
  g_probeBT = 20.0f;
  g_probeET = 0.0f;
  const int etReadsBefore = g_max6675EtReads;
  runLoops(60);
  check(g_max6675EtReads == etReadsBefore,
        "the control loop never reads the ET converter in a BT-only build");
  check(!safety_faulted(), "a 0 C ET cannot trip the latch in a BT-only build");
  check(std::isnan(web.getET()), "the ET channel reports no reading with the flag off");

  g_probeET = 999.0f;
  runLoops(60);
  check(!safety_faulted(), "nor can an absurd ET reading");
  g_probeET = 20.0f;
#endif

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
  // Which channels the safety layer is watching, in the same payload. A
  // reduced-protection image (SAFETY_ET_ENABLED 0) has to say so: the ET entity
  // reads as null in both cases - a dead module and a channel that is not read -
  // so the null alone cannot say "not watched" instead of "no reading".
#if SAFETY_ET_ENABLED
  check(statusHas("\"etDisabled\":false"),
        "mqtt status reports the ET channel as enabled");
#else
  check(statusHas("\"etDisabled\":true"),
        "mqtt status reports the ET channel as disabled");
#endif

  // Fan up to 100 %: NOT airflow yet. The duty ramps at FAN_SLEW_PCT_PER_SEC
  // (config.h), and the interlock judges on the reached speed. Judging on the
  // request instead is how the element gets lit while the fan is still spinning
  // up - which is the whole reason the ramp had to move the interlock with it.
  web.setFanSpeed(100);
  runLoops(1);   // one sample into the ramp: ~6 % reached, under the 10 % minimum
  check(web.getFanSpeed() == 100, "the request reports 100 % the moment it is made");
  check(web.getFanActual() < FAN_MIN_FOR_HEATER_PCT,
        "the fan has not actually reached the minimum yet");
  check(web.getFanFault(), "heat is refused while the fan is still ramping up");
  check(web.getHeaterDuty() == 0 && ssrState == LOW,
        "and nothing reaches the element meanwhile");

  runLoops(4);   // a second into the ramp: well past the minimum
  check(!web.getFanFault(), "interlock clears once the fan reaches the minimum");
  check(web.getHeaterDuty() > 0, "heat is allowed with airflow");
  check(ssrState == HIGH, "element conducts with airflow");
  check(web.getPidP() > 0.0f && web.getPidD() == 0.0f,
        "the PID reports a live P term and a silent D term while heating");
  check(web.getPidI() < 100.0f && web.getPidIntegral() < 1000.0f,
        "the I term and its state are reported inside the clamp");

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

  // --------------------------- bean weight caps the duty (0.14.0) -----------
  // The element is the same 1.1 kW whatever is in the drum, so a 20 g charge
  // gets ~5x the heat per gram that 100 g does. The profile's beanWeight caps
  // the control loop's request at 28 % there, rising to 100 % at 100 g. Nothing
  // else may change: the interlock still wins, manual mode has no ceiling, and
  // a profile without the field behaves as before.
  g_profileBeanWeight = 20;
  check(web.startRoast(String("ceiling-20")), "a 20 g profile starts");
  web.setFanSpeed(100);
  runLoops(6);
  check(web.getRoastActive(), "the 20 g profile is running");
  check(web.getDutyCeiling() >= 25.0f && web.getDutyCeiling() <= 30.0f,
        "status reports a 25-30 % duty ceiling for 20 g");
  check(web.getHeaterDuty() <= web.getDutyCeiling() + 0.001f,
        "the duty request never passes the ceiling");
  check(web.getHeaterDuty() == web.getDutyCeiling(),
        "under the ceiling the loop asks for exactly the ceiling, not less");
  check(!web.getFanFault(), "and the ceiling is not the interlock: air is fine");

  // The ceiling is not a safety limit: the fan interlock still takes the
  // element to 0 %, and it does so regardless of what the ceiling says. The
  // profile drives the fan itself, so the low speed comes from the profile.
  g_profileFan = 5.0f;
  runLoops(4);
  check(web.getFanFault() && web.getHeaterDuty() == 0 && ssrState == LOW,
        "the fan interlock still cuts the element under a ceiling");
  g_profileFan = 80.0f;
  runLoops(4);
  check(web.getHeaterDuty() == web.getDutyCeiling(),
        "and the ceiling is back in charge once the air returns");

  web.stopRoast();
  runLoops(2);
  check(web.getDutyCeiling() == 0.0f, "no profile running reports no ceiling");

  // 100 g and up: the ceiling is 100 %, i.e. today's behaviour.
  g_profileBeanWeight = 100;
  check(web.startRoast(String("ceiling-100")), "a 100 g profile starts");
  runLoops(6);
  check(web.getDutyCeiling() == HEATER_MAX_DUTY_PCT,
        "100 g reports 100 %, so nothing is capped");
  check(web.getHeaterDuty() > 30.0f,
        "and the duty really is allowed past the 20 g ceiling");
  web.stopRoast();
  runLoops(2);

  // A profile without the field (0) keeps the pre-0.14.0 behaviour exactly.
  g_profileBeanWeight = 0;
  check(web.startRoast(String("no-weight")), "a profile without a weight starts");
  runLoops(6);
  check(web.getDutyCeiling() == HEATER_MAX_DUTY_PCT,
        "no stated weight means no ceiling");
  check(web.getHeaterDuty() > 30.0f, "and the duty is unlimited there");
  web.stopRoast();
  runLoops(2);

  // Manual mode has no bean weight and therefore no ceiling: the ceiling must
  // never leak out of a profile run into a forced mode. Stop left the fan off,
  // so bring it back the way an operator would - the interlock refuses heat
  // without air, and that is older than this feature.
  web.setFanSpeed(100);
  runLoops(4);
  check(web.startManual(200, 600, false, 0, 0), "a manual run starts after a profile");
  runLoops(4);
  check(web.getDutyCeiling() == 0.0f, "manual mode reports no ceiling");
  check(web.getHeaterDuty() > 30.0f,
        "and manual mode is not capped by the previous profile's weight");
  web.stopManual();
  runLoops(2);
  g_profileBeanWeight = 0;

  // ------------------------------- stop and cool: one command, one lock -----
  // The point of the combined command is that the fan never passes through
  // 0 % between "stop" and "start cooling". With two requests the control loop
  // can sample in between, and the beans sit in a 200 C drum with no air while
  // the operator waits for the second one to land. Two things are asserted
  // here: the fan is re-commanded before the call returns, and the whole
  // transition is ONE state-lock acquisition - the only way a single-threaded
  // test can see "same critical section".
  check(web.startRoast(String("cool")), "roast starts for the stop-and-cool test");
  runLoops(4);
  check(web.getHeaterDuty() > 0 && ssrState == HIGH, "the roast heats before the stop");

  const unsigned long lockBeforeStopCool = state_lock_acquisitions();
  const bool stopCoolOk = web.stopRoastAndCool(60, 120);
  check(stopCoolOk, "stop-and-cool accepts 60 % for 120 s");
  check(state_lock_acquisitions() == lockBeforeStopCool + 1,
        "the whole stop-and-cool transition runs under ONE state lock");
  check(!web.getRoastActive(), "the roast is no longer active");
  check(web.getCoolActive(), "cooling is active in the same instant");
  check(web.getCoolSpeed() == 60, "cooling runs at the requested fan speed");
  check(web.getCoolRemainingSeconds() == 120, "cooling runs for the requested time");
  check(web.getFanSpeed() == 60,
        "the fan was re-commanded to the cool speed before the call returned");
  check(web.getElapsedSeconds() == 0, "the roast clock was cleared");

  bool fanDropped = false;
  for (int i = 0; i < 4; i++) {
    runLoops(1);
    if (web.getFanSpeed() != 60) fanDropped = true;
  }
  check(!fanDropped, "the fan stays at the cool speed on every sample after it");
  check(web.getCoolActive(), "the cool-down is still running a second later");
  check(web.getHeaterDuty() == 0 && ssrState == LOW, "nothing heats during the cool-down");

  // ----------------------------- Stop ends a running cool-down -------------
  // Measured on the machine 2026-10-20: after "Avbryt och kyl", pressing Stop
  // left the fan running (`coolActive` stayed true) until /api/cool/stop was
  // called by hand. From the roast tab a cool-down is still "the run", so Stop
  // has to end it - and end the fan with it, or the operator is left with air
  // and no visible control (the cooling panel was on the manual tab).
  web.stopRoast();
  check(!web.getCoolActive(), "Stop ends a cool-down that is still running");
  check(web.getFanSpeed() == 0, "and stops the fan the cool-down was running");
  check(web.getFanActual() == 0.0f, "the reached fan speed is 0 in the same call");
  check(web.getCoolRemainingSeconds() == 0, "with no countdown left behind");
  runLoops(2);
  check(web.getFanSpeed() == 0 && web.getFanActual() == 0.0f,
        "and no later control cycle puts the fan back on");

  // The same through the real route the Stop button hits.
  {
    const AsyncWebServer::Route *stopRoute = findRoute("/api/roast/stop", HTTP_POST);
    check(stopRoute != nullptr, "the roast-stop route is registered in the control test");
    if (stopRoute) {
      check(web.startRoast(String("cool")), "a roast starts for the Stop-route test");
      runLoops(4);
      check(web.stopRoastAndCool(75, 120), "with a cool-down started from it");
      check(web.getCoolActive(), "the cool-down runs before the route is called");
      AsyncWebServerRequest req;
      stopRoute->onRequest(&req);
      check(req.responseCode() == 200, "POST /api/roast/stop answers 200");
      check(!web.getCoolActive() && web.getFanSpeed() == 0,
            "and that route ends the cool-down and the fan");
    }
  }

  // The invalid cases are refused whole and change nothing: 0 % is "off", not a
  // cool speed, and 0 minutes is "no time". Falling back to a plain stop would
  // look like the roaster ignored the button while the beans kept cooking.
  web.stopCool();
  check(web.startRoast(String("cool")), "a roast starts for the refused stop-and-cool");
  runLoops(4);
  const unsigned long lockBeforeRefused = state_lock_acquisitions();
  check(!web.stopRoastAndCool(0, 120), "stop-and-cool refuses a 0 % fan");
  check(!web.stopRoastAndCool(60, 0), "stop-and-cool refuses a 0 s cool-down");
  check(state_lock_acquisitions() == lockBeforeRefused + 2,
        "each refused call takes the lock and does nothing else");
  check(web.getRoastActive(), "a refused stop-and-cool leaves the roast running");
  check(!web.getCoolActive(), "and starts no cooling");

  // Plain Stop is untouched: it still starts no cooling, so the two buttons can
  // never be the same button.
  web.stopRoast();
  check(!web.getRoastActive(), "plain Stop still stops the roast");
  check(!web.getCoolActive(), "plain Stop still starts no cooling");
  runLoops(2);

  // The same transition through the REAL route the button hits, so the HTTP
  // handler and the control layer are proven to be wired to each other (the
  // route's contract on its own is covered in test_web_server).
  {
    const AsyncWebServer::Route *stopCool =
        findRoute("/api/roast/stop-and-cool", HTTP_POST);
    check(stopCool != nullptr, "the stop-and-cool route is registered in the control test");
    if (stopCool) {
      check(web.startRoast(String("cool")), "a roast starts for the route test");
      runLoops(4);
      const char *json = "{\"coolSpeed\":75,\"coolMinutes\":3}";
      std::vector<uint8_t> body(json, json + strlen(json));
      const unsigned long lockBeforeRoute = state_lock_acquisitions();
      AsyncWebServerRequest req;
      stopCool->onBody(&req, body.data(), body.size(), 0, body.size());
      // Read the counter here, before any getter: every getter is a callback
      // and takes the lock itself, so a count taken after them would measure
      // the reads as well as the transition.
      const unsigned long locksTaken = state_lock_acquisitions() - lockBeforeRoute;
      check(req.responseCode() == 200, "the route answers 200 for that roast");
      check(!web.getRoastActive() && web.getCoolActive(),
            "and the roast is stopped with the cooling running");
      check(web.getCoolSpeed() == 75 && web.getCoolRemainingSeconds() == 180,
            "with the values from the request");
      check(locksTaken == 1,
            "the route's transition is still a single lock acquisition");
      web.stopCool();
      runLoops(2);
    }
  }

  // ------------------------- the profile's own cool-down (end of the run) ---
  // A profile carries coolFan/coolMinutes. When its last step has run out the
  // run ends by itself and the same fan-only cool timer starts - no request,
  // no operator. The canned profile is 5 s long here so the end arrives inside
  // the test's own clock.
  g_profileRampSeconds = 0;
  g_profileHoldSeconds = 5;
  g_profileCoolFan = 100.0f;
  g_profileCoolMinutes = 1.0f;
  check(web.startRoast(String("autocool")), "profile with a cool-down starts");
  // Eight samples = 2 s: long enough for the fan to spin back up past the
  // heater interlock (the cool-down before this section stopped it, and the
  // duty slews at FAN_SLEW_PCT_PER_SEC), still short of the 5 s profile.
  runLoops(8);
  check(web.getRoastActive(), "the profile runs while its last step has time left");
  check(web.getHeaterDuty() > 0, "and it heats");
  runLoops(13);   // 13 more samples take the clock past 5 s: the last step
                  // runs out inside this, and the cool-down has just started
  check(!web.getRoastActive(), "the profile ends by itself when the last step runs out");
  check(web.getCoolActive(), "its cool-down starts with no extra request");
  check(web.getCoolSpeed() == 100, "the cool-down uses the profile's fan value");
  check(web.getCoolRemainingSeconds() >= 59 && web.getCoolRemainingSeconds() <= 60,
        "the cool-down uses the profile's minutes");
  check(web.getHeaterDuty() == 0 && ssrState == LOW, "the element is off in the cool-down");
  check(web.getFanSpeed() == 100, "the fan keeps running at the cool speed");

  // 0 (or absent) in both fields = today's behaviour: the run ends, the element
  // goes off and the fan stops. No cool-down is invented for a profile that
  // never asked for one.
  //
  // The first cool-down is still running at this point (60 s of it), and a
  // cool-down owns the fan until it ends - so it is stopped here, exactly as an
  // operator would before starting the next profile.
  web.stopCool();
  runLoops(2);
  g_profileCoolFan = 0.0f;
  g_profileCoolMinutes = 0.0f;
  check(web.startRoast(String("nocool")), "profile without a cool-down starts");
  runLoops(22);
  check(!web.getRoastActive(), "that profile ends as well");
  check(!web.getCoolActive(), "no cooling is started when both fields are 0");
  check(web.getFanSpeed() == 0, "and the fan is stopped");
  check(web.getHeaterDuty() == 0 && ssrState == LOW, "the element is off");

  // A half-filled pair is not a cool-down either: a speed without a time is a
  // form someone left half-done, not an instruction.
  g_profileCoolFan = 100.0f;
  g_profileCoolMinutes = 0.0f;
  check(web.startRoast(String("halfcool")), "half-filled cool fields: the profile starts");
  runLoops(22);
  check(!web.getCoolActive() && web.getFanSpeed() == 0,
        "a fan speed without minutes starts no cooling");

  // A manual stop in the middle is NOT the end of a profile: the operator said
  // stop, and cooling is what "Avbryt och kyl" is for. Nothing is started
  // behind his back - not then, and not on a later sample either.
  g_profileCoolFan = 100.0f;
  g_profileCoolMinutes = 5.0f;
  g_profileHoldSeconds = 600;
  check(web.startRoast(String("manualstop")), "profile with a cool-down starts again");
  runLoops(4);
  web.stopRoast();
  check(!web.getRoastActive() && !web.getCoolActive(),
        "a manual Stop mid-profile starts no profile cooling");
  runLoops(4);
  check(!web.getCoolActive(), "and none appears on a later sample either");

  // ------------------------------------ pause is untouched (regression) -----
  // The pause freezes the roast clock and keeps the PID on the setpoint that
  // was active at the pause instant. That is unchanged by this work, and it is
  // also why a paused profile must not "end" past its last step: the clock is
  // stopped, so the end is not reached until it runs again.
  g_profileHoldSeconds = 6;
  check(web.startRoast(String("pause")), "pause-test profile starts");
  runLoops(8);   // 2 s in
  check(web.getRoastActive() && web.getHeaterDuty() > 0, "the profile heats before the pause");
  web.pauseRoast();
  const unsigned long pausedElapsed = web.getElapsedSeconds();
  runLoops(20);  // 5 s of wall clock - past the profile's 6 s
  check(web.getRoastPaused(), "the pause holds");
  check(web.getElapsedSeconds() == pausedElapsed,
        "the roast clock does not advance while paused");
  check(web.getRoastActive(), "a paused profile does not end, even past its last step");
  check(!web.getCoolActive(), "and no cool-down starts while it is paused");
  check(web.getHeaterDuty() > 0, "the heat keeps regulating the held step while paused");
  web.resumeRoast();
  runLoops(20);  // the clock runs again: the end is passed for real now
  check(!web.getRoastActive(), "the profile ends once the clock passes the end");
  check(web.getCoolActive(), "and its cool-down starts then");
  web.stopCool();
  runLoops(2);

  // Back to the settings the rest of the file assumes: the 10-minute step, no
  // profile cool-down, and the fan where the profile run above left it (the
  // manual sections below rely on there being airflow).
  g_profileRampSeconds = 0;
  g_profileHoldSeconds = 600;
  g_profileCoolFan = 0.0f;
  g_profileCoolMinutes = 0.0f;
  web.setFanSpeed(80);
  runLoops(2);

  // ------------------------------- the filter is in the control path only ----
  // The PID reads a low-passed bean temperature (include/filter.h), and the
  // safety layer deliberately does not: safety_update() and the
  // jump/plausibility checks behind it keep seeing the raw sample from
  // sensors_read(). Both halves are visible from here: a step BOTH probes make
  // together (19 C - accepted by the plausibility check, and not a BT/ET
  // disagreement) shows the raw path following it in the same sample while the
  // control input lags; and a jump large enough to be a fault still trips.
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  runLoops(SAFETY_CLEAR_STREAK + 4);
  check(!safety_faulted(), "clean state before the injected probe step");
  check(fabs(web.getFilteredBT() - web.getBT()) < 1.0f,
        "the filtered control input sits on the raw reading while steady");

  g_probeBT = 39.0f;   // +19 C in one sample: under SENSOR_FAULT_MAX_JUMP_C
  g_probeET = 39.0f;   // both channels, so the cold cross-check stays quiet
  runLoops(1);
  check(fabs(web.getBT() - 39.0f) < 0.01f,
        "the raw reading - what safety and the API see - follows the step at once");
  check(web.getFilteredBT() < web.getBT(),
        "while the PID's input is still catching up, so the filter is real");

  // A dropped probe: 21 C in one sample, and this time it is a fault. If the
  // filter sat in front of the jump check instead of behind sensors_read(),
  // this would arrive as a plausible ramp and nothing would trip.
  g_probeBT = 60.0f;
  runLoops(SENSOR_FAULT_MAX_COUNT + 2);
  check(safety_faulted(), "a jump a filter would smooth still trips the latch");
  check(safety_code() == SAFETY_SENSOR_BT, "and it trips as a BT sensor fault");

  // Clear it again for the rest of the test.
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  runLoops(SAFETY_CLEAR_STREAK + 4);
  check(!safety_faulted(), "the latch clears after the fault is taken away");

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
  check(web.getPidP() == 0.0f && web.getPidI() == 0.0f && web.getPidD() == 0.0f,
        "the PID terms read 0 again once no run is active");
  check(!web.startManual(200, 600, false, 0, 0), "restart stays denied while latched");
  runLoops(8);
  check(statusHas("\"safetyFault\":true"), "mqtt status carried the safety fault");

  // ----------------------------------------------- concurrency (state lock) --
  // The lock has to be taken by the web callbacks, and from a single-threaded
  // test the acquisition counter is the only way to see that.
  const unsigned long lockBefore = state_lock_acquisitions();
  (void)web.getBT();
  (void)web.getRorBt();
  (void)web.getFanSpeed();
  (void)web.getSafetyFault();
  (void)web.getFanActual();
  (void)web.getPidP();
  (void)web.getFilteredBT();
  check(state_lock_acquisitions() == lockBefore + 7,
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
      (void)web.getRorBt();
      (void)web.getRorEt();
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

  // ------------------------------------------------------- rate of rise -----
  // Through the real loop(), not against the module directly: both probes on
  // a straight 0.25 C per 250 ms sample, i.e. exactly 60 C/min, kept up across
  // two windows. The history is reset first - the minutes of flat readings
  // above would otherwise sit inside the fit and drag the slope down.
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  runLoops(SAFETY_CLEAR_STREAK + 4);
  check(!safety_faulted(), "clean state before the rate-of-rise ramp");
  ror_reset();
  const int rorSamples = 4 * 60 + 8;   // 62 s = two 30 s windows, ring wraps
  for (int i = 1; i <= rorSamples; i++) {
    g_probeBT = 20.0f + i * 0.25f;
    g_probeET = 20.0f + i * 0.25f;
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  check(web.getRorBt() > 59.5f && web.getRorBt() < 60.5f,
        "rate of rise follows a 60 C/min ramp through loop()");
#if SAFETY_ET_ENABLED
  check(web.getRorEt() > 59.5f && web.getRorEt() < 60.5f,
        "rate of rise reports the environment channel too");
#else
  // BT-only: no ET is read, so there is no history to fit and the ET rate stays
  // at its last value (0, from ror_reset()) instead of inventing a number from
  // a channel that is switched off. Nothing steers on it either: the RoR
  // guidance follows the channel the build measures (ROR_GUIDANCE_ON_BT), so in
  // this mode it steers on rorBt - covered in test_ror_guidance, documented in
  // include/config.h and in docs/firmware-notes.md.
  check(web.getRorEt() == 0.0f,
        "the ET rate stays empty when the ET channel is off");
#endif
  check(statusHas("\"rorBt\":"), "mqtt status carries rorBt");

  // ------------------------------------------- push OTA mutual exclusion ----
  // Do this before the ArduinoOTA tests so the heater emergency latch and the
  // Update state are still clean.
  {
    // A push OTA transfer owns Update while it runs; serviceOta() must not call
    // ArduinoOTA.handle() then, and the transfer must latch the element off.
    const AsyncWebServer::Route *update = findRoute("/api/update", HTTP_POST);
    check(update != nullptr, "POST /api/update is registered in the control test");

    std::vector<uint8_t> image(64 * 1024, 0x42);
    image[0] = 0xE9;
    const std::string hash = sha256hex(image.data(), image.size());

    // A push is refused while a run is active, and must not disturb that run.
    check(web.startManual(200, 600, false, 100, 0), "manual run starts for the push-exclusion test");
    runLoops(4);
    check(ssrState == HIGH, "element is conducting before the refused push");

    AsyncWebServerRequest refused;
    refused.client()->setRemoteIP(String(OTA_ALLOWED_CLIENT_IP));
    refused.addHeader("X-OTA-Token", String("host-test-push-token"));
    refused.addHeader("X-OTA-SHA256", String(hash.c_str()));
    update->onBody(&refused, image.data(), image.size(), 0, image.size());
    check(refused.responseCode() == 409, "push is refused while a run is active");
    check(ssrState == HIGH, "the refused push did not turn the element off");
    web.stopManual();
    runLoops(2);

    // Now the push is accepted; the first chunk starts the transfer and keeps
    // it in progress.
    AsyncWebServerRequest push;
    push.client()->setRemoteIP(String(OTA_ALLOWED_CLIENT_IP));
    push.addHeader("X-OTA-Token", String("host-test-push-token"));
    push.addHeader("X-OTA-SHA256", String(hash.c_str()));
    update->onBody(&push, image.data(), 4096, 0, image.size());
    check(!push.sent(), "the first chunk does not answer yet");
    check(ota_push_in_progress(), "a push transfer is in progress");
    check(ssrState == LOW, "push start latches the element off");
    check(!web.getManualActive(), "push start aborts any session");

    const int handlesDuringPush = ArduinoOTA.handleCount;
    runLoops(4);
    check(ArduinoOTA.handleCount == handlesDuringPush,
          "ArduinoOTA.handle() is not called while a push transfer owns Update");

    // Abort the push so the rest of the test sees a clean Update state.
    ota_push_finished(false);
    Update.reset();
    // The push latched the heater off for safety; clear that latch so the
    // ArduinoOTA test below can observe the element conducting before transfer.
    heater_clear_emergency();
    runLoops(2);
    check(!heater_emergency_active(), "heater emergency latch cleared after push abort");
  }

  // ------------------------------------------------------------------ OTA ---
  // The update path has to be up as soon as the link is, must refuse to run
  // while the roaster is doing its job, and must cut the element before a
  // single byte of a transfer lands - nothing runs during the transfer to
  // service the heater window, so a pin left high would heat the whole time.
  check(ArduinoOTA.beginCount == 1,
        "OTA starts exactly once when the link comes up");
  check(ArduinoOTA.hostname == OTA_HOSTNAME, "OTA advertises the configured hostname");
  check(ArduinoOTA.port == OTA_PORT, "OTA listens on the configured port");
  check(ArduinoOTA.password == OTA_PASSWORD, "OTA is armed with the password from secrets.h");

  const int idleHandles = ArduinoOTA.handleCount;
  runLoops(4);
  check(ArduinoOTA.handleCount > idleHandles, "OTA is served while the roaster is idle");

  check(web.startManual(200, 600, false, 100, 0), "manual run starts for the OTA check");
  runLoops(4);
  check(web.getHeaterDuty() > 0 && ssrState == HIGH,
        "element is conducting when a transfer would arrive");

  const int busyHandles = ArduinoOTA.handleCount;
  runLoops(6);
  check(ArduinoOTA.handleCount == busyHandles,
        "no transfer is served while a run is active");

  ArduinoOTA.fireStart();
  check(ssrState == LOW, "transfer start latches the element off before any byte lands");
  check(heater_emergency_active(), "transfer start holds the heater emergency latch");
  check(!web.getManualActive(), "transfer start aborts the running session");

  const int restartsBeforeError = ESP.restartCount;
  ArduinoOTA.fireError(OTA_RECEIVE_ERROR);
  loop();
  check(ESP.restartCount > restartsBeforeError,
        "a failed transfer restarts the device instead of leaving it latched");

  const int restartsBeforeEnd = ESP.restartCount;
  ArduinoOTA.fireEnd();
  loop();
  check(ESP.restartCount > restartsBeforeEnd,
        "a finished transfer restarts the device into the new image");

  // ------------------------------------------- filesystem OTA (U_SPIFFS) ----
  // This is the case that was broken: LittleFS stayed mounted while the
  // incoming image rewrote the very partition it lived on, and the transfer
  // died part-way through with "[ERROR]: Error Uploading" after ~190 chunks.
  // The rule is that the command decides: U_SPIFFS must unmount first, U_FLASH
  // must not.
  {
    // A fresh boot state for the filesystem.
    LittleFS.begin(true);
    check(LittleFS.mounted(), "the filesystem is mounted before any transfer");

    const size_t opsBefore = littlefs_stub::opCount();
    ArduinoOTA.setCommand(U_FLASH);
    ArduinoOTA.fireStart();
    check(LittleFS.mounted(),
          "a FIRMWARE transfer (U_FLASH) leaves LittleFS mounted - it writes the "
          "other partition, and the profiles are read for the rest of this boot");
    check(littlefs_stub::opLog().size() == opsBefore ||
              littlefs_stub::opLog().back().rfind("end", 0) != 0,
          "a firmware transfer does not unmount the filesystem");
  }
  {
    // U_SPIFFS: the image lands on the filesystem partition, so the filesystem
    // has to be out of the way before the first byte arrives.
    LittleFS.begin(true);
    const size_t opsBefore = littlefs_stub::opCount();
    ArduinoOTA.setCommand(U_SPIFFS);
    ArduinoOTA.fireStart();
    check(!LittleFS.mounted(),
          "a FILESYSTEM transfer (U_SPIFFS) unmounts LittleFS before the image lands");
    bool sawEnd = false;
    for (size_t i = opsBefore; i < littlefs_stub::opLog().size(); i++) {
      if (littlefs_stub::opLog()[i] == "end:/") sawEnd = true;
    }
    check(sawEnd, "the unmount is the real end() call, not just a flag");
    check(ssrState == LOW,
          "the element is still latched off on the filesystem path");
    check(heater_emergency_active(),
          "and the safety latch holds there too");
  }
  {
    // While unmounted, the profile endpoints have nothing readable to read:
    // they must refuse instead of reading a half-written partition.
    check(!web_fs_available(),
          "the API reports the filesystem as unavailable during a filesystem transfer");
    const AsyncWebServer::Route *list = nullptr;
    for (AsyncWebServer *s : asyncWebServers())
      for (const AsyncWebServer::Route &r : s->routes())
        if (r.uri == "/api/profiles" && r.method == HTTP_GET) list = &r;
    if (list) {
      AsyncWebServerRequest req;
      list->onRequest(&req);
      check(req.responseCode() == 503,
            "listing profiles during a filesystem transfer answers 503, not garbage");
      check(!LittleFS.mounted(), "and it still does not touch the unmounted filesystem");
    }
  }
  {
    // A failed filesystem transfer must remount before the restart: the device
    // comes back with a readable filesystem either way.
    const int restartsBefore = ESP.restartCount;
    ArduinoOTA.fireError(OTA_RECEIVE_ERROR);
    check(LittleFS.mounted(),
          "a failed filesystem transfer remounts LittleFS before restarting");
    loop();
    check(ESP.restartCount > restartsBefore,
          "and then restarts like any failed transfer");
    check(web_fs_available(), "the API reports the filesystem as available again");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
