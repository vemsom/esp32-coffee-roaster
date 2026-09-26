# ESP32 Coffee Roaster

Open source conversion of a popcorn popper into a profile-driven coffee roaster, built around an ESP32.

## Status
Early development. Firmware builds clean from scratch (`pio run -t clean &&
pio run`, PlatformIO env `esp32dev`, no warnings): **RAM 14.2 % (46 600 B),
Flash 69.8 % (914 669 B)** on Arduino core 2.0.17, platform espressif32 7.1.3,
FW 0.4.0. The host test suite is green: **252 checks, 0 failures** (plus the
same control test rebuilt under ThreadSanitizer). Nothing has been
flashed yet - the hardware is not assembled. See `docs/firmware-notes.md` for
what is verified and what is still an assumption, and `docs/hardware.md` for
the hardware decisions.

## Hardware
- Popcorn popper
- ESP32 dev board
- MAX6675 x2 (BT/ET sensors, K-type thermocouples)
- SSR for the heating element (time-proportioning control)
- Separate isolated DC PSU + MOSFET motor driver for the fan (the original popper circuit is NOT isolated from mains - see docs/hardware.md)

## Features
- Profile roasts from LittleFS JSON profiles (ramp/hold steps with per-step fan), plus a fixed manual mode and a fan-only cool timer
- PID with time-proportioning SSR control (~2 s window) and 20 kHz PWM fan control
- Own REST API + offline-capable web UI in `data/index.html` (no CDN dependencies), with a network pill that says when the roaster is unreachable ("INGEN KONTAKT n s") or off the network ("WIFI SAKNAS")
- WiFi connects in the background: `setup()` starts it and returns, so the control loop is running with or without a network
- Home Assistant integration over MQTT with MQTT discovery - temperatures, heater/fan, mode, profile, elapsed time, the safety alarm and the fan interlock. **Report-only:** the roaster publishes state and can never be started, stopped or adjusted over MQTT; all control lives in the web UI.

## Safety limits
The heater is behind a latched alarm that cannot be silenced by any command:
it trips on a hard temperature limit (260 C bean, 300 C environment, both in
`include/config.h`), on a sensor fault (NaN, a value outside 2-400 C, or an
implausible jump sustained over 5 samples), on the two probes disagreeing
while both are still cold, or on a stuck probe (the same reading bit for bit
for 60 s while the element is asking for power - a frozen probe is plausible
enough to fool every other check, and the heat condition is what keeps the
cooling tail from false-tripping). A probe that is disconnected at power-up
reads a steady 0 C rather than NaN - that is what the 2 C floor is for. While
the alarm is latched the SSR is held off, a running roast is aborted, new runs
are refused, and the fan is left running so the beans keep getting air. It
clears only once the readings are healthy again and the temperature has
dropped 10 C below the limit; a stuck alarm additionally waits for the probe
to show a different value.

The latch is persisted to NVS, so a power cycle during an alarm boots with the
heater already held off and the same reason reported - it does not clear
because the machine was unplugged.

Separately, and without latching, the fan interlock holds the element off
unless the fan runs at least 10 % - in manual and profile mode alike. It has
its own message in the web UI and its own `binary_sensor` in Home Assistant,
and it clears itself as soon as the fan is back. Details in
`docs/firmware-notes.md`.

## Configuration
WiFi and MQTT credentials go in `include/secrets.h`, which is gitignored:

```c
#define WIFI_SSID     "..."
#define WIFI_PASSWORD "..."

#define MQTT_HOST     "192.168.x.x"   // MQTT broker, e.g. the Mosquitto add-on
#define MQTT_PORT     1883
#define MQTT_USER     "..."           // required: this broker rejects anonymous connects
#define MQTT_PASSWORD "..."
```

Without `secrets.h` the firmware still builds and runs: WiFi stays offline and
MQTT is reported as disabled in the serial log.

## Build
```sh
pio run                    # firmware
pio run -t buildfs         # LittleFS image from data/ (the web UI lives here)
pio run --target upload    # flash the firmware (once the hardware is wired)
pio run --target uploadfs  # flash the filesystem - without this the web UI is missing
```

## Tests
`tools/host-tests/run.sh` compiles the firmware logic against a stub Arduino
and runs it on the host - no ESP32 and no broker needed. Three binaries:
`test_safety` (57 checks, latch + interlock + NVS persistence + stuck probe),
`test_mqtt_discovery` (133 checks, discovery payloads and the report-only
guarantees) and `test_control` (62 checks, the real `src/main.cpp` driven
through `setup()`/`loop()`, including a second thread in the role of the
AsyncTCP task). `test_control` is also built a second time under
ThreadSanitizer and run as part of the suite - that is what caught the
profile-name race. 252 checks, 0 failures as of 2026-09-26.

## License
Not decided yet (open source - MIT or similar, to be finalized before first release)
