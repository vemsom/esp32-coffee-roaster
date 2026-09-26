# ESP32 Coffee Roaster

Open source conversion of a popcorn popper into a profile-driven coffee roaster, built around an ESP32.

## Status
Early development. Firmware builds (`pio run`, PlatformIO env `esp32dev`). See
`docs/firmware-notes.md` for what is verified and what is still an assumption,
and `docs/hardware.md` for the hardware decisions.

## Hardware
- Popcorn popper
- ESP32 dev board
- MAX6675 x2 (BT/ET sensors, K-type thermocouples)
- SSR for the heating element (time-proportioning control)
- Separate isolated DC PSU + MOSFET motor driver for the fan (the original popper circuit is NOT isolated from mains - see docs/hardware.md)

## Features
- Profile roasts from LittleFS JSON profiles (ramp/hold steps with per-step fan), plus a fixed manual mode and a fan-only cool timer
- PID with time-proportioning SSR control (~2 s window) and 20 kHz PWM fan control
- Own REST API + offline-capable web UI in `data/index.html` (no CDN dependencies)
- Home Assistant integration over MQTT with MQTT discovery - temperatures, heater/fan, mode, profile, elapsed time and the safety alarm. **Report-only:** the roaster publishes state and can never be started, stopped or adjusted over MQTT; all control lives in the web UI.

## Safety limits
The heater is behind a latched alarm that cannot be silenced by any command:
it trips on a hard temperature limit (260 C bean, 300 C environment, both in
`include/config.h`) or on a sensor fault (NaN, implausible value, or an
implausible jump sustained over 5 samples). While it is latched the SSR is held
off, a running roast is aborted, new runs are refused, and the fan is left
running so the beans keep getting air. It clears only once the readings are
healthy again and the temperature has dropped 10 C below the limit. Details in
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
pio run                 # build
pio run --target upload # flash (once the hardware is wired)
```

## Tests
`tools/host-tests/run.sh` compiles the safety and heater logic against a stub
Arduino and checks the latch behaviour on the host - no ESP32 needed.

## License
Not decided yet (open source - MIT or similar, to be finalized before first release)
