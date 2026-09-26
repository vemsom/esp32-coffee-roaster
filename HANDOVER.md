# Handover - ESP32 Coffee Roaster

Repo: https://github.com/vemsom/esp32-coffee-roaster (public)

## What this is

Converting a hot-air popcorn popper into a profile-driven coffee roaster, controlled by an ESP32. Custom web UI (not ESPHome, see "Why not ESPHome" below), REST API, LittleFS profile storage, PID plus time-proportioning SSR control, PWM fan control via a MOSFET module.

## Repo sync status - IMPORTANT

Everything listed here is committed and pushed (verified 2026-09-26, working
tree clean, `origin/main` in sync). The earlier note about web_server/main.cpp/
data/index.html/platformio.ini/firmware-notes.md not being pushed was written
before those files landed - it was already stale when it was read.

Since then (2026-09-26) the firmware gained a latched safety layer (hard
temperature limit + sensor fault, heater held off, see docs/firmware-notes.md),
an MQTT bridge with Home Assistant discovery, and host-side tests for the
safety latch and for the MQTT discovery/command payloads under
tools/host-tests/. That work is committed locally and builds clean.

## Hardware status

Fan motor: confirmed 24V DC. The original popper circuit uses the heating element as a resistive voltage divider plus a 4-diode bridge rectifier and 2 chokes to generate low voltage for the motor. This circuit is NOT galvanically isolated from mains despite the low measured voltage, do not reuse it. Full writeup in docs/hardware.md. Sensors: 2x MAX6675 plus K-type thermocouples purchased, in transit as of last update. Purchased: V-TAC 60W 24V 2.5A LED transformer with screw terminals (about 106 kr) to power the fan motor; IRF520 MOSFET driver module 5-pack (about 86 kr) for PWM fan control from the ESP32. Not yet confirmed: exact GPIO pin assignments (placeholders in include/config.h); whether the IRF520 module needs an added NPN pre-driver stage for reliable 3.3V gate drive, a known weak point of bare IRF520 boards, test on arrival.

## Key technical decisions

SSR control: time-proportioning (on/off within a roughly 2s window), not fast PWM, gentler on a zero-cross SSR. See src/heater_control.cpp. Fan control: PWM via the MOSFET module, about 20kHz (above the audible range). See src/fan_control.cpp. Sensors: MAX6675 x2 (BT/bean temp, ET/environment temp), shared SPI CLK/MISO, separate CS pins. Firmware sanity check flags implausible jumps (over 20C between samples) since MAX6675 has no built-in fault reporting. Profiles: stored as JSON on LittleFS, linear interpolation between time/temp points. Web UI: vanilla JS plus canvas graph, no external CDN dependency, works even without internet on the viewing device. REST API for status, fan/heater control, profile CRUD, roast start/stop.

## Why not ESPHome

ESPHome cannot run this firmware, it is a separate YAML-based system, not a way to flash arbitrary C++ code. Rewriting this project as ESPHome YAML would mean losing or fighting the framework for PID plus profile-following, the custom REST API, and the custom web UI. Build and flash locally with PlatformIO instead, using the command: pio run --target upload

If HA dashboard visibility is wanted later, publish from our own firmware via MQTT (HA has MQTT discovery) rather than rewriting in ESPHome.

## Open items and unverified assumptions

Full list in docs/firmware-notes.md, but the highlights: src/fan_control.cpp uses the arduino-esp32 core 3.x LEDC API (ledcAttach/ledcWrite with a pin, not a channel) - if the build pulls core 2.x this needs rewriting to the old channel-based API. platformio.ini references the ESP32Async/ESPAsyncWebServer and ESP32Async/AsyncTCP fork, verify these package names still resolve in the PlatformIO registry when you build, the older me-no-dev packages do not work on newer cores. MAX6675 NaN-on-fault behavior is assumed, not yet verified against the actual purchased hardware/library version. No auth on the web API, fine on a home network, not meant for exposure beyond that.

## Immediate next steps

First, add WIFI_SSID/WIFI_PASSWORD to include/secrets.h - MQTT credentials are
already in place and verified against the broker, but WiFi is still the "TBD"
fallback, so the device cannot connect to anything. Second, when the MAX6675
modules arrive: wire them up, verify sensor readings against a known-good
thermometer, and check the fault thresholds against real thermocouple noise.
Third, confirm GPIO pin assignments once the physical layout is decided, update
include/config.h. Fourth, test the IRF520 module under load, check for excessive
heat, which would indicate the 3.3V gate drive is not sufficient (add an NPN
pre-driver if so). Fifth, wire the V-TAC PSU and MOSFET module to the fan motor,
physically isolated from the original popper circuit. Sixth, flash and verify:
pio run (green, flash ~69 %), then check that the 8 discovery configs show up in
Home Assistant and that a pulled thermocouple mid-run trips the safety latch.
