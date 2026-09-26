#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>

#include "config.h"
#include "pid.h"
#include "sensors.h"
#include "safety.h"
#include "heater_control.h"
#include "fan_control.h"
#include "roast_profile.h"
#include "web_server.h"
#include "mqtt_client.h"

// One PID instance, shared between manual and profile runs. It is reset
// whenever a run starts so the integral does not carry over between runs.
static SimplePID heaterPID(PID_KP, PID_KI, PID_KD, 0, 100);

enum ControlMode { MODE_IDLE, MODE_MANUAL, MODE_PROFILE };
static ControlMode controlMode = MODE_IDLE;

static float currentBT = NAN;
static float currentET = NAN;
static float currentHeaterDuty = 0;
static int currentFanSpeed = 0;

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
static String selectedProfileName = "";

// ---- Status callbacks ----
static float cbGetBT() { return currentBT; }
static float cbGetET() { return currentET; }
static float cbGetHeaterDuty() { return currentHeaterDuty; }
static int cbGetFanSpeed() { return currentFanSpeed; }

static bool cbGetRoastActive() { return controlMode == MODE_PROFILE; }

// Roast time excludes any paused stretches, so pausing holds the current
// setpoint in place without advancing the profile.
static unsigned long roastElapsedSeconds() {
  if (controlMode != MODE_PROFILE) return 0;
  unsigned long now = millis();
  unsigned long paused = roastPausedTotal + (roastPaused ? now - roastPauseStarted : 0);
  return (now - roastStartMillis - paused) / 1000;
}

static unsigned long cbGetElapsedSeconds() { return roastElapsedSeconds(); }

static bool cbGetRoastPaused() { return roastPaused; }

static bool cbGetManualActive() { return controlMode == MODE_MANUAL; }
static float cbGetManualTargetTemp() { return manualTargetTemp; }
static bool cbGetManualAutoCool() { return manualAutoCool; }

static unsigned long cbGetManualRemainingSeconds() {
  if (controlMode != MODE_MANUAL) return 0;
  unsigned long elapsed = (millis() - manualStartMillis) / 1000;
  if (elapsed >= manualDurationSeconds) return 0;
  return manualDurationSeconds - elapsed;
}

static bool cbGetCoolActive() { return coolActive; }
static int cbGetCoolSpeed() { return coolSpeed; }

static unsigned long cbGetCoolRemainingSeconds() {
  if (!coolActive) return 0;
  unsigned long elapsed = (millis() - coolStartMillis) / 1000;
  if (elapsed >= coolDurationSeconds) return 0;
  return coolDurationSeconds - elapsed;
}

static bool cbGetSafetyFault() { return safety_faulted(); }
static const char *cbGetSafetyReason() { return safety_code_text(); }
static bool cbGetFanFault() { return fanInterlockFault; }

static const char *cbGetModeName() {
  if (controlMode == MODE_PROFILE) return "profile";
  if (controlMode == MODE_MANUAL) return "manual";
  if (coolActive) return "cool";
  return "idle";
}

static const char *cbGetProfileName() { return selectedProfileName.c_str(); }

// ---- Command callbacks ----
static void cbSetFanSpeed(int percent) {
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
  bool wasManual = (controlMode == MODE_MANUAL);
  if (wasManual) controlMode = MODE_IDLE;
  manualStartMillis = 0;
  applyHeaterDuty(0);
  if (wasManual) maybeStartAutoCool();
}

static bool cbStartCool(int speed, unsigned long durationSeconds) {
  if (speed <= 0 || durationSeconds == 0) return false;
  coolSpeed = speed;
  coolDurationSeconds = durationSeconds;
  coolStartMillis = millis();
  coolActive = true;
  return true;
}

static void cbStopCool() {
  coolActive = false;
  coolStartMillis = 0;
  cbSetFanSpeed(0);
}

static bool cbStartRoast(const String &profileName) {
  if (safety_faulted()) return false;  // alarm must clear before a new run
  String path = String(PROFILES_DIR) + "/" + profileName + ".json";
  if (!activeProfile.loadFromFile(path)) return false;

  selectedProfileName = profileName;
  heaterPID.reset();
  roastStartMillis = millis();
  roastPaused = false;
  roastPauseStarted = 0;
  roastPausedTotal = 0;
  controlMode = MODE_PROFILE;
  return true;
}

static void cbStopRoast() {
  if (controlMode == MODE_PROFILE) controlMode = MODE_IDLE;
  roastStartMillis = 0;
  roastPaused = false;
  roastPauseStarted = 0;
  roastPausedTotal = 0;
  applyHeaterDuty(0);
}

// Pause freezes the roast clock. The PID keeps regulating the setpoint that
// was active at the pause instant, so the current step is held in place.
static void cbPauseRoast() {
  if (controlMode != MODE_PROFILE || roastPaused) return;
  roastPaused = true;
  roastPauseStarted = millis();
}

static void cbResumeRoast() {
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
  applyHeaterDuty(0);
}

// Runs the active control mode. Called at the sensor sample rate so the PID
// sees a stable dt - the main loop itself spins far too fast for that.
// Every duty request goes through applyHeaterDuty(), which is where the fan
// interlock holds the element off when there is not enough airflow.
static void updateControl() {
  if (controlMode == MODE_PROFILE) {
    unsigned long elapsed = roastElapsedSeconds();
    // Fan first: the interlock has to see this cycle's fan value, or the very
    // first duty request of a profile run would be judged against a stale fan.
    if (activeProfile.hasFan()) {
      cbSetFanSpeed((int)(activeProfile.fanAt(elapsed) + 0.5f));
    }
    float target = activeProfile.targetAt(elapsed);
    if (!isnan(target) && !isnan(currentBT)) {
      applyHeaterDuty(heaterPID.compute(target, currentBT));
    }
  } else if (controlMode == MODE_MANUAL) {
    unsigned long elapsed = (millis() - manualStartMillis) / 1000;
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

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed - continuing offline, web UI unreachable.");
  }

  WebServerCallbacks callbacks = {
    cbGetBT,
    cbGetET,
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

  MqttCallbacks mqttCallbacks = {
    cbGetBT,
    cbGetET,
    cbGetHeaterDuty,
    cbGetFanSpeed,
    cbGetModeName,
    cbGetProfileName,
    cbGetElapsedSeconds,
    cbGetSafetyFault,
    cbGetSafetyReason,
    cbGetRoastActive,
    cbGetFanFault,
  };
  mqtt_init(mqttCallbacks);
}

void loop() {
  unsigned long now = millis();

  if (now - lastSensorRead >= SENSOR_READ_INTERVAL_MS) {
    lastSensorRead = now;
    SensorReading r = sensors_read();
    currentBT = r.bt;
    currentET = r.et;

    safety_update(r);

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

  heater_update();
  mqtt_update();

  // AsyncWebServer handles requests in the background, no explicit
  // "server.handleClient()" call needed here like with the sync web server.
}
