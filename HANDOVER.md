# Handover - ESP32 Coffee Roaster

Repo: https://github.com/vemsom/esp32-coffee-roaster (public)

## What this is

Converting a hot-air popcorn popper into a profile-driven coffee roaster, controlled by an ESP32. Custom web UI (not ESPHome, see "Why not ESPHome" below), REST API, LittleFS profile storage, PID plus time-proportioning SSR control, PWM fan control via a MOSFET module.

## Repo sync status - IMPORTANT

**Local, not pushed (checked 2026-09-26): `main` is ahead of `origin/main` by
5 commits, working tree clean. Push is handled by github-operator - do not
push from here.** `origin/main` is at `4dc2569 Record the verified build and
test status and triage the open items`, so everything at or before that is
already pushed (the "3 commits ahead" note that used to be here is stale).
The five local ones:

    3031120  Persist the safety latch in NVS so an alarm survives a power cycle
    7c8fd6d  Connect to WiFi without blocking the control loop
    3592e91  Lock the state shared by the AsyncTCP callbacks and loop()
    9707c23  Detect a stuck temperature probe instead of trusting a plausible reading
    plus the commit that carries this file (docs + FW 0.4.0).

Net effect of the unpushed work: four changes Fredrik approved remotely, all
of them behaviour changes - an alarm now survives a power cycle, boot no
longer waits up to 15 s for WiFi (and the UI says when the roaster is
unreachable), the AsyncTCP task and `loop()` are separated by a mutex, and a
frozen probe trips its own alarm after 60 s of identical readings with heat
on. Host tests grew 210 -> 252 checks, the control test now runs a second
thread and a ThreadSanitizer build, and FW_VERSION is 0.4.0.

## Verification status (2026-09-26, remote only - no hardware involved)

Host tests, `tools/host-tests/run.sh`: **252 checks, 0 failures, exit 0, no
compiler warnings.** Split `test_safety` 57, `test_mqtt_discovery` 133,
`test_control` 62 (drives the real `src/main.cpp` through `setup()`/`loop()`,
boots with WiFi down, and runs a second thread in the role of the AsyncTCP
task). `test_control` is additionally built and run under ThreadSanitizer as
part of the same script - 62 more checks, zero race reports - so 314 checks
execute in total.

Clean build, `pio run -t clean && pio run`: **SUCCESS in 45.2 s, 0 warnings.**

    platform espressif32 7.1.3, framework-arduinoespressif32 4.20017.260907
    Arduino core 2.0.17 (ESP_ARDUINO_VERSION 2.0.17)
    ESPAsyncWebServer 3.12.1, AsyncTCP 3.5.0, ArduinoJson 7.4.3,
    PubSubClient 2.8.0, MAX6675 library 1.1.2, toolchain 8.4.0
    RAM:   14.2 %  (46 600 / 327 680 bytes)
    Flash: 69.8 %  (914 669 / 1 310 720 bytes)
    FW_VERSION 0.4.0

## Hardware status

Fan motor: confirmed 24V DC. The original popper circuit uses the heating element as a resistive voltage divider plus a 4-diode bridge rectifier and 2 chokes to generate low voltage for the motor. This circuit is NOT galvanically isolated from mains despite the low measured voltage, do not reuse it. Full writeup in docs/hardware.md. Sensors: 2x MAX6675 plus K-type thermocouples purchased, in transit as of last update. Purchased: V-TAC 60W 24V 2.5A LED transformer with screw terminals (about 106 kr) to power the fan motor; IRF520 MOSFET driver module 5-pack (about 86 kr) for PWM fan control from the ESP32. GPIO assignment is written down (CLK 18, MISO 19, CS-BT 5, CS-ET 17, SSR 26, fan PWM 27 - see docs/wiring.md) but has never been checked against a physical board. Still to confirm on arrival: whether the IRF520 module needs an added NPN pre-driver stage for reliable 3.3V gate drive, a known weak point of bare IRF520 boards.

## Key technical decisions

SSR control: time-proportioning (on/off within a roughly 2s window), not fast PWM, gentler on a zero-cross SSR. See src/heater_control.cpp. Fan control: PWM via the MOSFET module, about 20kHz (above the audible range). See src/fan_control.cpp. Sensors: MAX6675 x2 (BT/bean temp, ET/environment temp), shared SPI CLK/MISO, separate CS pins. Firmware sanity check flags implausible jumps (over 20C between samples) since MAX6675 has no built-in fault reporting, and a probe that freezes on a plausible value trips its own alarm after 60 s of identical readings while the heater is on (src/safety.cpp, thresholds in include/config.h). Concurrency: the AsyncTCP callbacks and `loop()` share main.cpp state under one recursive mutex (include/state_lock.h); `mqtt_update()` is deliberately outside it because its synchronous connect can block. WiFi never blocks boot: `setup()` starts the connection and returns. Profiles: stored as JSON on LittleFS, linear interpolation between time/temp points. Web UI: vanilla JS plus canvas graph, no external CDN dependency, works even without internet on the viewing device. REST API for status, fan/heater control, profile CRUD, roast start/stop.

## Why not ESPHome

ESPHome cannot run this firmware, it is a separate YAML-based system, not a way to flash arbitrary C++ code. Rewriting this project as ESPHome YAML would mean losing or fighting the framework for PID plus profile-following, the custom REST API, and the custom web UI. Build and flash locally with PlatformIO instead, using the command: pio run --target upload

If HA dashboard visibility is wanted later, publish from our own firmware via MQTT (HA has MQTT discovery) rather than rewriting in ESPHome.

## Open items - what is CLOSED (verified 2026-09-26, no hardware needed)

- **LEDC fan API.** The old note here said fan_control.cpp uses the *core 3.x
  pin-based* API and would need rewriting if the build pulled core 2.x. That was
  backwards. The build resolves Arduino **core 2.0.17**, where the channel-based
  API the code uses (`ledcSetup`/`ledcAttachPin`/`ledcWrite`, src/fan_control.cpp:11-21)
  is the *native* one - `cores/esp32/esp32-hal-ledc.h:30,36`. The pin-based
  `ledcAttach(pin)` does not exist in this framework. Nothing to rewrite, and
  the clean build proves it.
- **Package names.** `ESPAsyncWebServer @ 3.12.1` and `AsyncTCP @ 3.5.0`
  (ESP32Async fork) resolve and link. The old me-no-dev packages are not used.
- **Configuration.** include/secrets.h (gitignored, 600) holds WiFi + MQTT
  credentials; every value was matched byte-for-byte against `firmware.elf`.
  No `TBD` left in the built firmware.
- **Report-only MQTT is enforced by tests**, not just by intent: no
  subscriptions, no message callback, no `command_topic`, no controllable
  entity types.
- **MAX6675 NaN behaviour at library level**: `Adafruit MAX6675 1.1.2`,
  max6675.cpp:46 returns NAN when bit 2 of the raw word is set. The library
  gap (a floating-low data line reads a fixed 0 C) is covered by our own range
  and jump checks. The *hardware* behaviour of both cases is still unproven.

Residual risk from the closed LEDC item, decision not code:
`platform = espressif32` in platformio.ini is unpinned. Core 3.x removed the
channel API, so a future `pio platform update` breaks fan_control.cpp at
compile time (loudly). Pin `platform = espressif32@7.1.3`, or rewrite the fan
code to the pin-based API when that upgrade is wanted.

## Open items - what is LEFT, and exactly what it takes

**Needs hardware / physical test:**

1. Confirm the GPIO assignment on a wired board. Measure GPIO5 high at reset
   with both MAX6675 modules powered (strapping pin, CS-BT); fallback is
   CS-BT -> GPIO13 plus one line in include/config.h. See docs/wiring.md.
2. Run the 8-step threshold calibration in docs/firmware-notes.md. Its steps
   2-4 (baseline noise, jump threshold, fault count) cannot be chosen from a
   desk - they are inputs from a real roast curve. Step 2 now also feeds
   `SENSOR_STUCK_MAX_MS` (record the longest run of identical samples).
3. Fault injection on hardware: pull a thermocouple heater OFF (expect NaN ->
   latch), then heater ON (expect heat off within count x 250 ms, run aborted).
   Also confirm the floating-low 0 C case on the real input.
4. Verify sensor readings against a known-good thermometer; confirm 260 C BT /
   300 C ET are the right limits, and freeze the constants in
   include/config.h.
5. PID tuning (PID_KP/KI/KD are unguessed starting values) against the real
   thermal response.
6. IRF520 under load: check for excessive heat = insufficient 3.3V gate drive
   (add NPN pre-driver if so).
7. Wire the V-TAC PSU and MOSFET module to the fan motor, isolated from the
   original popper circuit.
8. Flash + verify: `pio run` (green, flash 69.8 %) plus `pio run -t uploadfs`
   (the web UI lives in the LittleFS image, not the firmware), then check that
   the 9 discovery configs appear in Home Assistant and that MQTT reconnect
   behaves on the flaky home network.
9. MAX6675 modules: still in transit as of last update.

**Done 2026-09-26 (approved by Fredrik remotely, all four host-tested):**

- **NVS persistence.** Every latch transition goes to the `Preferences`
  namespace `safety` and `safety_init()` restores it; `setup()` re-asserts
  `heater_emergency_off()` before the first sensor sample. A corrupt record is
  repaired rather than obeyed, and if NVS will not open the alarm still works
  in RAM only (logged once). Note the nuance: a restored over-temp alarm on a
  *cold* machine still clears after the healthy streak, because the condition
  really is gone - what persistence buys is that the alarm is visible and the
  heater held off from the first instruction after boot.
- **Non-blocking WiFi.** `setup()` starts the connection and returns;
  `serviceWifi()` in `loop()` logs the transitions and re-kicks every 15 s.
  `/api/status` carries `wifiConnected`, and the web UI has a network pill:
  "INGEN KONTAKT n s" when the status poll fails (the roast history stops
  appending until contact returns) and "WIFI SAKNAS" when the device reports
  itself off the network while a page is still being served.
- **Locking.** One recursive mutex in `include/state_lock.h` between the
  AsyncTCP callbacks and `loop()`. `mqtt_update()` is deliberately outside it
  (it can block for seconds on a synchronous connect) - see the known
  limitation below.
- **Stuck-sensor detection.** `SENSOR_STUCK_MAX_MS` 60 s of bit-identical
  readings *while the element is asking for power*, plus the rule that a stuck
  alarm is only released by the probe showing a different value. The window is
  gated on live heat precisely so cooling cannot false-trip; the 60 s number
  still wants confirming against calibration step 2.

**Still needs a code decision from Fredrik (doable without hardware):**

- Auth on the web API (token or basic auth on every handler in
  src/web_server.cpp + a login page). Accepted limitation today: anyone on the
  LAN can control it. **Explicitly deferred until after the first flash** - do
  not start it before that.

**Accepted, not a todo:** no auth on MQTT topics beyond the broker; MQTT stays
report-only (start/stop and fan would be the first candidates if that ever
changes, never a raw heater duty).

## Immediate next steps

Done remotely, no action needed: tests green (252/0, plus a ThreadSanitizer
run), clean build green (RAM 14.2 %, Flash 69.8 %), secrets complete, open
items in docs/firmware-notes.md triaged and tagged with what closes them, and
the four approved changes implemented with their docs.

Next action is physical, in this order: (1) wait for the MAX6675 modules,
(2) confirm the GPIO pinout on the real board and re-check the GPIO5 strapping
question, (3) wire the probes and run the threshold calibration in
docs/firmware-notes.md before any constant is frozen, (4) verify the readings
against a known-good thermometer, (5) test the IRF520 under load,
(6) wire the 24 V PSU + MOSFET to the fan motor, physically isolated from the
original popper circuit, (7) flash with `pio run --target upload` and
`pio run -t uploadfs`, then confirm the 9 discovery entities in Home Assistant
and the safety latch trip with a pulled thermocouple.

Once the build is in front of you: `pio run` for firmware,
`pio run -t buildfs` to preview the LittleFS image,
`tools/host-tests/run.sh` for the host tests (needs only g++).
