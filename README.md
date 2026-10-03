# ESP32 Coffee Roaster

Turn a hot-air popcorn popper into a programmable coffee roaster. An ESP32
reads two temperatures, switches the heater and the fan, and follows a roasting
profile you draw yourself. Everything is controlled from a web page that lives
on the device. Home Assistant can watch the numbers; it can never start or stop
a roast.

The project is open source hardware plus firmware. You buy the parts, wire
them, build the firmware with PlatformIO, flash it once over USB, and update it
over the network from then on.

## What you need

- A hot-air popcorn popper (the heating element and the fan you reuse)
- An ESP32 dev board
- 2x MAX6675 boards with K-type thermocouples (one for the beans, one for the air)
- A solid-state relay (SSR) with heatsink, to switch the heater element
- A MOSFET driver board, to PWM the fan motor
- A separate isolated 24 V DC power supply for the fan motor
- A 5 V supply for the ESP32 (USB is fine)

The stock fan wiring inside the popper is not galvanically isolated from the
mains. This build does not use it: the fan runs from its own 24 V supply, and
the heater runs only through the SSR. See `docs/hardware.md` before you wire
anything, and `docs/wiring.md` for the pin-by-pin connections.

## Build and flash

You need [PlatformIO](https://platformio.org/) (`pio` on your PATH).

```sh
pio run                    # build the firmware
pio run -t buildfs          # build the web UI image (it lives in data/)
pio run --target upload     # first flash, over USB
pio run --target uploadfs   # flash the web UI - without this there is no UI
```

After the first flash, updates go over the network, no cable:

```sh
sh tools/ota-push.sh 192.168.x.x        # firmware - the normal path
sh tools/ota-upload.sh 192.168.x.x fs   # web UI only
```

Both work with no USB cable and no firewall rule. The `fs` form sends the
filesystem image to its own partition; it runs on firmware that has the
filesystem-transfer fix, so it completes instead of dying part-way.

The push script connects to the roaster and POSTs the image to `/api/update`
with the token from `include/secrets.h` (`OTA_TOKEN`) and a SHA-256 of the
image, which the device verifies before it commits anything. Two things worth
knowing:

- The device only accepts the push from **one address**,
  `OTA_ALLOWED_CLIENT_IP` in `include/secrets.h` - run the script from that
  machine. A valid token from anywhere else in the IoT VLAN is refused, because
  IoT devices reach each other on port 80 and the network will not isolate this
  route for you. Everything else (wrong token, wrong checksum, wrong address)
  gets a plain 404, like an unknown path.
- It refuses to flash while the machine is busy - a session running **or the
  element still asking for power** - so a push never interrupts a roast and
  never lands on a live heater.

That direction is deliberate: the ArduinoOTA variant
(`sh tools/ota-upload.sh <ip>`, still in the firmware) needs the *device* to
connect back to your machine, which a network that isolates its IoT VLAN will
not allow unless espota's listener is bound to the right address
(`-I 192.168.1.x`, already in `platformio.ini`). See `docs/firmware-notes.md`.

Replace `192.168.x.x` with the address your roaster got on your network.

**Did the push land?** One request answers it:

```sh
curl -s http://192.168.x.x/api/status   # -> ..."fw":"0.7.0","build":"82888dc","built":"..."
git rev-parse --short HEAD              # on the machine that sent the image
```

If `build` matches that sha, the image you sent is what is running. Same value
in `fw` as Home Assistant shows as `sw_version`, so the two never disagree.

## Configuration

WiFi and MQTT credentials go in `include/secrets.h`, which is gitignored:

```c
#define WIFI_SSID     "..."
#define WIFI_PASSWORD "..."

#define MQTT_HOST     "192.168.x.x"   // MQTT broker, e.g. the Mosquitto add-on
#define MQTT_PORT     1883
#define MQTT_USER     "..."           // required if your broker rejects anonymous connects
#define MQTT_PASSWORD "..."
```

You can skip this file entirely. The firmware still builds and runs: it stays
offline and reports MQTT as disabled in the serial log.

### Language

`FW_LANG_EN` in `include/config.h` picks the language of every user-visible
text: `1` (the default) is English, `0` is Swedish. It decides the Home
Assistant discovery names (`include/strings.h`) and the web page, which reads
the language from `/api/status` and follows it - the browser's own language
never decides, so the page and the entity names always agree. It also works as
a build flag: `PLATFORMIO_BUILD_FLAGS="-DFW_LANG_EN=0" pio run`.

Whichever language you pick, `unique_id`, `object_id` and the topics do not
change, so switching never moves an entity that already exists in Home
Assistant. The host suite proves that by building the discovery test once per
language and diffing the two `unique_id` dumps.

## Use it

1. Power the roaster and wait for it to join your WiFi, then open the device
   address in a browser (any phone or laptop on the same network works).
2. The page shows bean temperature, air temperature, the rate of rise for both,
   heater power, fan power and a live graph. There are three modes:
   - **Profile** - the roaster follows a saved ramp/hold schedule, with a
     fan percentage per step. Profiles are stored as JSON on the device.
   - **Manual** - you set the heater and fan yourself.
   - **Cool** - fan only, on a timer, for after a roast.
3. Press start. The graph draws the actual curve against your setpoint.
4. Stop at any time from the same page. The safety alarm (below) also stops it.

Everything is controlled from the web page. MQTT is **report-only**: the
firmware publishes temperatures, rate of rise, heater and fan, mode, elapsed
time and the alarm states, and it subscribes to nothing. There is no MQTT
command that can start, stop or change a roast.

The web UI has no external dependencies (no CDN), so it works without internet
access on the device that views it.

## Guiding by rate of rise

The profile can drive the roast by rate of rise instead of only by temperature.
Add `rorTarget`, `rorStart` and `rorEnd` to a profile step (values are C/min):

```json
{
  "ramp": 60,
  "hold": 60,
  "temp": 50,
  "fan": 80,
  "rorTarget": 1,
  "rorStart": 15,
  "rorEnd": 3
}
```

`rorTarget > 0` enables the step. The target slope then falls linearly from
`rorStart` at the beginning of the step to `rorEnd` at the end, the same way
temperature ramps. The firmware compares this target to the measured environment
rate of rise (`rorEt`) and nudges the heater duty up or down through a simple
gain. PID still runs underneath; the RoR correction is added on top and clamped
to the safe duty range, with a rate limit so a cold probe or a startup glitch
cannot slam the element to full power.

RoR guidance is **off by default**. A step without `rorTarget`, or any old
profile, behaves exactly as before: temperature target and PID are unchanged.
Guiding also pauses itself when the roast is paused, a safety alarm is active,
the RoR estimate is still warming up, or the fan interlock is holding the
element off. The web UI and MQTT status show the current RoR target, error and
whether guidance is active.

## Safety limits

The heater sits behind a latched alarm that no command can silence. It trips on:

- a hard temperature limit (260 C bean, 300 C air, both in `include/config.h`)
- a sensor fault: NaN, a value outside 2-400 C, or an implausible jump sustained
  over 5 samples
- the two probes disagreeing while both are still cold
- a stuck probe: the same reading bit for bit for 60 s while the element is
  asking for power

While the alarm is latched the relay is held off, a running roast is aborted,
new runs are refused, and the fan keeps running so the beans still get air. It
clears only when the readings are healthy again and the temperature has dropped
10 C below the limit. The latch is stored in flash, so unplugging the machine
does not clear it.

Separately, and without latching, the fan interlock holds the element off unless
the fan runs at least 10 % - in manual and profile mode alike. Details in
`docs/firmware-notes.md`.

Do not skip the wiring notes in `docs/hardware.md`. This is mains-voltage
hardware.

## Tests

`tools/host-tests/run.sh` compiles the firmware logic against a stub Arduino
and runs it on your computer. No ESP32 and no broker needed, only `g++` (plus
`node` for the web UI check, which is skipped when node is absent):

```sh
tools/host-tests/run.sh
```

Four binaries: `test_safety` (the alarm, the interlock and the stuck-probe
detector), `test_ror` (the rate-of-rise maths: window, warm-up, sign and the
quantised tolerance), `test_mqtt_discovery` (discovery payloads and the
report-only guarantees) and `test_control` (the real `src/main.cpp` driven
through `setup()`/`loop()`, including a second thread in the role of the
AsyncTCP task). `test_control` also runs a second time under ThreadSanitizer -
that pass is what found a data race in the profile name.

Two guards sit next to them: `test_mqtt_discovery` is built once per build
language and the two `unique_id` dumps are diffed (identical, or the language
switch would move entities in Home Assistant), and `check_web_i18n.js` checks
the language table in `data/index.html` - every key in both languages, nothing
hard-coded outside it.

## Project status

Early development. The firmware builds clean and the host test suite is green
(RAM 16.0 %, Flash 73.6 %, FW 0.7.0, Arduino core 2.0.17, espressif32 7.1.3).
The hardware is not finished: the temperature modules were still in transit and
the GPIO assignment has never been checked against a physical board.
`docs/firmware-notes.md` lists what is verified and what is still an
assumption; `docs/hardware.md` records the hardware decisions.

## License

Not decided yet (open source - MIT or similar, to be finalized before the first
release).
