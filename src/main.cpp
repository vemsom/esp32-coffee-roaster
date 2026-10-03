#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>

#include "config.h"
#include "pid.h"
#include "sensors.h"
#include "ror.h"
#include "safety.h"
#include "heater_control.h"
#include "fan_control.h"
#include "roast_profile.h"
#include "web_server.h"
#include "ota_push.h"
#include "mqtt_client.h"
#include "state_lock.h"

// One PID instance, shared between manual and profile runs. It is reset
// whenever a run starts so the integral does not carry over between runs.
static SimplePID heaterPID(PID_KP, PID_KI, PID_KD, 0, 100);

enum ControlMode { MODE_IDLE, MODE_MANUAL, MODE_PROFILE };
static ControlMode controlMode = MODE_IDLE;

// The push-OTA callbacks handed to ota_push_init() in setup(). Kept at file
// scope so the host test can reach the predicate the endpoint will ask: the
// test drives setup()/loop() and then has to answer "is a run active?" the
// same way the device would.
OtaPushCallbacks g_otaCallbacks = {};

// The web callback struct is file-static, but the host test needs it to reach
// the routes' handlers. It is handed over here, if the test provides the
// (weak) receiver - a normal build does not, and nothing happens then.
void test_web_server_cb_captured(WebServerCallbacks callbacks) __attribute__((weak));

static float currentBT = NAN;
static float currentET = NAN;
static float currentHeaterDuty = 0;
static int currentFanSpeed = 0;

// Rate-of-rise guidance state, updated each control cycle. Guidance is only
// active in profile mode when the current step has rorTarget > 0 and the RoR
// estimate has warmed up. These are reported in /api/status and MQTT.
static float currentRorTarget = 0.0f;
static float currentRorError = 0.0f;
static float currentRorCorrection = 0.0f;
static int currentRorGuidanceStep = -1;  // -1 = none
static bool currentRorActive = false;
// Last time the RoR correction was updated, used to rate-limit its movement.
static unsigned long lastRorCorrectionUpdate = 0;

// Fan interlock: true while a positive heat request is being withheld because
// the fan is below FAN_MIN_FOR_HEATER_PCT. Self-clearing and deliberately
// separate from the latched sensor alarm - it has its own message and must
// never be reported as a safety fault.
static bool fanInterlockFault = false;

// Every heater command goes through here. The interlock decides whether the
// requested duty may reach the SSR, and the flag is recomputed from scratch on
// each call so it always means "heat is being withheld right now" instead of
// sticking on after the run has ended.
static void applyHeaterDuty(float duty) {
  if (duty > 0.0f && currentFanSpeed < FAN_MIN_FOR_HEATER_PCT) {
    fanInterlockFault = true;
    currentHeaterDuty = 0;
    heater_set_duty(0);
    return;
  }
  fanInterlockFault = false;
  currentHeaterDuty = duty;
  heater_set_duty(duty);
}

// ---- Manual mode state (fixed target temp for a fixed duration) ----
static float manualTargetTemp = 0;
static unsigned long manualDurationSeconds = 0;
static unsigned long manualStartMillis = 0;
static bool manualAutoCool = false;
static int manualCoolSpeed = 0;
static unsigned long manualCoolSeconds = 0;

// ---- Profile mode state ----
static RoastProfile activeProfile;
static unsigned long roastStartMillis = 0;
static bool roastPaused = false;
static unsigned long roastPauseStarted = 0;
static unsigned long roastPausedTotal = 0;

// ---- Cool mode state (fan-only timer, independent of heater control) ----
static bool coolActive = false;
static int coolSpeed = 0;
static unsigned long coolDurationSeconds = 0;
static unsigned long coolStartMillis = 0;

static unsigned long lastSensorRead = 0;

// ---- Safety and profile selection ----
// Latched alarm state as seen on the previous sample: used so the log line and
// the heater re-arm happen once per trip/clear instead of every cycle.
static bool safetyLatched = false;

// Selected profile: set when a roast starts, reported by the web UI status
// and by the MQTT status payload.
// A fixed buffer rather than a String: cbGetProfileName() hands this pointer
// to the MQTT payload builder, which runs outside the lock, and a String could
// be re-allocated by a concurrent profile start while that pointer is in use.
// 64 bytes covers any LittleFS profile name; longer ones are truncated.
static char selectedProfileName[64] = "";

// ---- Status callbacks ----
// Each of these runs on the AsyncTCP task while loop() runs the control state
// machine on the Arduino task, so each one takes the state lock first
// (include/state_lock.h). The lock is recursive: a command callback may call
// another command callback while holding it.
static float cbGetBT() {
  StateLockGuard guard;
  return currentBT;
}

static float cbGetET() {
  StateLockGuard guard;
  return currentET;
}

// Rate of rise, computed in the control loop at the sensor sample rate. The
// lock matters here just as much as for the temperatures: ror_update() rewrites
// the history the AsyncTCP task reads through these getters.
static float cbGetRorBt() {
  StateLockGuard guard;
  return ror_get_bt();
}

static float cbGetRorEt() {
  StateLockGuard guard;
  return ror_get_et();
}

static int cbGetRorGuidance() {
  StateLockGuard guard;
  return currentRorGuidanceStep;
}

static float cbGetRorTarget() {
  StateLockGuard guard;
  return currentRorTarget;
}

static float cbGetRorError() {
  StateLockGuard guard;
  return currentRorError;
}

static bool cbGetRorActive() {
  StateLockGuard guard;
  return currentRorActive;
}

static float cbGetHeaterDuty() {
  StateLockGuard guard;
  return currentHeaterDuty;
}

static int cbGetFanSpeed() {
  StateLockGuard guard;
  return currentFanSpeed;
}

static bool cbGetRoastActive() {
  StateLockGuard guard;
  return controlMode == MODE_PROFILE;
}

// Roast time excludes any paused stretches, so pausing holds the current
// setpoint in place without advancing the profile.
static unsigned long roastElapsedSeconds() {
  if (controlMode != MODE_PROFILE) return 0;
  unsigned long now = millis();
  unsigned long paused = roastPausedTotal + (roastPaused ? now - roastPauseStarted : 0);
  return (now - roastStartMillis - paused) / 1000;
}

static unsigned long cbGetElapsedSeconds() {
  StateLockGuard guard;
  return roastElapsedSeconds();
}

static bool cbGetRoastPaused() {
  StateLockGuard guard;
  return roastPaused;
}

static bool cbGetManualActive() {
  StateLockGuard guard;
  return controlMode == MODE_MANUAL;
}

static float cbGetManualTargetTemp() {
  StateLockGuard guard;
  return manualTargetTemp;
}

static bool cbGetManualAutoCool() {
  StateLockGuard guard;
  return manualAutoCool;
}

static unsigned long cbGetManualRemainingSeconds() {
  StateLockGuard guard;
  if (controlMode != MODE_MANUAL) return 0;
  unsigned long elapsed = (millis() - manualStartMillis) / 1000;
  if (elapsed >= manualDurationSeconds) return 0;
  return manualDurationSeconds - elapsed;
}

// Whether cooling is running right now. Cooling is not a ControlMode: the
// callbacks expose it through the remaining seconds (the same value the UI
// shows), so "active" means coolActive with time left on it.
static bool cbGetCoolActive() {
  StateLockGuard guard;
  if (!coolActive) return false;
  unsigned long elapsed = (millis() - coolStartMillis) / 1000;
  return elapsed < coolDurationSeconds;
}

// One predicate for "the machine is doing something": every run mode counts,
// for the same reason serviceOta() refuses then - a flash aborts the run, and
// the element should never be conducting during one.
static bool cbIsRunActive() {
  StateLockGuard guard;
  return controlMode == MODE_PROFILE || controlMode == MODE_MANUAL ||
         cbGetCoolActive();
}

static int cbGetCoolSpeed() {
  StateLockGuard guard;
  return coolSpeed;
}

static unsigned long cbGetCoolRemainingSeconds() {
  StateLockGuard guard;
  if (!coolActive) return 0;
  unsigned long elapsed = (millis() - coolStartMillis) / 1000;
  if (elapsed >= coolDurationSeconds) return 0;
  return coolDurationSeconds - elapsed;
}

static bool cbGetSafetyFault() {
  StateLockGuard guard;
  return safety_faulted();
}

static const char *cbGetSafetyReason() {
  StateLockGuard guard;
  return safety_code_text();
}

static bool cbGetFanFault() {
  StateLockGuard guard;
  return fanInterlockFault;
}

// Not under the state lock: the link state belongs to the WiFi driver, not to
// anything loop() writes, and serviceWifi() runs on this same task.
static bool cbGetWifiConnected() { return WiFi.status() == WL_CONNECTED; }

static const char *cbGetModeName() {
  StateLockGuard guard;
  if (controlMode == MODE_PROFILE) return "profile";
  if (controlMode == MODE_MANUAL) return "manual";
  if (coolActive) return "cool";
  return "idle";
}

static const char *cbGetProfileName() {
  StateLockGuard guard;
  return selectedProfileName;
}

// ---- Command callbacks ----
// Same rule as the getters: these are entered from the AsyncTCP task, so they
// take the lock for the whole mutation. cbStartRoast() reaches into LittleFS
// while holding it - state lock first, filesystem second, never the other way
// round (see state_lock.h).
static void cbSetFanSpeed(int percent) {
  StateLockGuard guard;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  currentFanSpeed = percent;
  fan_set_speed(percent);
}

static bool cbStartCool(int speed, unsigned long durationSeconds);  // defined below

// Starts the cooling fan after a manual run if the user armed it (either on
// natural completion or on a manual stop).
static void maybeStartAutoCool() {
  if (manualAutoCool && manualCoolSpeed > 0 && manualCoolSeconds > 0) {
    cbStartCool(manualCoolSpeed, manualCoolSeconds);
  }
  manualAutoCool = false;
}

static bool cbStartManual(float targetTemp, unsigned long durationSeconds,
                          bool autoCool, int coolSpeed, unsigned long coolSeconds) {
  StateLockGuard guard;
  if (safety_faulted()) return false;  // alarm must clear before a new run
  if (targetTemp <= 0 || durationSeconds == 0) return false;
  manualTargetTemp = targetTemp;
  manualDurationSeconds = durationSeconds;
  manualStartMillis = millis();
  manualAutoCool = autoCool;
  manualCoolSpeed = coolSpeed;
  manualCoolSeconds = coolSeconds;
  heaterPID.reset();
  controlMode = MODE_MANUAL;
  return true;
}

static void cbStopManual() {
  StateLockGuard guard;
  bool wasManual = (controlMode == MODE_MANUAL);
  if (wasManual) controlMode = MODE_IDLE;
  manualStartMillis = 0;
  applyHeaterDuty(0);
  if (wasManual) maybeStartAutoCool();
}

static bool cbStartCool(int speed, unsigned long durationSeconds) {
  StateLockGuard guard;
  if (speed <= 0 || durationSeconds == 0) return false;
  coolSpeed = speed;
  coolDurationSeconds = durationSeconds;
  coolStartMillis = millis();
  coolActive = true;
  return true;
}

static void cbStopCool() {
  StateLockGuard guard;
  coolActive = false;
  coolStartMillis = 0;
  cbSetFanSpeed(0);
}

static void resetRorGuidance() {
  currentRorTarget = 0.0f;
  currentRorError = 0.0f;
  currentRorCorrection = 0.0f;
  currentRorGuidanceStep = -1;
  currentRorActive = false;
  lastRorCorrectionUpdate = 0;
}

static bool cbStartRoast(const String &profileName) {
  StateLockGuard guard;
  if (safety_faulted()) return false;  // alarm must clear before a new run
  String path = String(PROFILES_DIR) + "/" + profileName + ".json";
  if (!activeProfile.loadFromFile(path)) return false;

  snprintf(selectedProfileName, sizeof(selectedProfileName), "%s",
           profileName.c_str());
  heaterPID.reset();
  resetRorGuidance();
  roastStartMillis = millis();
  roastPaused = false;
  roastPauseStarted = 0;
  roastPausedTotal = 0;
  controlMode = MODE_PROFILE;
  return true;
}

static void cbStopRoast() {
  StateLockGuard guard;
  if (controlMode == MODE_PROFILE) controlMode = MODE_IDLE;
  roastStartMillis = 0;
  roastPaused = false;
  roastPauseStarted = 0;
  roastPausedTotal = 0;
  resetRorGuidance();
  applyHeaterDuty(0);
}

// Pause freezes the roast clock. The PID keeps regulating the setpoint that
// was active at the pause instant, so the current step is held in place.
static void cbPauseRoast() {
  StateLockGuard guard;
  if (controlMode != MODE_PROFILE || roastPaused) return;
  roastPaused = true;
  roastPauseStarted = millis();
}

static void cbResumeRoast() {
  StateLockGuard guard;
  if (controlMode != MODE_PROFILE || !roastPaused) return;
  roastPausedTotal += millis() - roastPauseStarted;
  roastPaused = false;
}

// Stops an active run because the safety latch tripped. The fan is left alone
// on purpose: the beans should keep getting air while the fault is handled.
static void abortRunForSafety() {
  controlMode = MODE_IDLE;
  roastStartMillis = 0;
  roastPaused = false;
  roastPauseStarted = 0;
  roastPausedTotal = 0;
  manualStartMillis = 0;
  manualAutoCool = false;
  resetRorGuidance();
  applyHeaterDuty(0);
}

// Clamp a delta to the per-second rate limit, accounting for the time since
// the last update. dt is in milliseconds and is treated as at least one sample
// interval so a single long stall cannot unlock a huge step.
static float rateLimitDelta(float delta, unsigned long dtMs) {
  if (dtMs == 0) return 0.0f;
  float maxDelta = ROR_GUIDANCE_MAX_STEP_PCT_PER_S * (dtMs / 1000.0f);
  if (delta > maxDelta) return maxDelta;
  if (delta < -maxDelta) return -maxDelta;
  return delta;
}

// Update the RoR-guidance correction. Returns the new correction value and
// writes the diagnostic state (target/error/active/step) into the globals.
// The correction is added on top of the PID output and is always 0 when any
// precondition is missing, so existing behaviour is unchanged unless a profile
// step explicitly enables rorTarget > 0.
static float updateRorGuidance(unsigned long nowMs) {
  currentRorTarget = 0.0f;
  currentRorError = 0.0f;
  currentRorGuidanceStep = -1;
  currentRorActive = false;

  if (controlMode != MODE_PROFILE) return 0.0f;
  if (roastPaused) return 0.0f;

  unsigned long elapsed = roastElapsedSeconds();
  int stepIdx = activeProfile.stepIndexAt(elapsed);
  float rorTarget = activeProfile.rorTargetAt(elapsed);
  if (stepIdx < 0 || rorTarget <= 0.0f) return 0.0f;

  if (safety_faulted()) return 0.0f;
  if (!ror_valid()) return 0.0f;
  if (isnan(currentET)) return 0.0f;

  float rorEt = ror_get_et();
  float error = rorTarget - rorEt;          // positive = too slow = need more heat
  float rawCorrection = error * ROR_GUIDANCE_GAIN;

  // Rate-limit the change of the correction, not the absolute value, so a
  // sudden cold-probe reading can only pull the correction by the configured
  // step per second instead of jumping straight to the clamp.
  unsigned long dtMs = (lastRorCorrectionUpdate == 0)
                           ? SENSOR_READ_INTERVAL_MS
                           : (nowMs - lastRorCorrectionUpdate);
  if ((long)dtMs < 0) dtMs = SENSOR_READ_INTERVAL_MS;  // millis() wrap
  float delta = rawCorrection - currentRorCorrection;
  delta = rateLimitDelta(delta, dtMs);
  float correction = currentRorCorrection + delta;

  if (correction > HEATER_MAX_DUTY_PCT) correction = HEATER_MAX_DUTY_PCT;
  if (correction < -HEATER_MAX_DUTY_PCT) correction = -HEATER_MAX_DUTY_PCT;

  currentRorTarget = rorTarget;
  currentRorError = error;
  currentRorCorrection = correction;
  currentRorGuidanceStep = stepIdx;
  currentRorActive = true;
  lastRorCorrectionUpdate = nowMs;
  return correction;
}

// Runs the active control mode. Called at the sensor sample rate so the PID
// sees a stable dt - the main loop itself spins far too fast for that.
// Every duty request goes through applyHeaterDuty(), which is where the fan
// interlock holds the element off when there is not enough airflow.
static void updateControl() {
  unsigned long nowMs = millis();
  float rorCorrection = updateRorGuidance(nowMs);

  if (controlMode == MODE_PROFILE) {
    unsigned long elapsed = roastElapsedSeconds();
    // Fan first: the interlock has to see this cycle's fan value, or the very
    // first duty request of a profile run would be judged against a stale fan.
    if (activeProfile.hasFan()) {
      cbSetFanSpeed((int)(activeProfile.fanAt(elapsed) + 0.5f));
    }
    float target = activeProfile.targetAt(elapsed);
    if (!isnan(target) && !isnan(currentBT)) {
      float pidDuty = (float)heaterPID.compute(target, currentBT);
      float duty = pidDuty + rorCorrection;
      if (duty > HEATER_MAX_DUTY_PCT) duty = HEATER_MAX_DUTY_PCT;
      if (duty < 0.0f) duty = 0.0f;
      applyHeaterDuty(duty);
    }
  } else if (controlMode == MODE_MANUAL) {
    unsigned long elapsed = (nowMs - manualStartMillis) / 1000;
    if (elapsed >= manualDurationSeconds) {
      controlMode = MODE_IDLE;
      applyHeaterDuty(0);
      maybeStartAutoCool();
    } else if (!isnan(currentBT)) {
      applyHeaterDuty(heaterPID.compute(manualTargetTemp, currentBT));
    }
  } else {
    applyHeaterDuty(0);
  }
}

// Runs the fan-only cool timer. Independent of the heater control modes, and
// applied after updateControl() so an explicit cool command wins the fan.
static void updateCool() {
  if (!coolActive) return;
  unsigned long elapsed = (millis() - coolStartMillis) / 1000;
  if (elapsed >= coolDurationSeconds) {
    coolActive = false;
    cbSetFanSpeed(0);
  } else {
    cbSetFanSpeed(coolSpeed);
  }
}

// ---- WiFi (non-blocking) ----
// setup() only starts the connection; this runs from loop() and keeps it
// going. Nothing here ever waits: a missing or flaky access point costs a
// log line and a re-kick, never a control cycle. WiFi.setAutoReconnect()
// covers the ordinary dropout, the periodic begin() covers the case where the
// SDK has given up (wrong channel, AP that renumbered, WPA rekey gone bad).
static unsigned long wifiLastAttempt = 0;
static bool wifiWasConnected = false;

// ---- OTA ----
// Network updates, so the next one never needs the USB cable again. Started
// from serviceWifi() the moment the link is up: with a non-blocking
// connection the interface can come up long after setup() has returned.
//
// Two rules keep this safe to run next to a live heater:
//
//   * Nothing is served while a roast, a manual session or the cool-down is
//     running. handle() is simply not called, so espota times out instead of
//     interrupting half a roast - and the reason is logged once, on the
//     serial console, so a timeout during a run is explicable.
//   * onStart() latches the heater off before the first byte lands. No
//     control cycle runs during the transfer, and a windowed heater left at
//     its last state would hold the element ON for the whole upload. The
//     latch is cleared only by heater_clear_emergency(), which never runs
//     here: a finished transfer reboots, and a failed one restarts too, so
//     the device always comes back with the heater off and no session.
//
// OTA_PASSWORD comes from include/secrets.h. Empty means the service is not
// started at all, which is the same rule MQTT follows: no password, no service.
static bool otaStarted = false;
static bool otaSuppressed = false;
static volatile bool otaReboot = false;

static void serviceOta() {
  if (otaReboot) {
    Serial.println("[OTA] transfer finished - restarting into the new image");
    ESP.restart();
    return;  // the real restart does not return; the host stub does
  }
  // Push OTA served itself over HTTP in the AsyncTCP task; loop() only does
  // the restart, after that task has sent its response.
  if (web_ota_reboot_pending()) {
    Serial.println("[OTA] push upload verified - restarting into the new image");
    ESP.restart();
    return;
  }
  if (!otaStarted) return;

  const bool busy =
      cbGetRoastActive() || cbGetManualActive() || cbGetCoolActive();
  if (busy) {
    if (!otaSuppressed) {
      otaSuppressed = true;
      Serial.println(
          "[OTA] paused: a session is running, transfers are not accepted");
    }
    return;
  }
  otaSuppressed = false;
  ArduinoOTA.handle();
}

static void startOta() {
  if (otaStarted) return;

  if (OTA_PASSWORD[0] == '\0') {
    static bool reported = false;
    if (!reported) {
      reported = true;
      Serial.println(
          "[OTA] not started: OTA_PASSWORD is empty in include/secrets.h");
    }
    return;
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] transfer starting - heater latched off, run aborted");
    heater_emergency_off();
    abortRunForSafety();
  });
  ArduinoOTA.onEnd([]() { otaReboot = true; });
  ArduinoOTA.onError([](ota_error_t err) {
    // Restart as well: the latch set by onStart() must never outlive the
    // attempt, and the old image is still the boot target when a transfer
    // fails (otadata only flips after a successful end).
    Serial.print("[OTA] transfer failed, code ");
    Serial.println((int)err);
    otaReboot = true;
  });

  ArduinoOTA.begin();
  otaStarted = true;
  Serial.print("[OTA] ready on ");
  Serial.print(OTA_HOSTNAME);
  Serial.print(".local, port ");
  Serial.println(OTA_PORT);
}

static void serviceWifi() {
  const bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected != wifiWasConnected) {
    if (connected) {
      Serial.print("[WiFi] connected, IP: ");
      Serial.println(WiFi.localIP());
      startOta();
    } else {
      Serial.println("[WiFi] link lost - reconnecting in the background");
    }
    wifiWasConnected = connected;
  }

  if (connected) return;
  if (millis() - wifiLastAttempt < WIFI_RETRY_INTERVAL_MS) return;
  wifiLastAttempt = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[WiFi] still not connected - retrying in the background");
}

void setup() {
  // Heater and fan pins first, before anything that can delay. Between reset
  // and here the pins are plain inputs, so GPIO26 floats in front of the SSR
  // input for the length of the ROM boot plus this code - and a floating SSR
  // input is how an element can go live at power-up. A pull-down on SSR IN+ is
  // still the only thing that covers the milliseconds before the ROM hands
  // over, but from the first instruction of setup() onward the pin is driven
  // low and the fan is at 0 %.
  heater_init();
  fan_init();

  Serial.begin(115200);
  delay(200);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }
  if (!LittleFS.exists(PROFILES_DIR)) {
    LittleFS.mkdir(PROFILES_DIR);
  }

  safety_init();
  sensors_init();
  ror_reset();

  // A latch restored from NVS has to reach the heater before the first sensor
  // sample, not 250 ms after it: the SSR pin is already low from heater_init(),
  // this holds it there by refusing every duty from here on.
  if (safety_faulted()) {
    heater_emergency_off();
    abortRunForSafety();
    Serial.print("[SAFETY] booting with a persisted alarm: ");
    Serial.println(safety_code_text());
  }

  // Start the connection and move on - never wait for it. The old code spun
  // up to 15 s here, which meant a roaster whose AP was missing took 15 s to
  // react to anything at all. serviceWifi() in loop() reports, retries and
  // keeps the control loop free either way.
  wifiLastAttempt = millis();
  wifiWasConnected = false;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] connecting to ");
  Serial.print(WIFI_SSID);
  Serial.println(" (non-blocking, the control loop starts now)");

  WebServerCallbacks callbacks = {
    cbGetBT,
    cbGetET,
    cbGetRorBt,
    cbGetRorEt,
    cbGetHeaterDuty,
    cbGetFanSpeed,
    cbGetRoastActive,
    cbGetElapsedSeconds,
    cbGetRoastPaused,
    cbGetManualActive,
    cbGetManualTargetTemp,
    cbGetManualRemainingSeconds,
    cbGetManualAutoCool,
    cbGetCoolActive,
    cbGetCoolSpeed,
    cbGetCoolRemainingSeconds,
    cbGetSafetyFault,
    cbGetSafetyReason,
    cbGetFanFault,
    cbGetWifiConnected,
    cbGetRorGuidance,
    cbGetRorTarget,
    cbGetRorError,
    cbGetRorActive,
    cbIsRunActive,
    cbSetFanSpeed,
    cbStartManual,
    cbStopManual,
    cbStartCool,
    cbStopCool,
    cbStartRoast,
    cbStopRoast,
    cbPauseRoast,
    cbResumeRoast,
  };
  web_server_init(callbacks);
  if (test_web_server_cb_captured) test_web_server_cb_captured(callbacks);

  OtaPushCallbacks otaCallbacks = { cbIsRunActive };
  ota_push_init(otaCallbacks);
  g_otaCallbacks = otaCallbacks;

  MqttCallbacks mqttCallbacks = {
    cbGetBT,
    cbGetET,
    cbGetRorBt,
    cbGetRorEt,
    cbGetHeaterDuty,
    cbGetFanSpeed,
    cbGetModeName,
    cbGetProfileName,
    cbGetElapsedSeconds,
    cbGetSafetyFault,
    cbGetSafetyReason,
    cbGetRoastActive,
    cbGetFanFault,
    cbGetRorGuidance,
    cbGetRorTarget,
    cbGetRorError,
    cbGetRorActive,
  };
  mqtt_init(mqttCallbacks);
}

void loop() {
  unsigned long now = millis();

  serviceWifi();
  serviceOta();

  if (now - lastSensorRead >= SENSOR_READ_INTERVAL_MS) {
    lastSensorRead = now;
    // The SPI read happens before the lock: it touches only sensors.cpp
    // state, which is written from this task alone, and holding the lock
    // across it would only make web requests wait longer.
    SensorReading r = sensors_read();

    // Everything below mutates the state the AsyncTCP task reads.
    StateLockGuard stateLock;
    currentBT = r.bt;
    currentET = r.et;

    // Rate of rise: same cadence as the sample itself (250 ms) - and the
    // module stores EVERY one of those samples in its ring (ROR_BUFFER_LEN,
    // 128 entries = 32 s) and refits both rates on each of them. Under the
    // lock, so the web and MQTT getters never see a half-written history.
    ror_update(now, r.bt, r.et);

    // The stuck-probe check is armed by the heater actually asking for
    // power, not by "heat recently" - see include/config.h for why that is
    // what keeps the cooling tail from false-tripping.
    safety_update(r, currentHeaterDuty > 0);

    if (safety_faulted()) {
      if (!safetyLatched) {
        safetyLatched = true;
        Serial.print("[SAFETY] FAULT: ");
        Serial.print(safety_code_text());
        Serial.println(" - heater off, run aborted");
        abortRunForSafety();
      }
      // Re-asserted every cycle: the heater stays latched off until the
      // condition is measurably gone.
      heater_emergency_off();
    } else if (safetyLatched) {
      safetyLatched = false;
      heater_clear_emergency();
      Serial.println("[SAFETY] alarm cleared - heater re-armed, start the run again manually");
    }

    if (!safety_faulted()) updateControl();
    updateCool();
  }

  // heater_set_duty() runs from the web task as well (cbStopManual,
  // cbStopRoast, abortRunForSafety), so the window computation is guarded
  // too. It never blocks, so the lock is held for microseconds.
  {
    StateLockGuard stateLock;
    heater_update();
  }

  // mqtt_update() deliberately stays outside the lock: PubSubClient connects
  // synchronously and can hold loop() for seconds while the broker is down,
  // and freezing every HTTP request for that long would be worse than a
  // status payload that straddles one change. It reads through the same
  // callbacks, which take the lock themselves.
  mqtt_update();

  // AsyncWebServer handles requests in the background, no explicit
  // "server.handleClient()" call needed here like with the sync web server.
}
