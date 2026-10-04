#include "fan_control.h"
#include "config.h"
#include <Arduino.h>

// Channel-based LEDC API, compatible with arduino-esp32 core 2.x
// (ledcSetup/ledcAttachPin/ledcWrite with a channel). Core 3.x keeps
// these as a compatibility layer, so this also builds on newer cores.
#define FAN_LEDC_CHANNEL 0

void fan_init() {
    ledcSetup(FAN_LEDC_CHANNEL, FAN_PWM_FREQ_HZ, FAN_PWM_RESOLUTION);
    ledcAttachPin(PIN_FAN_PWM, FAN_LEDC_CHANNEL);
    fan_set_speed(0);
}

// percent is the LOGICAL speed the rest of the firmware talks (web UI, profile
// steps, MQTT/HA, the fan interlock): 0 = off, 1-100 = requested speed. What
// changes here is only the duty written to the LEDC channel: 1-100 is mapped
// linearly onto the duty band the fan actually responds in, so the dead zone
// below the fan's start threshold is not part of the control range. A
// misconfigured band (max <= min) falls back to the old straight 0-100 map
// instead of inverting or locking the fan. Fan duty curve in
// docs/firmware-notes.md.
void fan_set_speed(int percent) {
    int maxDuty = (1 << FAN_PWM_RESOLUTION) - 1;

    if (percent <= 0) {           // 0 = off, exactly as before
        ledcWrite(FAN_LEDC_CHANNEL, 0);
        return;
    }
    if (percent > 100) percent = 100;

    int dutyPct;
    if (FAN_DUTY_MAX_PCT > FAN_DUTY_MIN_PCT) {
        dutyPct = FAN_DUTY_MIN_PCT +
                  percent * (FAN_DUTY_MAX_PCT - FAN_DUTY_MIN_PCT) / 100;
    } else {
        dutyPct = percent;        // broken config: straight 0-100 map
    }

    ledcWrite(FAN_LEDC_CHANNEL, map(dutyPct, 0, 100, 0, maxDuty));
}
