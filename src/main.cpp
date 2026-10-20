#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>

#include "config.h"
#include "pid.h"
#include "filter.h"
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

// The bean temperature as the CONTROL loop reads it: the same sample as
// currentBT, low-passed at PID_INPUT_FILTER_TAU_S (include/filter.h). Kept
// separate from currentBT on purpose - currentBT stays the raw reading, because
// that is what the safety layer, the RoR fit, MQTT and /api/status must see. A
// filter on the sensor channel would turn a dropped probe into a plausible ramp
// and hide exactly what SENSOR_FAULT_MAX_JUMP_C is looking for.
static EmaFilter btControlFilter(PID_INPUT_FILTER_TAU_S);
static float currentBTFiltered = NAN;

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
// Timestamp of the last control sample, used as the filter's dt. The control
// path is sampled at SENSOR_READ_INTERVAL_MS, so that is also the fallback when
// the measured gap is unusable (zero, or a stalled loop).
static unsigned long lastControlSampleMs = 0;

// Rate-of-rise guidance state, updated each control cycle. Guidance is only
// active in profile mode when the current step has rorTarget > 0 and the RoR
// estimate has warmed up. These are reported in /api/status and MQTT. The rate
// it steers on is a build choice: rorEt, or rorBt when the ET channel is not
// read at all (ROR_GUIDANCE_ON_BT, include/config.h).
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
//
// It judges on the REACHED fan speed (fan_actual_pct()), not on the requested
// one: the duty slews up at FAN_SLEW_PCT_PER_SEC (config.h), so "the user asked
// for 100 % a moment ago" is not airflow yet. On the request alone the element
// could be lit for the seconds the fan is still spinning up - which is exactly
// the state this interlock exists for.
static void applyHeaterDuty(float duty) {
  if (duty > 0.0f && fan_actual_pct() < FAN_MIN_FOR_HEATER_PCT) {
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
// Reached three ways: the cooling panel, "Avbryt och kyl" on a running roast
// (cbStopRoastAndCool) and a profile whose last step has run out
// (finishProfileLocked). All three go through startCoolLocked().
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

// PID diagnostics (include/pid.h): the three contributions as they were last
// applied, and the raw integral state. Reported as 0 while no run is active -
// the terms belong to the last compute(), and a stale P term next to a stopped
// machine would be read as a live one while trimming.
static float cbGetPidP() {
  StateLockGuard guard;
  return controlMode == MODE_IDLE ? 0.0f : (float)heaterPID.pTerm();
}

static float cbGetPidI() {
  StateLockGuard guard;
  return controlMode == MODE_IDLE ? 0.0f : (float)heaterPID.iTerm();
}

static float cbGetPidD() {
  StateLockGuard guard;
  return controlMode == MODE_IDLE ? 0.0f : (float)heaterPID.dTerm();
}

static float cbGetPidIntegral() {
  StateLockGuard guard;
  return controlMode == MODE_IDLE ? 0.0f : (float)heaterPID.integral();
}

// The bean temperature the PID actually read this cycle - the filtered one. The
// raw value stays in `bt` (above), which is what the safety layer, the RoR fit
// and HA see, so the two can be compared while tuning without the firmware
// having to say which is which.
static float cbGetFilteredBT() {
  StateLockGuard guard;
  return currentBTFiltered;
}

// True between the unmount in onStart() and the reboot (or the remount in
// onError()). While it is set, /profiles is not readable - the partition is
// being rewritten - so the profile endpoints must refuse instead of reading
// whatever the half-written image happens to look like as a filesystem.
// Declared up here because the web callback below reads it.
static volatile bool otaFsUnmounted = false;

// While a filesystem OTA transfer rewrites the profile partition, the profile
// endpoints have nothing readable to read. No lock: it is a volatile flag set
// from the OTA callback, and taking the state lock on a path that runs during
// a transfer would only add a way to block.
//
// This overrides the weak default in web_server.cpp, which is what makes the
// host tests for the profile endpoints work without main.cpp. Two sources can
// unmount: ArduinoOTA's onStart() (the flag above) and the push route for the
// filesystem image (ota_push_fs_unmounted()). Either one means /profiles has
// nothing safe to read.
bool web_fs_available() { return !otaFsUnmounted && !ota_push_fs_unmounted(); }

static float cbGetHeaterDuty() {
  StateLockGuard guard;
  return currentHeaterDuty;
}

static int cbGetFanSpeed() {
  StateLockGuard guard;
  return currentFanSpeed;
}

// The reached speed, next to the requested one above. The two differ exactly
// while the ramp is running, which is when the interlock is holding the element
// off - so this is the field that says why.
static float cbGetFanActual() {
  StateLockGuard guard;
  return fan_actual_pct();
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

// The other half of the OTA window: is the element asking for power right now?
// The window is "no run active AND the element off" - a heater left conducting
// by anything else (a stale duty, a run that just gave up) must block a flash
// just as hard as a running roast does.
static bool cbIsHeaterAsking() {
  StateLockGuard guard;
  return currentHeaterDuty > 0;
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
// The body of the fan command. The caller holds the state lock: two paths want
// to set the fan from inside a critical section they already own (the stop-
// and-cool transition and the end of a profile), and re-entering the recursive
// lock there would blur what "one lock" means.
static void setFanSpeedLocked(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  currentFanSpeed = percent;
  fan_set_speed(percent);
}

static void cbSetFanSpeed(int percent) {
  StateLockGuard guard;
  setFanSpeedLocked(percent);
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
  // Start the control input from the reading that exists right now instead of
  // from the filter's leftover state (or from nothing, which would look like a
  // cold probe for one sample): the first control cycle sees the real machine.
  btControlFilter.seed(currentBT);
  currentBTFiltered = currentBT;
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

// Starts the fan-only cool timer. The caller holds the state lock - this is the
// one body behind the cooling panel, "Avbryt och kyl" and the end of a profile,
// so all three leave exactly the same state behind. It deliberately does NOT
// touch the fan: updateCool() applies coolSpeed on the next sample, and the two
// paths that must have the fan running immediately set it themselves in the
// same critical section (they are already inside one).
static bool startCoolLocked(int speed, unsigned long durationSeconds) {
  if (speed <= 0 || durationSeconds == 0) return false;
  coolSpeed = speed;
  coolDurationSeconds = durationSeconds;
  coolStartMillis = millis();
  coolActive = true;
  return true;
}

static bool cbStartCool(int speed, unsigned long durationSeconds) {
  StateLockGuard guard;
  return startCoolLocked(speed, durationSeconds);
}

// Clears the cool timer. The caller holds the state lock, and it deliberately
// does NOT touch the fan - the same rule the roast stop below follows. A caller
// that ends a cooling because the whole run has to stop commands the fan
// itself, and one that ends it to hand over to a fresh cool speed (the profile
// end, "Avbryt och kyl") must not put the fan through 0 % first.
static void clearCoolLocked() {
  coolActive = false;
  coolStartMillis = 0;
}

static void cbStopCool() {
  StateLockGuard guard;
  clearCoolLocked();
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
  btControlFilter.seed(currentBT);
  currentBTFiltered = currentBT;
  resetRorGuidance();
  roastStartMillis = millis();
  roastPaused = false;
  roastPauseStarted = 0;
  roastPausedTotal = 0;
  controlMode = MODE_PROFILE;
  return true;
}

// The state part of a stop, shared by the plain stop and stop-and-cool. The
// caller holds the state lock. It leaves the fan alone on purpose: a stop that
// wants air says so itself, and one that is about to hand over to a cool speed
// must not pass through 0 % on the way.
//
// Cooling IS ended here, though. From the roast tab a running cool-down is
// still "the run": Stop pressed after "Avbryt och kyl" used to leave the fan
// spinning with the only stop button on the manual tab (measured on the machine
// 2026-10-20 - the fan kept running until /api/cool/stop was called by hand).
// The two callers that want a cool-down after this call (cbStopRoastAndCool,
// finishProfileLocked) start it after the stop, so ending it here cannot race
// them.
static void stopRoastLocked() {
  if (controlMode == MODE_PROFILE) controlMode = MODE_IDLE;
  roastStartMillis = 0;
  roastPaused = false;
  roastPauseStarted = 0;
  roastPausedTotal = 0;
  resetRorGuidance();
  applyHeaterDuty(0);
  clearCoolLocked();
}

static void cbStopRoast() {
  StateLockGuard guard;
  stopRoastLocked();
  // A stop from the roast view means everything off, the fan included: the
  // profile's last fan speed must not keep running where no control on that tab
  // can stop it. A SAFETY abort still leaves the fan alone (abortRunForSafety) -
  // there the beans need air while the fault is handled.
  setFanSpeedLocked(0);
}

// "Avbryt och kyl": stop the roast and start the cool-down in ONE critical
// section, so no scheduler pass can put the fan through 0 % in between - which
// is exactly what two separate requests (/api/roast/stop followed by
// /api/cool/start) leave room for. Returns false and changes nothing when the
// values are out of range: a stop without cooling is the plain Stop button, and
// silently falling back to it would look like the roaster ignored the command
// while the beans kept cooking.
static bool cbStopRoastAndCool(int speed, unsigned long durationSeconds) {
  StateLockGuard guard;
  if (speed < COOL_SPEED_MIN_PCT || speed > COOL_SPEED_MAX_PCT ||
      durationSeconds == 0) {
    return false;
  }
  stopRoastLocked();
  if (!startCoolLocked(speed, durationSeconds)) return false;
  // Applied here rather than on the next control cycle: the fan is already
  // spinning at the profile's speed, so this is a level change, not a start.
  setFanSpeedLocked(speed);
  return true;
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

// The profile's last step has run out. What happens next is the profile's own
// choice: coolFan/coolMinutes set -> the existing fan-only cool timer starts in
// the same critical section as the stop, so the fan goes from the last step's
// speed straight to the cool speed; absent or 0 -> the profile ends the way it
// always did (heat off), and the fan is stopped here because nothing else would
// ever command it again.
//
// Called with the state lock held, from updateControl() - loop() takes it once
// per sample around that call. A manual stop never comes through here: an
// aborted roast is an intentional act, and "Avbryt och kyl" is the command for
// the case where the beans should still get air.
static void finishProfileLocked() {
  const int speed = (int)(activeProfile.coolFanPct() + 0.5f);
  float minutes = activeProfile.coolMinutes();
  if (minutes > COOL_MINUTES_MAX) minutes = COOL_MINUTES_MAX;
  const unsigned long seconds = (unsigned long)(minutes * 60.0f + 0.5f);

  stopRoastLocked();
  if (speed >= COOL_SPEED_MIN_PCT && speed <= COOL_SPEED_MAX_PCT && seconds > 0 &&
      startCoolLocked(speed, seconds)) {
    setFanSpeedLocked(speed);
  } else {
    setFanSpeedLocked(0);
  }
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
// step explicitly enables rorTarget > 0. The measured rate it steers on is
// rorEt, or rorBt in a BT-only build (ROR_GUIDANCE_ON_BT) - the error reported
// here is always target minus the channel actually in use.
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

  // Which channel the loop compares the target against is a build choice
  // (ROR_GUIDANCE_ON_BT, include/config.h): rorEt while the ET channel is
  // watched, rorBt when it is not read at all. The gate has to sit on the
  // channel behind the number, not on the other one: in a BT-only build ET is
  // NaN by design, and a gate on a NaN ET is exactly what used to make the
  // guidance silently dead there.
  //
  // The gate is on the live reading, not on the fitted rate: ror.cpp keeps
  // publishing the previous rate while a sample is missing (a probe that has
  // never read, a faulted read), so the fitted number alone cannot say whether
  // there is a measurement behind it right now.
#if ROR_GUIDANCE_ON_BT
  if (isnan(currentBT)) return 0.0f;
  float rorActual = ror_get_bt();
#else
  if (isnan(currentET)) return 0.0f;
  float rorActual = ror_get_et();
#endif

  float error = rorTarget - rorActual;      // positive = too slow = need more heat
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
    // The last step has run out: the profile is over. Until this check existed
    // the run simply held its final setpoint forever, so a profile never ended
    // by itself; it ends here instead, and hands over to the profile's own
    // cool-down if it has one. A pause holds the clock, so a paused profile
    // holds its last setpoint exactly as a pause does everywhere else.
    const unsigned long profileEnd = activeProfile.totalSeconds();
    if (!roastPaused && profileEnd > 0 && elapsed >= profileEnd) {
      finishProfileLocked();
      return;
    }
    // Fan first: the interlock has to see this cycle's fan value, or the very
    // first duty request of a profile run would be judged against a stale fan.
    if (activeProfile.hasFan()) {
      cbSetFanSpeed((int)(activeProfile.fanAt(elapsed) + 0.5f));
    }
    float target = activeProfile.targetAt(elapsed);
    // The PID reads the FILTERED bean temperature (currentBTFiltered); the raw
    // currentBT is what the safety layer, the RoR fit and the UI get. Guarding
    // on the filtered value is the honest test of "is there a number for the
    // controller at all".
    if (!isnan(target) && !isnan(currentBTFiltered)) {
      float pidDuty = (float)heaterPID.compute(target, currentBTFiltered);
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
    } else if (!isnan(currentBTFiltered)) {
      applyHeaterDuty(heaterPID.compute(manualTargetTemp, currentBTFiltered));
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
  if (busy || ota_push_in_progress()) {
    if (!otaSuppressed) {
      otaSuppressed = true;
      if (ota_push_in_progress()) {
        Serial.println(
            "[OTA] paused: a push OTA transfer is in progress, ArduinoOTA is not served");
      } else {
        Serial.println(
            "[OTA] paused: a session is running, transfers are not accepted");
      }
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
    // A push OTA transfer already owns Update; starting ArduinoOTA now would
    // abort that transfer and reboot the device. This should not be reachable
    // because serviceOta() refuses to call handle() while a push is in
    // progress, but the guard here makes the invariant robust against any path
    // that manages to fire onStart.
    if (ota_push_in_progress()) {
      Serial.println("[OTA] ArduinoOTA start refused: a push OTA transfer is already in progress");
      return;
    }

    // What is being written decides what has to get out of the way.
    //
    // Update.begin() is called by the library BEFORE this callback, and for
    // U_SPIFFS it writes straight to the flash partition the filesystem lives
    // on. LittleFS must not be mounted while that happens: a mounted
    // filesystem holds its own metadata and buffers in RAM and would be
    // writing its view of the same blocks over/behind the incoming image.
    // The symptom is not a clean refusal - the transfer starts, runs for a
    // while, and then dies, which is exactly what an interrupt mid-write
    // looks like from the outside.
    //
    // So U_SPIFFS unmounts. U_FLASH goes to the inactive app slot and never
    // shares a block with the filesystem, so it leaves LittleFS alone (and
    // must: the profiles are read for the rest of this boot).
    const bool writingFs = (ArduinoOTA.getCommand() == U_SPIFFS);
    Serial.print("[OTA] transfer starting - heater latched off, run aborted (");
    Serial.print(writingFs ? "filesystem" : "firmware");
    Serial.println(")");
    heater_emergency_off();
    abortRunForSafety();
    if (writingFs) {
      // Close the filesystem before the incoming image touches its partition.
      // Nothing after this may read /profiles until it is mounted again - and
      // after a successful transfer the device reboots, so for that path the
      // only thing that matters is that this returns quickly.
      LittleFS.end();
      otaFsUnmounted = true;
      Serial.println("[OTA] LittleFS unmounted for the filesystem transfer");
    }
  });
  ArduinoOTA.onEnd([]() { otaReboot = true; });
  ArduinoOTA.onError([](ota_error_t err) {
    // If a push OTA transfer owns Update, onStart() refused to set up the
    // ArduinoOTA transfer. Any error that arrives in that state must not
    // reboot the device out from under the legitimate push.
    if (ota_push_in_progress()) {
      Serial.print("[OTA] ArduinoOTA error ignored: a push OTA transfer is in progress (code ");
      Serial.print((int)err);
      Serial.println(")");
      return;
    }

    // Restart as well: the latch set by onStart() must never outlive the
    // attempt, and the old image is still the boot target when a transfer
    // fails (otadata only flips after a successful end).
    Serial.print("[OTA] transfer failed, code ");
    Serial.println((int)err);
    // A failed filesystem transfer leaves the partition mounted in the
    // firmware's eyes but not in ours: remount before the restart so the
    // profiles are readable if the device does not come back up cleanly.
    if (otaFsUnmounted) {
      LittleFS.begin(true);
      otaFsUnmounted = false;
      Serial.println("[OTA] LittleFS remounted after the failed transfer");
    }
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
    cbGetFanActual,
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
    cbGetPidP,
    cbGetPidI,
    cbGetPidD,
    cbGetPidIntegral,
    cbGetFilteredBT,
    cbIsRunActive,
    cbSetFanSpeed,
    cbStartManual,
    cbStopManual,
    cbStartCool,
    cbStopCool,
    cbStartRoast,
    cbStopRoast,
    cbStopRoastAndCool,
    cbPauseRoast,
    cbResumeRoast,
  };
  web_server_init(callbacks);
  if (test_web_server_cb_captured) test_web_server_cb_captured(callbacks);

  OtaPushCallbacks otaCallbacks = { cbIsRunActive, cbIsHeaterAsking,
                                    []() -> uint32_t { return millis(); },
                                    heater_emergency_off, abortRunForSafety };
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

    // The control input: the same reading, low-passed for the PID only. It is
    // updated HERE, after currentBT, and safety_update() below is still fed the
    // raw sample `r` - that order is the whole promise of this filter: the
    // controller gets a signal it can differentiate, and the safety layer keeps
    // a signal it can trip on. dt is the measured gap between control samples,
    // with the nominal interval as the fallback for a stalled loop or a
    // first sample; anything larger would be one huge filter step.
    unsigned long controlDtMs = now - lastControlSampleMs;
    lastControlSampleMs = now;
    if (controlDtMs == 0 || controlDtMs > 5000) controlDtMs = SENSOR_READ_INTERVAL_MS;
    if (!isnan(currentBT)) {
      currentBTFiltered =
          (float)btControlFilter.update(currentBT, controlDtMs / 1000.0);
    } else {
      // No reading at all (never connected): there is nothing to filter. The
      // filter re-seeds from the first real sample, so a probe that comes back
      // does not first have to pull its old value out of the filter.
      btControlFilter.reset();
      currentBTFiltered = NAN;
    }

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
  //
  // fan_update() advances the fan ramp here for the same reason: the target is
  // written by the web task (cbSetFanSpeed) and the reached speed is read by the
  // interlock, so both sides of the ramp belong under the lock. Every pass, not
  // once per sensor sample, so the duty climbs smoothly instead of in 6 %
  // steps.
  {
    StateLockGuard stateLock;
    fan_update();
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
