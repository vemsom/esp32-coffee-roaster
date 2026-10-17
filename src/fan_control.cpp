#include "fan_control.h"
#include "config.h"
#include <Arduino.h>

// Channel-based LEDC API, compatible with arduino-esp32 core 2.x
// (ledcSetup/ledcAttachPin/ledcWrite with a channel). Core 3.x keeps
// these as a compatibility layer, so this also builds on newer cores.
#define FAN_LEDC_CHANNEL 0

// The two speeds, both in the 0-100 unit the rest of the firmware speaks.
// requestedPct is what the web UI / a profile / the cool timer asked for;
// actualPct is what the fan has reached, and the only one of the two that is
// written to the LEDC channel. Between them sits the ramp: a 24 V fan switched
// straight on at 100 % draws a start current the supply cuts out on, so the
// duty climbs at FAN_SLEW_PCT_PER_SEC instead of jumping.
static float requestedPct = 0;
static float actualPct = 0;
static unsigned long lastStepMs = 0;

// The duty in percent that a reached speed maps to: 1-100 is spread linearly
// over the band the fan actually responds in (FAN_DUTY_MIN_PCT..MAX_PCT), so
// the dead zone below its start threshold is not part of the control range. A
// misconfigured band (max <= min) falls back to the old straight 0-100 map
// instead of inverting or locking the fan. Curve in docs/firmware-notes.md.
static int dutyPctFor(float percent) {
  if (percent <= 0.0f) return 0;          // off is exactly as before
  if (percent > 100.0f) percent = 100.0f;
  if (FAN_DUTY_MAX_PCT > FAN_DUTY_MIN_PCT) {
    return FAN_DUTY_MIN_PCT +
           (int)(percent * (FAN_DUTY_MAX_PCT - FAN_DUTY_MIN_PCT) / 100.0f);
  }
  return (int)percent;                    // broken config: straight 0-100 map
}

static void writeSpeed(float percent) {
  const int maxDuty = (1 << FAN_PWM_RESOLUTION) - 1;
  ledcWrite(FAN_LEDC_CHANNEL, map(dutyPctFor(percent), 0, 100, 0, maxDuty));
}

void fan_init() {
  ledcSetup(FAN_LEDC_CHANNEL, FAN_PWM_FREQ_HZ, FAN_PWM_RESOLUTION);
  ledcAttachPin(PIN_FAN_PWM, FAN_LEDC_CHANNEL);
  requestedPct = 0;
  actualPct = 0;
  lastStepMs = millis();
  writeSpeed(0);
}

// percent is the LOGICAL speed the rest of the firmware talks (web UI, profile
// steps, MQTT/HA, the fan interlock): 0 = off, 1-100 = requested speed. Anything
// that does not ask the fan to go UP is written here and now - spinning down
// draws no start current, and the heat interlock must see a requested stop in
// the same control cycle. An INCREASE only moves the target; fan_update() walks
// the duty up to it.
void fan_set_speed(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  requestedPct = (float)percent;
  if (actualPct >= requestedPct) {
    actualPct = requestedPct;
    writeSpeed(actualPct);
  }
}

// Advances the ramp toward the requested speed. Called from loop() every pass,
// under the same state lock the web callbacks take when they set the target, so
// the two never interleave. With FAN_SLEW_PCT_PER_SEC at 0 (or negative) the
// step is large enough to reach the target in one pass, which is the old
// immediate-write behaviour.
void fan_update() {
  unsigned long now = millis();
  unsigned long dtMs = now - lastStepMs;   // unsigned: wrap-safe
  lastStepMs = now;

  if (actualPct >= requestedPct) return;

  const float rate = (float)FAN_SLEW_PCT_PER_SEC;
  const float step = (rate > 0.0f) ? rate * (dtMs / 1000.0f) : 1000.0f;
  actualPct += step;
  if (actualPct > requestedPct) actualPct = requestedPct;
  writeSpeed(actualPct);
}

float fan_actual_pct() { return actualPct; }
float fan_requested_pct() { return requestedPct; }
