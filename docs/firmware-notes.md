# Firmware notes - assumptions and what is verified

Status: **verified items below were confirmed on 2026-09-26** with `pio run`
(framework-arduinoespressif32 4.20017.260907, ESP32 core 3.x) and a host-side
test of the safety logic. Everything still listed as open is untested against
real hardware.

## Verified on build / in source

Fan PWM LEDC API (src/fan_control.cpp): uses the channel-based API
(ledcSetup + ledcAttachPin + ledcWrite), which the ESP32 core 3.x framework in
this project still provides as a compatibility layer. Compiles clean.

ESPAsyncWebServer/AsyncTCP package names (platformio.ini): resolved and built as
`ESPAsyncWebServer @ 3.12.1` and `AsyncTCP @ 3.5.0` (the ESP32Async fork).

MAX6675 NaN on an open thermocouple (src/sensors.cpp): confirmed in the
installed library source - `Adafruit MAX6675 1.1.2`, max6675.cpp:46 returns NAN
when bit 2 of the raw word is set. The same file shows the case that is *not*
covered by the library: a data line that floats low reads as a fixed 0 C rather
than NAN, which is why sensors.cpp also rejects values outside
SENSOR_MIN_VALID_C..SENSOR_MAX_VALID_C and values that jump more than
SENSOR_FAULT_MAX_JUMP_C.

Safety latch and heater interlock (src/safety.cpp, src/heater_control.cpp):
exercised by a host test with stubbed Arduino calls, 25 assertions, all passing.
See `tools/host-tests/`.

## Safety behaviour (implemented 2026-09-26)

Two conditions latch a single alarm, both checked on every validated sensor
sample in `safety_update()`:

1. Hard temperature limit - BT at or above `SAFETY_MAX_TEMP_C` (260 C) or ET at
   or above `SAFETY_MAX_ET_TEMP_C` (300 C). Both constants live in
   include/config.h and are configurable there.
2. Sensor fault - NAN, implausible value, or an implausible jump, sustained for
   `SENSOR_FAULT_MAX_COUNT` (5) consecutive samples.

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

Open items in the safety layer:

- The fault thresholds (`SENSOR_FAULT_MAX_JUMP_C` 20 C, 5 consecutive samples)
  were chosen on paper. They need to be checked against real thermocouple noise
  during a roast - too tight and a noisy reading aborts a roast, too loose and a
  dropped probe is noticed late.
- No stuck-sensor detection: a probe that freezes on a plausible value that
  still jitters by less than 20 C per sample is not detected. The hard limit
  catches the dangerous outcome.
- The alarm state is in RAM only; a reset clears it. Safe (heater off on boot)
  but it means an alarm is not visible after a power cycle.
- 260 C is a guess for this popper and these probes. Confirm against the
  hardware before the first real roast, and keep in mind that a K-type
  thermocouple in a hot air stream reads air, not bean temperature.

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

Entities published in discovery (8 configs, all read-only, all under
`homeassistant/<component>/coffee_roaster_<object>/config`): bean temperature,
environment temperature, heater duty, fan speed, mode, elapsed seconds and
profile (sensors), plus the safety alarm (binary_sensor, device_class problem).
Every config is retained and carries the device block, unique_id and
availability topic.

Topic separation: everything the roaster owns is under `coffee_roaster/` plus
its own `homeassistant/` configs, so nothing overlaps Tibber Pulse MQTT's topics
on the same broker.

Open items in the MQTT layer:

- **Broker host, port and credentials are not filled in.** They go in
  include/secrets.h (MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASSWORD or
  MQTT_PASS). Nothing MQTT-related has been run against a real broker yet, and
  this broker rejects anonymous connects, so without them MQTT stays disabled.
- No control over MQTT - deliberate, see above. If that ever changes the first
  candidate is a start/stop pair and the fan, never a raw heater duty.
- The fan can be commanded to 0 % while the heater is on (web UI only). With no
  airflow the element heats the chamber quickly; the 260 C latch is the
  backstop. A "fan required when the heater is on" interlock would be the next
  safety improvement.
- MQTT reconnects every 5 s while the link is down. The home network has
  intermittent dropouts, so this is expected to be exercised in practice.
- HA only shows state: manual mode, profile editing and start/stop stay in the
  web UI and cannot be reached from MQTT.

## Still open, not done

GPIO pins in include/config.h: placeholders only. Update once the actual board
layout is decided, and avoid the ESP32 strapping pins (0, 2, 12, 15) for
critical functions like SSR control.

PID values (PID_KP/KI/KD in config.h): unguessed starting values. Will need
tuning against the real thermal response once the machine is testable.

No authentication on the web API - anyone on the same WiFi network can control
the roaster. Fine for hobby use on your own network, not for sharing beyond
that. The same applies to the MQTT topics: broker-level auth is the only
protection.

WiFi connection is blocking in setup() (up to a 15 s timeout). Works, but gives
no feedback in the UI if it fails, only the serial log.

Concurrency note: ESPAsyncWebServer callbacks run in the AsyncTCP task while
MQTT callbacks run from `loop()`. Both mutate the same state in main.cpp without
a lock. Pre-existing, unchanged here, and the consequences are limited to a
clipped value or a refused start - but worth a proper fix if the UI ever gets
multi-user.

## Host-side tests

`tools/host-tests/run.sh` compiles the firmware logic against stubbed Arduino/
WiFi/PubSubClient headers and runs it on the host - no ESP32 and no broker:

- `test_safety` - safety latch and heater interlock (25 checks): trip on the
  hard limit and on sustained sensor faults, commands refused while latched,
  clear only after the healthy streak.
- `test_mqtt_discovery` - MQTT layer (119 checks): all 8 discovery configs are
  valid JSON with unique_id, device block and availability; the status payload
  carries the expected fields; every payload fits the PubSubClient buffer
  (largest 647 B against the 900 B limit); and the report-only guarantees hold -
  no subscriptions, no message callback, no `command_topic` on any entity, no
  controllable entity types, and every published topic under `coffee_roaster/`
  or `homeassistant/`.

The MQTT test needs an enabled config: without `include/secrets.h`, `run.sh`
passes throwaway `-DMQTT_HOST/-DMQTT_USER/-DMQTT_PASSWORD` values to both
translation units (macros do not cross translation units). The PubSubClient stub
refuses anonymous connects, so the credential path is exercised too.
