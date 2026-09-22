#include "heater_control.h"
#include "config.h"
#include <Arduino.h>

static float currentDuty = 0;
static unsigned long windowStart = 0;
static bool emergencyStop = false;

void heater_init() {
    pinMode(PIN_SSR_HEATER, OUTPUT);
  digitalWrite(PIN_SSR_HEATER, LOW);
  windowStart = millis();
}

void heater_set_duty(float dutyPercent) {
    if (emergencyStop) return;
  if (dutyPercent < 0) dutyPercent = 0;
  if (dutyPercent > 100) dutyPercent = 100;
  currentDuty = dutyPercent;
}

void heater_update() {
    if (emergencyStop) {
    digitalWrite(PIN_SSR_HEATER, LOW);
    return;
    }

  unsigned long now = millis();
  unsigned long elapsed = now - windowStart;

  if (elapsed >= HEATER_WINDOW_MS) {
    windowStart = now;
    elapsed = 0;
  }

  unsigned long onTime = (unsigned long)(HEATER_WINDOW_MS * (currentDuty / 100.0));
  digitalWrite(PIN_SSR_HEATER, elapsed < onTime ? HIGH : LOW);
}

void heater_emergency_off() {
    emergencyStop = true;
  currentDuty = 0;
  digitalWrite(PIN_SSR_HEATER, LOW);
}
