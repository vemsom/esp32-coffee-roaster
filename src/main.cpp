#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>

#include "config.h"
#include "pid.h"
#include "sensors.h"
#include "heater_control.h"
#include "fan_control.h"
#include "roast_profile.h"
#include "web_server.h"

static SimplePID heaterPID(PID_KP, PID_KI, PID_KD, 0, 100);

static float currentBT = NAN;
static float currentET = NAN;
static float currentHeaterDuty = 0;
static int currentFanSpeed = 0;

static bool roastActive = false;
static bool manualHeaterMode = false;
static float manualHeaterDuty = 0;
static unsigned long roastStartMillis = 0;
static RoastProfile activeProfile;

static unsigned long lastSensorRead = 0;

static float cbGetBT() { return currentBT; }
static float cbGetET() { return currentET; }
static float cbGetHeaterDuty() { return currentHeaterDuty; }
static int cbGetFanSpeed() { return currentFanSpeed; }
static bool cbGetRoastActive() { return roastActive; }

static unsigned long cbGetElapsedSeconds() {
    if (!roastActive) return 0;
  return (millis() - roastStartMillis) / 1000;
}

static void cbSetFanSpeed(int percent) {
    currentFanSpeed = percent;
  fan_set_speed(percent);
}

static void cbSetManualHeaterDuty(float percent) {
    manualHeaterMode = true;
  manualHeaterDuty = percent;
}

static bool cbStartRoast(const String &profileName) {
    String path = String(PROFILES_DIR) + "/" + profileName + ".json";
  if (!activeProfile.loadFromFile(path)) return false;

  manualHeaterMode = false;
  heaterPID.reset();
  roastStartMillis = millis();
  roastActive = true;
  return true;
}

static void cbStopRoast() {
    roastActive = false;
  manualHeaterMode = false;
  heater_set_duty(0);
}

void setup() {
    Serial.begin(115200);
  delay(200);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }
  if (!LittleFS.exists(PROFILES_DIR)) {
    LittleFS.mkdir(PROFILES_DIR);
  }

  sensors_init();
  heater_init();
  fan_init();

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
    cbSetFanSpeed,
    cbSetManualHeaterDuty,
    cbStartRoast,
    cbStopRoast,
};
  web_server_init(callbacks);
}

void loop() {
    unsigned long now = millis();

  if (now - lastSensorRead >= SENSOR_READ_INTERVAL_MS) {
    lastSensorRead = now;
    SensorReading r = sensors_read();
    currentBT = r.bt;
    currentET = r.et;

    if (sensors_safety_triggered()) {
      Serial.println("SAFETY: sensor fault - shutting off heater");
      heater_emergency_off();
      roastActive = false;
    }
  }

  if (roastActive) {
    unsigned long elapsed = cbGetElapsedSeconds();
    float target = activeProfile.targetAt(elapsed);
    if (!isnan(target) && !isnan(currentBT)) {
      currentHeaterDuty = heaterPID.compute(target, currentBT);
      heater_set_duty(currentHeaterDuty);
    }
  } else if (manualHeaterMode) {
    currentHeaterDuty = manualHeaterDuty;
    heater_set_duty(currentHeaterDuty);
  } else {
    currentHeaterDuty = 0;
    heater_set_duty(0);
  }

  heater_update();

  // AsyncWebServer handles requests in the background, no explicit
  // "server.handleClient()" call needed here like with the sync web server.
}

