# Firmware notes - assumptions and what is verified

Status: **verified items below were confirmed on 2026-09-26** with a clean
`pio run` (`pio run -t clean` then `pio run`, 46.7 s, zero warnings) and the
host-side test suite. Everything still listed as open is untested against
real hardware.

Build of record (2026-09-26, clean rebuild):

    platform espressif32 7.1.3, framework-arduinoespressif32 4.20017.260907
    Arduino core 2.0.17 (esp_arduino_version.h: ESP_ARDUINO_VERSION 2.0.17)
    ESPAsyncWebServer @ 3.12.1, AsyncTCP @ 3.5.0, ArduinoJson @ 7.4.3,
    PubSubClient @ 2.8.0, MAX6675 library @ 1.1.2, toolchain 8.4.0
    RAM:   14.2 %  (46 432 / 327 680 bytes)
    Flash: 69.3 %  (908 057 / 1 310 720 bytes)
    [SUCCESS] - no warnings, no errors

## Verified on build / in source

Fan PWM LEDC API (src/fan_control.cpp) - **CLOSED 2026-09-26.** The code uses
the channel-based API (`ledcSetup` + `ledcAttachPin` + `ledcWrite` with
`FAN_LEDC_CHANNEL 0`). The resolved framework is Arduino core **2.0.17**, where
that is the *native* API, not a compatibility layer:
`cores/esp32/esp32-hal-ledc.h:30` declares
`ledcSetup(uint8_t channel, uint32_t freq, uint8_t resolution_bits)` and
`:36` declares `ledcAttachPin(uint8_t pin, uint8_t channel)`. The pin-based
3.x API (`ledcAttach(pin)`) does not exist in this framework at all, so there
was never anything to rewrite - the old HANDOVER note had it backwards.
Compiles clean with `-Wall` equivalent, 0 warnings.

Residual risk (no hardware needed, just a decision): `platform = espressif32`
in platformio.ini is unpinned. Core 3.x removed the channel API, so a future
`pio platform update` would break fan_control.cpp at compile time (loudly, not
silently). Either pin `platform = espressif32@7.1.3` or rewrite fan_control.cpp
to the pin-based API when that upgrade is wanted.

ESPAsyncWebServer/AsyncTCP package names - **CLOSED 2026-09-26.** Both resolve
in the registry and link into the firmware: `ESPAsyncWebServer @ 3.12.1` and
`AsyncTCP @ 3.5.0` (the ESP32Async fork). The me-no-dev packages are not
referenced anywhere.

MAX6675 NaN on an open thermocouple (src/sensors.cpp) - **library source
verified, hardware behaviour still open:** confirmed in the installed library
source - `Adafruit MAX6675 1.1.2`, max6675.cpp:46 returns NAN
when bit 2 of the raw word is set. The same file shows the case that is *not*
covered by the library: a data line that floats low reads as a fixed 0 C rather
than NAN, which is why sensors.cpp also rejects values outside
SENSOR_MIN_VALID_C..SENSOR_MAX_VALID_C and values that jump more than
SENSOR_FAULT_MAX_JUMP_C. The NaN path has never been seen on real hardware -
that is calibration step 5.

The floating-low case is not hypothetical: a bench run on 2026-09-26 with
nothing wired to the ESP32 read 0 C on every channel, ran the heater at 100 %
and never tripped - the lower bound was -10 C, so steady zeros counted as
perfectly healthy readings. `SENSOR_MIN_VALID_C` is now 2.0 (include/config.h)
and there is a BT/ET cross-check on top of it.

Safety latch and heater interlock (src/safety.cpp, src/heater_control.cpp):
exercised by host tests with stubbed Arduino calls - `test_safety` (30 checks),
`test_mqtt_discovery` (133 checks) and `test_control` (47 checks, which drives
the real setup()/loop()). 210 checks, 0 failures, as of the 2026-09-26 run of
`tools/host-tests/run.sh`. See `tools/host-tests/`.

## Safety behaviour (implemented 2026-09-26)

Four conditions latch a single alarm, all checked on every validated sensor
sample in `safety_update(r, heaterActive)`:

1. Hard temperature limit - BT at or above `SAFETY_MAX_TEMP_C` (260 C) or ET at
   or above `SAFETY_MAX_ET_TEMP_C` (300 C). Both constants live in
   include/config.h and are configurable there.
2. Sensor fault - NAN, an implausible value (outside `SENSOR_MIN_VALID_C`
   2 C .. `SENSOR_MAX_VALID_C` 400 C), or an implausible jump, sustained for
   `SENSOR_FAULT_MAX_COUNT` (5) consecutive samples. The 2 C floor is what
   catches a probe that was disconnected before power-up: it reads a steady
   0 C with no previous value to jump from.
3. Probe disagreement - while both probes stay below
   `SENSOR_SPREAD_MAX_COLD_C` (60 C) they must agree within
   `SENSOR_MAX_SPREAD_C` (15 C). This is the case where each reading is
   plausible on its own but one of them is wrong, so there is no per-channel
   window that can see it. Above 60 C the check switches off: a roast really
   does run ET and BT tens of degrees apart.
4. Stuck probe (implemented 2026-09-26) - the same reading, bit for bit, for
   `SENSOR_STUCK_MAX_MS` (60 s) **while the element is asking for power at
   that moment**. A frozen probe is plausible, never jumps and disagrees with
   nobody, so conditions 1-3 cannot see it; the dangerous direction (probe
   stuck low while the PID drives at 100 %) is exactly the one this catches,
   with the ET limit as backstop if ET is the healthy one. The two choices
   that keep it from false-tripping are deliberate and are spelled out in
   include/config.h:
   - *heat right now*, not "heat recently". The tail of a cooling cycle is
     where the reading stalls for minutes (the drift falls below the 0.25 C
     resolution), and by then the element has been off for longer than any
     reasonable context window. With the element off there is no overheat to
     protect against anyway, and that is where the check holds back.
   - *the timer is reset whenever the element is off*, so the window can only
     be spent under actual heat. A probe sitting still in a switched-off
     machine does not start counting the moment someone hits start - it gets
     a full period of heat to prove itself first.
   A stuck alarm is also the one condition that is **not** released by the
   healthy streak: its samples look healthy by definition, so it would
   otherwise clear 2.5 s after tripping (or straight after a reboot, where the
   detector starts out disarmed). It releases only when the frozen channel
   reports a value different from the one it froze on - and that rule travels
   with the alarm through NVS, so a power cycle does not launder it.

While the alarm is latched:

- `heater_emergency_off()` is re-asserted every control cycle, and
  `heater_set_duty()` refuses all writes. The SSR is held LOW.
- The active run (profile or manual) is aborted, but the **fan is deliberately
  left running** so the beans keep getting air.
- New runs are refused: `cbStartRoast()` and `cbStartManual()` return false.
  Fan-only cooling is still allowed.
- The latch cannot be silenced. There is no command, web endpoint or MQTT
  message that clears it. It clears only after `SAFETY_CLEAR_STREAK` (10)
  consecutive samples that are fault-free *and* below the limit minus
  `SAFETY_CLEAR_MARGIN_C` (10 C). After it clears the heater is re-armed but the
  run does not resume - it has to be started again.

The alarm is exposed as `safetyFault` + `safetyReason` in `/api/status` (shown
as a red banner in the web UI) and as the `binary_sensor` "Sakerhetslarm" in
Home Assistant.

**Persistence (implemented 2026-09-26).** Every latch transition (trip *and*
clear) is written to NVS, namespace `safety`, keys `latched` + `code`, and
`safety_init()` reads it back on boot. A power cycle in the middle of an alarm
therefore boots with the heater already held off (`heater_emergency_off()` is
called from `setup()`, before the first sensor sample, not 250 ms after it) and
with the same reason reported by the UI and MQTT. Details that matter:

- Writes happen on transitions only, never per sample - flash endurance is not
  a 4 Hz resource.
- The reason code is written before the flag, so a reset between the two
  writes can never restore "latched" with a stale or unknown reason. An
  out-of-range stored code is ignored, logged and repaired.
- A restored latch is the *same alarm*, so it follows the normal clear rule:
  healthy readings for `SAFETY_CLEAR_STREAK` samples and it releases. The
  exceptions are the fault types whose evidence is gone simply because the
  machine rebooted (see the stuck-probe item below).
- If NVS will not open (`Preferences::begin` fails) the alarm still works, it
  just lives in RAM only - logged once at boot, degraded and never silent.
- What persistence does *not* buy: an over-temp alarm restored on a cold
  machine clears after the healthy streak, because the condition really is
  measurably gone. The point is that the alarm is visible and the heater is
  held off from the first instruction after boot.

Open items in the safety layer (tagged with what it actually takes to close
them - the two that needed no hardware are marked CLOSED 2026-09-26, the rest
still cannot be closed from a desk):

- **REQUIRES HARDWARE/ROAST DATA** The fault thresholds
  (`SENSOR_FAULT_MAX_JUMP_C` 20 C, 5 consecutive samples) were chosen on paper.
  They need to be checked against real thermocouple noise during a roast - too
  tight and a noisy reading aborts a roast, too loose and a dropped probe is
  noticed late. Input: calibration steps 2-4 below.
- **CLOSED 2026-09-26 (approved by Fredrik, host-tested)** Stuck-sensor
  detection exists: `SENSOR_STUCK_MAX_MS` (60 s of bit-identical readings
  while the element is asking for power), conditions 4 above. The window was
  chosen so that neither roasting nor cooling can false-trip - see the two
  rules in include/config.h. Residual: the 60 s number still wants a
  confirmation against the real noise figures from calibration step 2 (record
  the longest run of *identical* consecutive samples there; if a healthy
  static reading ever holds still that long with heat on, raise the constant).
  That is a calibration input, not a missing feature.
- **CLOSED 2026-09-26 (approved by Fredrik, host-tested)** The alarm state is
  persisted to NVS and re-asserted in `setup()` - see "Persistence" above. The
  behaviour change is deliberate and was signed off: a power cycle no longer
  clears an alarm. Degradation path if NVS is unavailable: RAM-only latch, one
  serial log line.
- **REQUIRES HARDWARE** 260 C is a guess for this popper and these probes.
  Confirm against the hardware before the first real roast (step 7 below), and
  keep in mind that a K-type thermocouple in a hot air stream reads air, not
  bean temperature.

## Fan interlock (implemented 2026-09-26)

The element may only fire while the fan runs at least `FAN_MIN_FOR_HEATER_PCT`
(10 %, include/config.h), in manual and profile mode alike:

- Every heater command goes through `applyHeaterDuty()` in src/main.cpp, which
  holds the duty at 0 and raises `fanInterlockFault` when the fan is below the
  threshold. The flag is recomputed every control cycle, so it clears itself as
  soon as the fan is back (or nothing is requested) - unlike the safety latch,
  this is deliberately not one-way.
- `handleManualStart` refuses a start below the fan minimum with its own 409
  ("cannot start: fan must run at least 10 % first"), separate from the safety
  409. The control-level check behind it is the backstop.
- Reported as `fanFault` in `/api/status`, as an amber banner in the web UI
  ("FLÄKT <10 % - värmen av") and as the `binary_sensor` "Flaktsparr" in HA.
  Kept out of `safetyFault` on purpose: different condition, different fix, and
  it clears on its own.

In profile mode the fan is applied *before* the duty is computed, otherwise the
first sample of a run would be judged against the previous run's fan value.

## Web UI monitor

The manual view carries the same readout as the roast view: cards for BT, ET,
setpoint, heater duty and fan duty, plus a graph with the setpoint drawn as a
flat dashed line. Measured fan duty is drawn as a line in both views. The
manual view also has a fan input (`POST /api/fan`) - that is how the fan is
raised before a start when the interlock is in the way.

## WiFi (changed 2026-09-26)

`setup()` no longer waits for an association. It configures STA mode, turns on
the SDK's auto-reconnect, fires one `WiFi.begin()` and returns - the control
loop starts with or without a network. `serviceWifi()` runs from `loop()` on
every pass and does three things, none of them blocking:

- logs the transition when the link comes up (with the IP) or goes down;
- re-kicks `WiFi.begin()` every `WIFI_RETRY_INTERVAL_MS` (15 s,
  include/config.h) while the link is down, for the cases auto-reconnect does
  not recover from on its own;
- exposes the state as `wifiConnected` in `/api/status`.

The web UI shows it as a new pill next to the alarm pills, with two distinct
meanings that look the same from the browser: **"INGEN KONTAKT n s"** when the
status poll itself fails (the roaster's WiFi is down, the AP is gone, or the
viewing device lost the network - it also stops appending to the roast history
until contact returns), and **"WIFI SAKNAS"** when the device reports
`wifiConnected:false` while a page is still being served. The second case is
rare on purpose: if the roaster's WiFi is down, normally nothing can be served
at all.

## Concurrency / state lock (implemented 2026-09-26)

Two tasks touch the same state in src/main.cpp: `loop()` runs the sensor
sample, the safety latch, the PID and the mode state machine on the Arduino
loop task, and every web callback runs on the AsyncTCP task because
ESPAsyncWebServer dispatches its handlers there. There was nothing between
them - a clipped value or a refused start at worst, a torn read of the profile
name at best.

`include/state_lock.h` is now that something:

- **One recursive mutex for the whole state.** Recursive because a command
  callback legitimately calls other command callbacks (`cbStopManual` ->
  `maybeStartAutoCool` -> `cbStartCool`) and because the loop task's tick
  calls the same helpers. On the ESP32 it is a FreeRTOS recursive semaphore;
  on the host it is `std::recursive_mutex`. The branch is chosen with
  `__has_include(<freertos/FreeRTOS.h>)`, not a platform macro, because
  main.cpp is compiled by both PlatformIO and the host test suite.
- **Where it is taken:** every status getter and every command callback in
  main.cpp; the whole sensor-tick block in `loop()` (the SPI read itself is
  deliberately outside, it only touches loop-task state); `heater_update()`;
  and the two status-snapshot blocks - `mqtt_publish_status()` and
  `handleStatus()` in web_server.cpp, so one payload cannot straddle a
  change. The lock is never held across a network publish or a response send.
- **Lock order:** state lock, then NVS or LittleFS if a callback needs them.
  Nothing that takes NVS or the filesystem ever comes back for the state lock,
  so there is no cycle and no deadlock.
- **`mqtt_update()` is deliberately outside the lock.** PubSubClient connects
  synchronously and can hold `loop()` for seconds with the broker down;
  freezing every HTTP request for that long would be worse than a status
  payload that straddles one change (it reads through the locked getters
  anyway).
- **The profile name is a fixed `char[64]`, not a `String`.**
  `cbGetProfileName()` hands that pointer to the JSON builder, which runs
  outside the lock - a `String` could be re-allocated underneath it. The
  pointer can still tear, which is why the payload blocks above hold the lock
  across the copy.

Two host tests back this up: an acquisition counter (a getter or command that
stops taking the lock fails the test, which is otherwise invisible to a
single-threaded run), and a stress test where a second thread plays the
AsyncTCP task against `loop()`. The whole control test is *also* built under
ThreadSanitizer - that is what found the profile-name race in the first place,
and it now runs clean.

## MQTT / Home Assistant (implemented 2026-09-26)

Broker for this build: the MQTT broker on the Home Assistant host,
192.168.1.173:1883 - reachable, and authentication is required (an anonymous
CONNECT gets CONNACK rc=5 "not authorized"). The firmware therefore refuses to
enable MQTT when `MQTT_USER` is empty instead of retrying a connection that can
never succeed. Credentials are not in the repo: they go in include/secrets.h,
which is gitignored. `MQTT_PASS` is accepted as an alias for `MQTT_PASSWORD` in
case secrets.h is written by other tooling.

**Report-only by design.** The roaster publishes state and nothing else: it
subscribes to no topic, registers no message callback, and publishes no entity
with a `command_topic`. There is no `number`, `select`, `button` or `switch` in
HA and no `fan/set`, `heater/set`, `profile/set`, `roast/start` or
`roast/stop` topic - MQTT is for reporting, never for control. All control
lives in the web UI (and in the safety layer).

`src/mqtt_client.cpp` publishes Home Assistant MQTT discovery payloads
(`homeassistant/<component>/coffee_roaster_<entity>/config`, retained) and a
status JSON every 2 s. Broker connection details come from include/secrets.h
(gitignored); include/config.h carries `TBD` placeholders, and while
`MQTT_HOST` is "TBD" or `MQTT_USER` is empty MQTT is disabled with a serial log
line - the rest of the firmware runs normally.

Topics, base `coffee_roaster` (nothing else is published, nothing at all is
subscribed):

    coffee_roaster/status          JSON, published every 2 s
    coffee_roaster/availability    "online" / "offline" (LWT, retained)

Entities published in discovery (9 configs, all read-only, all under
`homeassistant/<component>/coffee_roaster_<object>/config`): bean temperature,
environment temperature, heater duty, fan speed, mode, elapsed seconds and
profile (sensors), plus the safety alarm and the fan interlock (binary_sensors,
both device_class problem). Every config is retained and carries the device
block, unique_id and availability topic.

Topic separation: everything the roaster owns is under `coffee_roaster/` plus
its own `homeassistant/` configs, so nothing overlaps Tibber Pulse MQTT's topics
on the same broker.

Verified against the real broker (2026-09-26, read-only probe from this
machine, nothing published): CONNACK Success with the credentials in
include/secrets.h. A 15 s listen on `#` saw only `zigbee2mqtt/*` (8 retained
bridge topics) and one `tibber` message - zero `coffee_roaster/*` and zero
`homeassistant/*`. Nothing from the roaster exists in HA yet: the firmware has
never been flashed and the hardware is not assembled (MAX6675 still in
transit). Discovery and the status stream can only be verified once the ESP32
is on the air.

Configuration is complete as of 2026-09-26: include/secrets.h (600, gitignored)
carries WIFI_SSID, WIFI_PASSWORD, MQTT_HOST, MQTT_PORT, MQTT_USER and
MQTT_PASSWORD. Verified present in the built firmware by matching every value
byte-for-byte against `firmware.elf` (no config value is left as "TBD").

Open items in the MQTT layer:

- **CLOSED (decision recorded)** No control over MQTT - deliberate, see above.
  The host test asserts it: no subscriptions, no message callback, no
  `command_topic`, no controllable entity types. If that ever changes the first
  candidate is a start/stop pair and the fan, never a raw heater duty.
- **CLOSED (decision recorded)** HA only shows state: manual mode, profile
  editing and start/stop stay in the web UI and cannot be reached from MQTT.
- **REQUIRES HARDWARE** Configuration is done, hardware is not.
  WIFI_SSID/WIFI_PASSWORD and the four MQTT macros are in include/secrets.h
  (600, gitignored), confirmed present in `firmware.elf`, and the broker accepts
  them (CONNACK Success). What is left is physical: the ESP32 has never been
  flashed, so nothing has ever been published and no discovery config has ever
  appeared in HA. Pinout in include/config.h is assigned but not confirmed
  against a wired board.
- **REQUIRES RUNTIME** MQTT reconnects every 5 s while the link is down. The
  home network has intermittent dropouts, so this is expected to be exercised
  in practice - it needs the device on the network, not a host test.

## Calibrating the safety thresholds on real hardware

Do this once the MAX6675 modules have arrived and the pinout is confirmed. In
order - each step's output is an input to the next, and no constant gets frozen
before step 8.

1. **Pinout first.** Put the real GPIO numbers in include/config.h and keep the
   SSR off the strapping pins (0, 2, 12, 15). Everything below is meaningless
   while the code and the wiring disagree.
2. **Baseline noise, heater OFF.** Both probes in the chamber at ambient,
   logging raw MAX6675 samples over serial at the 250 ms cadence for at least
   5 minutes (~1200 samples per channel). Record peak-to-peak, standard
   deviation and the largest |delta| between consecutive samples per channel.
   Also record the longest run of *bit-identical* consecutive samples - that
   is the number that confirms or moves `SENSOR_STUCK_MAX_MS` (60 s), which
   only ever looks at readings taken while the element is on.
3. **Set SENSOR_FAULT_MAX_JUMP_C from that.** It has to be several times the
   largest |delta| seen in a *static* reading, and still above the largest
   |delta| seen during a real ramp. The current 20 C per 250 ms is 80 C/s,
   which this popper cannot reach - but prove it with an actual roast curve
   instead of assuming it.
4. **Set SENSOR_FAULT_MAX_COUNT from the glitch rate.** count x 250 ms is how
   long a fault may persist before the heater is cut: short enough to be safe
   (a couple of seconds at most), long enough that one spike never aborts a
   roast.
5. **Fault injection, heater OFF.** Pull one thermocouple while logging.
   Confirm the library really returns NaN on an open probe (assumed from
   max6675.cpp:46, never seen on hardware) and that the latch trips and holds.
   Then confirm the second failure mode on hardware as well: a data line that
   floats low reads a fixed 0.0 C rather than NaN. Today that case is covered
   by test_control with an *injected* 0 C reading - the real floating input
   still has to be seen once.
6. **Fault injection, heater ON, chamber cold.** Start a manual run, then pull
   the probe. Expect: heater off within count x 250 ms, run aborted, alarm in
   the web UI (and HA once flashed), no re-arm until the probe is back and
   readings have been healthy for SAFETY_CLEAR_STREAK samples.
7. **Check the limits themselves.** Run a real roast, record peak BT and ET.
   Confirm 260 C BT / 300 C ET sit above normal operation with margin, and that
   the probe placement (air stream vs beans) makes those numbers meaningful.
8. **Freeze the constants** and write the measured values back into this file.

Resolved since the bench run, but decide the number here: a probe disconnected
*at power-on* used to read 0.0 C straight through the -10..400 C window with no
previous value to jump from, so the PID drove the heater at 100 % on a
fabricated reading and the alarm never fired. Two fixes are in:
`SENSOR_MIN_VALID_C` 2 C (a steady 0 C becomes a sensor fault after
`SENSOR_FAULT_MAX_COUNT` samples) and the BT/ET cross-check. What is still a
judgement call is whether 2 C is the right floor - if this machine ever stands
somewhere that cold, move the floor using the numbers from step 2, and confirm
in step 6 that a genuinely floating input lands below it.

## Still open, not done

Tagged the same way as above: what it takes, not just what is left.

- **ASSIGNED, REQUIRES HARDWARE CONFIRMATION** GPIO pins in include/config.h.
  These are no longer TBD placeholders - the current assignment is CLK 18,
  MISO 19, CS-BT 5, CS-ET 17, SSR 26, fan PWM 27, written down and cross-checked
  against the code in docs/wiring.md (2026-09-26). What has *not* happened is
  the physical check: the board does not exist yet. Re-check on wiring that
  nothing sits on a strapping pin (0, 2, 12, 15), and specifically measure
  GPIO5 high at reset with both MAX6675 modules powered - wiring.md has the
  fallback (move CS-BT to GPIO13) if it is not.
- **REQUIRES HARDWARE** PID values (PID_KP/KI/KD in config.h): unguessed
  starting values. Will need tuning against the real thermal response once the
  machine is testable.
- **KNOWN LIMITATION, accepted on purpose** No authentication on the web API -
  anyone on the same WiFi network can control the roaster. Fine for hobby use on
  your own network, not for sharing beyond that. The same applies to the MQTT
  topics: broker-level auth is the only protection. Closing it means a token or
  basic-auth check on every handler in src/web_server.cpp plus a login page in
  data/index.html - doable without hardware, deliberately not done.
- **CLOSED 2026-09-26 (approved by Fredrik, host-tested)** WiFi connection is
  non-blocking: `setup()` starts it and returns, `serviceWifi()` in `loop()`
  reports and re-kicks it, and the web UI has a network pill (see the WiFi
  section above). The host test boots with the link down and asserts that
  `setup()` returns promptly, that the control loop still trips the safety
  latch, and that the retry fires after `WIFI_RETRY_INTERVAL_MS`.
- **KNOWN LIMITATION, not part of this change** MQTT reconnect can still block
  `loop()` while the broker is unreachable: `PubSubClient::connect()` does a
  synchronous TCP connect, and the Arduino core's default connect timeout is
  3000 ms (`libraries/WiFi/src/WiFiClient.cpp:26`). Paced by
  `MQTT_RECONNECT_INTERVAL_MS` (5 s), so the worst case is roughly a 3 s stall
  every 5 s while WiFi is up but the broker is not - longer than the 2 s
  heater window, which means one skewed proportioning window per stall.
  Closing it needs a non-blocking connect (raw `WiFiClient` + state machine)
  or an MQTT library with an async connect. Separate decision, not done.
- **CLOSED 2026-09-26 (approved by Fredrik, host-tested + ThreadSanitizer)**
  Concurrency: ESPAsyncWebServer callbacks and `loop()` are separated by one
  recursive mutex, see the Concurrency section above. The residual note is
  about MQTT: `mqtt_update()` stays outside the lock on purpose because it can
  block for seconds - see the known limitation above for what that costs.

## Host-side tests

`tools/host-tests/run.sh` compiles the firmware logic against stubbed Arduino/
WiFi/PubSubClient headers and runs it on the host - no ESP32 and no broker.
Last run 2026-09-26: **210 checks, 0 failures**, exit 0, no compiler warnings:

- `test_safety` - safety latch and heater interlock (30 checks): trip on the
  hard limit, on sustained sensor faults and on BT/ET disagreement while cold;
  commands refused while latched; clear only after the healthy streak.
- `test_mqtt_discovery` - MQTT layer (133 checks): all 9 discovery configs are
  valid JSON with unique_id, device block and availability; the status payload
  carries the expected fields (including `fanFault`); every payload fits the
  PubSubClient buffer (largest 647 B against the 900 B limit); and the
  report-only guarantees hold - no subscriptions, no message callback, no
  `command_topic` on any entity, no controllable entity types, and every
  published topic under `coffee_roaster/` or `homeassistant/`.
- `test_control` - the real src/main.cpp against stubbed hardware (47 checks):
  the bench case (probes disconnected, 0 C on every channel) trips the latch
  and denies manual start; `heater_set_duty(100)` cannot get past a held alarm;
  the fan interlock in manual *and* profile mode, both directions; a probe
  pulled mid-run aborts the running heat; and the MQTT status payload carries
  both fault flags. This is the test that would have caught the 0 C bug.

The MQTT test needs an enabled config: without `include/secrets.h`, `run.sh`
passes throwaway `-DMQTT_HOST/-DMQTT_USER/-DMQTT_PASSWORD` values to both
translation units (macros do not cross translation units). The PubSubClient stub
refuses anonymous connects, so the credential path is exercised too.
