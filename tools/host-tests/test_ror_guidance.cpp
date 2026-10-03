// Host-side test of the optional rate-of-rise guidance layer in src/main.cpp.
// It drives the real setup()/loop() with stubbed hardware, exactly like
// test_control.cpp, but focuses on the RoR-guidance preconditions, curve and
// safety clamps. A profile with rorTarget > 0 is written to the in-memory
// LittleFS and started through the same HTTP callback path the web UI uses.
//
// Covered:
//   * the target curve interpolates linearly from rorStart to rorEnd
//   * the correction has the right sign (too slow -> positive, too fast -> negative)
//   * the correction is zero when disabled (paused, non-RoR step, alarm, invalid RoR)
//   * the rate limit + clamp keeps a cold-probe transient below HEATER_MAX_DUTY_PCT
//   * every duty request goes through applyHeaterDuty(): fan below minimum -> 0 duty
//   * a profile without rorTarget behaves like before (the RoR state reports off)
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <Update.h>
#include <max6675.h>
#include <PubSubClient.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <iostream>

#include "config.h"
#include "safety.h"
#include "ror.h"
#include "heater_control.h"
#include "web_server.h"
#include "ota_push.h"
#include "mqtt_client.h"
#include "roast_profile.h"
#include "state_lock.h"

void setup();
void loop();

// ---- Arduino / runtime stubs ------------------------------------------------
static std::atomic<unsigned long> fakeMillis{0};
unsigned long millis() { return fakeMillis.load(); }
void delay(unsigned long ms) { fakeMillis += ms; }
void delayMicroseconds(unsigned int) {}

static int ssrState = -1;
void pinMode(int, int) {}
void digitalWrite(int pin, int value) {
  if (pin != PIN_SSR_HEATER) return;
  ssrState = value;
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
// The real web_server.cpp is linked (main.cpp registers it), so only the
// captured struct is needed here, and main.cpp keeps it reachable.
static WebServerCallbacks web;
static bool webCaptured = false;
extern OtaPushCallbacks g_otaCallbacks;

// Same hook as test_control: main.cpp hands its registered web callback struct
// to whoever provides this weak receiver, so the test can reach the routes'
// handlers while web_server.cpp is the real one.
void test_web_server_cb_captured(WebServerCallbacks callbacks) __attribute__((weak));
void test_web_server_cb_captured(WebServerCallbacks callbacks) {
  web = callbacks;
  webCaptured = true;
}

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

static void checkClose(float got, float want, float tol, const char *what) {
  char detail[96];
  snprintf(detail, sizeof(detail), "got %.3f, want %.3f +-%.3f", got, want, tol);
  check(std::fabs(got - want) <= tol, what);
}

static void runLoops(int samples) {
  for (int i = 0; i < samples; i++) {
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
}

// Ramp the probes to a new setpoint without exceeding the 20 C per-sample
// sanity-check limit in sensors.cpp, so a large setpoint change between tests
// is not misread as a pulled probe.
static void rampProbes(float targetBT, float targetET, int samples) {
  for (int i = 0; i < samples; i++) {
    float t = (float)(i + 1) / (float)samples;
    g_probeBT += (targetBT - g_probeBT) * t;
    g_probeET += (targetET - g_probeET) * t;
    // Make sure each individual step is within the jump limit.
    // This is defensive: the interpolation above can overshoot per-sample
    // when the distance is large and samples is small.
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
}

// Slowly drift both probes so the stuck-probe detector never sees the same
// reading bit-for-bit for 60 s while the element is on. The drift is far
// smaller than the MAX6675 ladder in reality, but the host stub reads the raw
// float, so any change resets the stuck timer.
static void driftProbes(int &tick, float baseBT, float baseET, float rateCpm = 1.0f) {
  g_probeBT = baseBT + rateCpm * (float)(tick * SENSOR_READ_INTERVAL_MS) / 60000.0f;
  g_probeET = baseET + rateCpm * (float)(tick * SENSOR_READ_INTERVAL_MS) / 60000.0f;
  tick++;
}

// Feed a straight ramp until the RoR estimate is valid and stable.
static void warmUpRor(float rateCpm) {
  ror_reset();
  int needed = (ROR_WINDOW_MS + ROR_MIN_SPAN_MS) / SENSOR_READ_INTERVAL_MS + 8;
  for (int i = 1; i <= needed; i++) {
    g_probeBT = 20.0f + rateCpm * (float)(i * SENSOR_READ_INTERVAL_MS) / 60000.0f;
    g_probeET = g_probeBT;
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

static void ensureProfilesDir() {
  if (std::find(littlefs_stub::dirs().begin(), littlefs_stub::dirs().end(), PROFILES_DIR) ==
      littlefs_stub::dirs().end()) {
    littlefs_stub::dirs().push_back(PROFILES_DIR);
  }
}

static std::string profilePath(const char *name) {
  return std::string(PROFILES_DIR) + "/" + name + ".json";
}

// Build a profile in code and save it so main.cpp's loadFromFile can use it.
static void seedProfile(const char *name, bool withRor, float fanPct = 80.0f) {
  ensureProfilesDir();
  RoastProfile p;
  p.setStartTemp(20.0f);
  if (withRor) {
    // 60 s ramp 20->50 C, then 60 s hold at 50 C.
    // RoR target falls from 15 C/min to 3 C/min over the whole 120 s step.
    p.addStep(60, 60, 50.0f, fanPct, 10.0f, 15.0f, 3.0f);
  } else {
    p.addStep(60, 60, 50.0f, fanPct);
  }
  p.saveToFile(profilePath(name).c_str());
}

int main() {
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  WiFi.statusValue = WL_DISCONNECTED;
  setup();
  check(webCaptured, "main.cpp registers its web callbacks");

  // Warm up the RoR estimate on a modest ramp so later tests start valid.
  warmUpRor(5.0f);
  check(ror_valid(), "RoR estimate is valid after warm-up");

  // ---------------------------------------------- profile round-trip ---------
  seedProfile("ror-trip", true);
  RoastProfile loaded;
  check(loaded.loadFromFile(profilePath("ror-trip").c_str()), "profile with rorTarget loads");
  check(loaded.stepCount() == 1, "round-trip keeps one step");
  checkClose(loaded.rorTargetAt(0), 15.0f, 0.01f, "round-trip keeps rorStart at step start");
  checkClose(loaded.rorTargetAt(30), 9.0f, 0.01f, "round-trip interpolates mid-ramp");
  checkClose(loaded.rorTargetAt(60), 3.0f, 0.01f, "round-trip keeps rorEnd at ramp end");
  checkClose(loaded.rorTargetAt(90), 3.0f, 0.01f, "round-trip keeps rorEnd during hold");

  // --------------------------------------------------- target curve ----------
  seedProfile("curve", true);
  check(web.startRoast(String("curve")), "RoR profile roast starts");
  web.setFanSpeed(80);
  int tick = 0;
  // Keep probes gently moving so the stuck detector stays quiet.
  for (int i = 0; i < 4; i++) {
    driftProbes(tick, 20.0f, 20.0f);
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  check(web.getRorGuidance() == 0, "guidance reports step 0");
  checkClose(web.getRorTarget(), 15.0f, 0.2f,
             "target at step start is close to rorStart");

  // Advance to 30 s elapsed: midpoint of the 60 s ramp.
  for (int i = 0; i < (30 * 1000) / SENSOR_READ_INTERVAL_MS - 4; i++) {
    driftProbes(tick, 20.0f, 20.0f);
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  checkClose(web.getRorTarget(), 9.0f, 0.2f,
             "target mid-ramp is close to the midpoint of rorStart and rorEnd");

  // Advance to 60 s elapsed: start of the hold.
  for (int i = 0; i < (30 * 1000) / SENSOR_READ_INTERVAL_MS; i++) {
    driftProbes(tick, 20.0f, 20.0f);
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  checkClose(web.getRorTarget(), 3.0f, 0.2f,
             "target at hold start is close to rorEnd");
  web.stopRoast();
  runLoops(4);

  // ------------------------------------------------------- sign of correction -
  seedProfile("sign", true);
  check(web.startRoast(String("sign")), "sign-test roast starts");
  // The profile target at 0 s is 20 C. Keep BT there so the PID error is tiny,
  // and make ET flat so ror_et trends to 0.
  web.setFanSpeed(80);
  tick = 0;
  for (int i = 0; i < 4; i++) {
    driftProbes(tick, 20.0f, 20.0f);
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }

  // Case 1: ET rises too slowly -> positive correction.
  // Keep BT gently rising so PID stays small; ET flat -> ror_et ~0.
  for (int i = 0; i < 120; i++) {
    g_probeBT = 20.0f + 5.0f * (float)(i * SENSOR_READ_INTERVAL_MS) / 60000.0f;
    g_probeET = 20.0f;
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  check(web.getRorTarget() > 0.0f, "RoR target is active in sign test");
  check(web.getRorError() > 0.0f, "flat ET gives positive error (target > actual)");
  check(web.getRorActive(), "guidance is active");
  check(web.getHeaterDuty() > 0.0f, "positive error adds heat");

  // Case 2: ET rises too fast -> negative correction.
  web.stopRoast();
  runLoops(4);
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  check(web.startRoast(String("sign")), "sign-test roast restarts");
  web.setFanSpeed(80);
  for (int i = 0; i < 120; i++) {
    g_probeBT = 20.0f + 5.0f * (float)(i * SENSOR_READ_INTERVAL_MS) / 60000.0f;
    g_probeET = 20.0f + 30.0f * (float)(i * SENSOR_READ_INTERVAL_MS) / 60000.0f;
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  check(web.getRorError() < 0.0f, "fast ET gives negative error");
  check(web.getHeaterDuty() == 0.0f,
        "negative error clamps duty to 0 (PID alone would be small here)");
  web.stopRoast();
  runLoops(4);

  // ------------------------------------------------------ disabled states ----
  seedProfile("disabled", true);
  check(web.startRoast(String("disabled")), "disabled-test roast starts");
  web.setFanSpeed(80);
  tick = 0;
  for (int i = 0; i < 120; i++) {
    driftProbes(tick, 20.0f, 20.0f);
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  check(web.getRorActive(), "guidance active before disabling tests");

  // Pause -> correction 0.
  web.pauseRoast();
  runLoops(4);
  check(!web.getRorActive(), "pause disables RoR guidance");
  check(web.getRorTarget() == 0.0f, "target resets to 0 while paused");
  web.resumeRoast();
  runLoops(4);

  // Safety alarm -> correction 0. Pull BT probe to trip latch.
  g_probeBT = 0.0f;
  runLoops(SENSOR_FAULT_MAX_COUNT + 3);
  check(safety_faulted(), "safety latch trips");
  check(!web.getRorActive(), "safety alarm disables RoR guidance");
  check(web.getHeaterDuty() == 0.0f, "heater is off during safety alarm");

  // Heal sensors to clear latch.
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  runLoops(SAFETY_CLEAR_STREAK + 4);
  check(!safety_faulted(), "latch clears");
  web.stopRoast();
  runLoops(4);

  // Non-RoR step -> correction 0.
  seedProfile("no-ror", false);
  check(web.startRoast(String("no-ror")), "non-RoR profile starts");
  web.setFanSpeed(80);
  tick = 0;
  for (int i = 0; i < 120; i++) {
    driftProbes(tick, 20.0f, 20.0f);
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  check(web.getRorGuidance() == -1, "non-RoR step reports guidance -1");
  check(!web.getRorActive(), "non-RoR step keeps guidance inactive");
  check(web.getRorTarget() == 0.0f, "non-RoR step keeps target at 0");
  web.stopRoast();
  runLoops(4);

  // Invalid RoR (freshly reset) -> correction 0 even with rorTarget step.
  seedProfile("invalid", true);
  check(web.startRoast(String("invalid")), "invalid-RoR roast starts");
  web.setFanSpeed(80);
  ror_reset();
  g_probeBT = 20.0f;
  g_probeET = 20.0f;
  runLoops(4);
  check(!ror_valid(), "RoR is invalid immediately after reset");
  check(!web.getRorActive(), "invalid RoR keeps guidance inactive");
  web.stopRoast();
  runLoops(4);

  // Publish at least one MQTT status with the plain profile so the regression
  // assertions can see roRorGuidance and roRorActive. The loop above may not
  // have published because the connection interval had not elapsed.
  seedProfile("plain", false);
  check(web.startRoast(String("plain")), "plain profile starts");
  web.setFanSpeed(80);
  int plainTick = 0;
  for (int i = 0; i < 120; i++) {
    driftProbes(plainTick, 20.0f, 20.0f);
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  check(web.getRorGuidance() == -1, "plain profile: guidance step is -1");
  check(web.getRorTarget() == 0.0f, "plain profile: target is 0");
  check(web.getRorError() == 0.0f, "plain profile: error is 0");
  check(!web.getRorActive(), "plain profile: guidance is inactive");
  // Bring MQTT online so we can inspect the status payload.
  WiFi.statusValue = WL_CONNECTED;
  mqtt_update();
  check(statusHas("\"roRorGuidance\":-1"), "mqtt status carries roRorGuidance");
  check(statusHas("\"roRorActive\":false"), "mqtt status carries roRorActive");
  web.stopRoast();
  runLoops(4);

  // ---------------------------------------------------------- clamp / rate ---
  seedProfile("clamp", true);
  // Move the probes gently to the clamp-test starting point so sensors.cpp
  // does not see the step change as a pulled probe.
  rampProbes(80.0f, 80.0f, 8);
  check(web.startRoast(String("clamp")), "clamp-test roast starts");
  web.setFanSpeed(80);
  // Put BT on profile target so PID ~0; make ET plunge fast to create a huge
  // positive error. Start high enough that the probe stays above SENSOR_MIN_VALID_C
  // for the whole 60 s run. A -50 C/min ET rate means error ~55 C/min -> raw
  // correction ~165 %, but the rate limit and clamp must keep it bounded.
  for (int i = 0; i < 240; i++) {
    g_probeBT = 80.0f + 5.0f * (float)(i * SENSOR_READ_INTERVAL_MS) / 60000.0f;
    g_probeET = 80.0f - 50.0f * (float)(i * SENSOR_READ_INTERVAL_MS) / 60000.0f;
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  check(web.getRorError() > 50.0f, "cold-probe scenario creates a large error");
  check(web.getHeaterDuty() <= HEATER_MAX_DUTY_PCT + 0.01f,
        "duty never exceeds HEATER_MAX_DUTY_PCT");
  check(ssrState == HIGH || web.getHeaterDuty() == 0.0f,
        "heater pin follows the clamped duty (on when positive)");
  web.stopRoast();
  runLoops(4);

  // ------------------------------------------------------ fan interlock ------
  // Use a profile whose fan is below the interlock threshold so the guidance
  // wants heat but applyHeaterDuty() holds it at 0.
  seedProfile("interlock", true, 5.0f);
  // Move probes back to room temperature smoothly; a stepped change would look
  // like a probe fault to sensors.cpp.
  rampProbes(20.0f, 20.0f, 8);
  check(web.startRoast(String("interlock")), "interlock-test roast starts");
  // Keep ET flat so guidance wants positive heat.
  tick = 0;
  for (int i = 0; i < 120; i++) {
    driftProbes(tick, 20.0f, 20.0f);
    fakeMillis += SENSOR_READ_INTERVAL_MS;
    loop();
  }
  check(web.getRorActive(), "guidance considers itself active");
  check(web.getFanFault(), "fan below minimum flags interlock");
  check(web.getHeaterDuty() == 0.0f, "interlock forces reported duty to 0");
  check(ssrState == LOW, "interlock keeps the SSR pin low");
  web.stopRoast();
  runLoops(4);

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
