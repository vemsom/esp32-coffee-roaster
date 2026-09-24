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

void fan_set_speed(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int maxDuty = (1 << FAN_PWM_RESOLUTION) - 1;
    int duty = map(percent, 0, 100, 0, maxDuty);
    ledcWrite(FAN_LEDC_CHANNEL, duty);
}