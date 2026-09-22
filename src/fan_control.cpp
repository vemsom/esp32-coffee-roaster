#include "fan_control.h"
#include "config.h"
#include <Arduino.h>

// Uses the LEDC API from arduino-esp32 core 3.x (ledcAttach/ledcWrite
// with a pin argument, not a channel). If the build ends up on core 2.x,
// switch to ledcSetup(channel, freq, res) + ledcAttachPin(pin, channel) +
// ledcWrite(channel, duty). Verify on first build.

void fan_init() {
    ledcAttach(PIN_FAN_PWM, FAN_PWM_FREQ_HZ, FAN_PWM_RESOLUTION);
  fan_set_speed(0);
}

void fan_set_speed(int percent) {
    if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  int maxDuty = (1 << FAN_PWM_RESOLUTION) - 1;
  int duty = map(percent, 0, 100, 0, maxDuty);
  ledcWrite(PIN_FAN_PWM, duty);
}
