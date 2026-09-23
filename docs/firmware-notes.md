# Firmware notes - assumptions to verify on first build

This code was written before the hardware arrived, so the following are reasonable assumptions that have NOT yet been tested against real hardware. Go through this before trusting the firmware in an actual roast.

## Must verify

Fan PWM LEDC API (src/fan_control.cpp): written against the arduino-esp32 core 3.x API (ledcAttach/ledcWrite with a pin, not a channel). If platformio pulls core 2.x it will not compile - switch to ledcSetup plus ledcAttachPin plus ledcWrite(channel, ...) instead.

ESPAsyncWebServer/AsyncTCP package names in platformio.ini: written against the ESP32Async fork (actively maintained as of 2025/2026). Verify the names still resolve in the PlatformIO registry when you build - the older me-no-dev packages do not work on later core versions.

MAX6675 NaN behavior (src/sensors.cpp): the sanity check assumes the library returns NaN on a broken or disconnected probe. Verify this actually holds for the library version that gets installed, otherwise fault detection could miss real sensor failures.

GPIO pins in include/config.h: placeholders only. Update once you have decided the actual board layout, and avoid the ESP32's strapping pins (0, 2, 12, 15) for critical functions like SSR control.

PID values (PID_KP/KI/KD in config.h): unguessed starting values. Will need tuning against real thermal response once the machine is testable.

## Known gaps, not yet done

No error feedback to the web UI if sensors_safety_triggered() fires mid-roast - the UI just shows the heater turning off, with no clear "SENSOR FAULT" indicator yet. Worth adding.

No authentication on the web API - anyone on the same WiFi network can control the roaster. Fine for hobby use on your own network, not for sharing beyond that.

WiFi connection is blocking in setup() (up to a 15s timeout). Works, but gives no feedback in the UI if it fails, only the serial log.
