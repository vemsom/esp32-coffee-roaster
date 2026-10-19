# Firmware notes - assumptions and what is verified

Status: **verified items below were re-confirmed on 2026-10-04** with a clean
`pio run` (zero warnings) and the host-side test suite (943 checks, 0
failures). Push-OTA and filesystem OTA are both verified against real hardware.
Everything still listed as open is untested against real hardware.
Re-confirmed again on 2026-10-17 with the two-pass suite (the firmware now has a
BT-only build mode): **1093 checks, 0 failures** with `SAFETY_ET_ENABLED 1` and
**1035 checks, 0 failures** with it at 0 - see "BT-only build mode" and "RoR
guidance source" below. (Earlier on the same day it read 989/792 and then
1091/976: the suite grew with the filesystem-push tests first and with the
BT-source guidance tests after that.)
**2026-10-17, 0.10.0: the filesystem image has its own push route**
(`POST /api/update/fs`), so the web UI no longer needs espota's dial-back - see
"Default path now: push, for BOTH partitions" and "Verified on hardware
2026-10-17 (0.10.0): the filesystem push" below for the current totals and what
was verified on the device.
**2026-10-17, 0.11.0: the RoR guidance steers on BT when the ET channel is
off** - the machine is still a BT-only build, and a profile step with
`rorTarget > 0` now moves the element there instead of doing nothing - see
"RoR guidance source: BT when the ET channel is off" below.
**2026-10-17, 0.11.1: the control loop is fixed and the fan ramps** - the
integral was clamped in the wrong unit (C*s against the 0-100 % output range),
which quietly made the regulator P-only and parked BT ~20 C under its setpoint;
KD 2.0 differentiated the 250 ms sample and switched duty between 0 and 100;
and the 24 V supply cut out when the fan was switched straight on at 100 %. See
"PID and the control input filter" below. The device reported FW 0.11.0
`build b1c4fbc` when this was written, so the note that the 0.11.0 image was
still unflashed had gone stale in the meantime. Both hashes named here are
pre-rewrite: the scrub of the LAN address turned `b1c4fbc` into `5ca4af8`. The
image on the machine now carries these fixes - `/api/status`: fw 0.11.1,
`build 343fd5c` (rewritten to `21341bc`), built 2026-10-17T18:15:08Z. A flashed
image keeps the build string it was compiled with, so the device is the one
place the old hash survives.

**2026-10-19, 0.12.0: cooling at the end of a run** - "Avbryt och kyl" stops a
roast and starts the fan-only cool-down in ONE request, a profile can carry
`coolFan`/`coolMinutes` so the cooling starts by itself when its last step runs
out, and both are editable in the web UI. See "Cooling at the end of a run
(0.12.0)" below for what changed, what was verified, and the test to run on the
machine. **No image has been flashed with this yet** - the device still runs
0.11.1 (see the paragraph above), so the host-suite numbers are the only
verification so far.

Build of record (2026-09-27, clean rebuild):

```text
platform espressif32 7.1.3, framework-arduinoespressif32 4.20017.260907
Arduino core 2.0.17 (esp_arduino_version.h: ESP_ARDUINO_VERSION 2.0.17)
ESPAsyncWebServer @ 3.12.1, AsyncTCP @ 3.5.0, ArduinoJson @ 7.4.3,
PubSubClient @ 2.8.0, MAX6675 library @ 1.1.2, toolchain 8.4.0
RAM:   16.0 %  (52 280 / 327 680 bytes)
Flash: 73.3 %  (961 017 / 1 310 720 bytes)
[SUCCESS] - no warnings, no errors
FW_VERSION 0.6.0
```

## Cooling at the end of a run (0.12.0, 2026-10-19)

**What was asked for.** Two things, from a roast: a single "cancel and cool"
action (today's stop leaves the fan where it was and starts nothing), and a
per-profile cool-down that runs by itself when the last step is done. The pause
had to stay exactly as it is, and no cooling must be reachable mid-roast.

**What changed.**

1. `POST /api/roast/stop-and-cool` (body `{"coolSpeed":1-100,"coolMinutes":0.5-120}`,
   defaults in the UI: 100 % / 5 min). It calls one new callback,
   `stopRoastAndCool(speed, seconds)`, which does `stopRoastLocked()` +
   `startCoolLocked()` + one fan command inside **one** `StateLockGuard`. Plain
   `POST /api/roast/stop` is byte-for-byte what it was (same handler, same
   callback) and starts no cooling; the routes cannot collide, the library's
   matcher needs a `/` after the URI and this one is `stop-and-cool`.
2. Profiles carry `coolFan` (1-100 %) and `coolMinutes` (0.5-120) at profile
   level. `RoastProfile::totalSeconds()` (sum of every step's ramp + hold) plus a
   check in `updateControl()` is what ends a run now: at `elapsed >= total`, with
   the clock not paused, the run ends and - if the pair is set - the same
   fan-only cool timer starts in the same control cycle. Without the pair the
   element goes off and the fan stops: the behaviour a profile end was always
   described as having. Both fields are clamped to the limits above in
   `RoastProfile::setCool()`, so a hand-written 999 is stored as 120.
3. The web UI: *Avbryt och kyl* (blue, next to the red Stop) with its own fan %
   and minutes fields on the roast tab, and *Kylning efter sista steget* (fan %
   + minutes, saved with the profile) in the profile editor. Export/import carry
   the pair too, so a bundle round-trips.

**Decisions worth stating.**

- **A manual stop never starts the profile's cool-down.** Pressing Stop
  mid-roast ends the run and nothing else. The profile's values belong to the
  profile *running to its end*; the case "beans are dropped early but should
  still get air" is exactly what *Avbryt och kyl* is for, and there the operator
  picks the values by hand. This is why `cbStopRoast()` is not `finishProfileLocked()`.
- **A profile that never ends does not exist any more.** Before this, a profile
  held its last setpoint until someone stopped it - `targetAt()`/`fanAt()` keep
  returning the last value past the end of the curve, and nothing compared the
  roast clock against the profile's length. Ending a run at its last step is the
  point of the feature (there has to be *an* end for the cool-down to attach
  to), and it is what the UI has always drawn: the target curve stops there.
- **A pause near the end holds the run.** `roastElapsedSeconds()` excludes
  paused time, so a paused profile cannot reach its end - which also means the
  pause behaves exactly as documented: clock frozen, PID holding the step.
- **A safety abort starts no cooling** (`abortRunForSafety()` untouched): the
  fan is left running so the beans get air while the fault is handled, and the
  operator decides what happens after.
- **A running cool-down owns the fan** until it ends. Starting a new run while
  one is running is not special-cased (unchanged behaviour): the cool timer
  writes the fan every sample, so it wins over the new run's fan value. Not a
  regression, not something this change introduced - but it is what the host
  test tripped over first, so it is written down here.

**Verified in source and on the host (2026-10-19).**

- `pio run -e esp32dev`: **SUCCESS**, RAM 16.1 %, flash 74.7 % - the image builds.
- Host suite, both passes, exit 0: **1335 checks, 0 failures** with
  `SAFETY_ET_ENABLED 1` and **1203 checks, 0 failures** with it at 0 (before this
  change: 1201 / 1069). The new checks live where the real code is:
  - `test_control` (links the real `main.cpp` + `web_server.cpp`, 173 checks):
    stop-and-cool in ONE lock acquisition with the fan re-commanded before the
    call returns and never 0 % on any sample after it; the same command refusing
    0 % / 0 s and leaving the roast running; plain Stop starting no cooling; a
    5 s profile with `coolFan`/`coolMinutes` ending by itself and handing over to
    the cool timer; the 0/0 and half-filled cases ending with fan 0 and no
    cooling; a manual stop mid-profile starting none; and the pause regression
    (clock frozen, heat held, no end, no cooling while paused - then both once
    it resumes).
  - `test_web_server` (links the real `web_server.cpp` + `roast_profile.cpp`,
    88 checks): the new route is registered and not shadowed by
    `/api/roast/stop`; valid values reach the callback once with seconds; the
    six out-of-range/absent/garbage bodies answer 400 without reaching the
    callback; a refusal from the control layer answers 409; and the profile
    fields round-trip through a real file, are clamped (999 -> 100/120), and a
    profile without them writes a byte-for-byte identical file.
- **Not verified on hardware.** I have no access to the machine (no HA, no
  network keys, and the roaster is not on my segment), so nothing here is
  claimed about the real fan, the real SSR or the real timing. It also has not
  been flashed: the device still runs 0.11.1.

**What to run on the machine, and what to look for.** Flash BOTH images - the
endpoint is in the app image, the button and the fields are in the web UI, which
travels in the filesystem image (`pio run -e esp32-ota -t buildfs` then
`sh tools/ota-push.sh <ip> fs`; export the profiles from the UI first if there
are any on the device, the filesystem image replaces them with the one built
from `data/`).

1. *Avbryt och kyl, mid-roast.* Start a profile, let BT climb past 100 C, then
   set the two fields next to the buttons to 100 % / 2 min and press **Avbryt och
   kyl**. Expect the pill to go straight from *rostning* to *kylning*: Värme 0,
   Fläkt at 100 %, the cooling timer counting down from 2:00 and the fan stopping
   when it reaches 0. The fan must never sit at 0 % in between. Without a browser
   the same request is
   `curl -s -X POST -H 'Content-Type: application/json' -d '{"coolSpeed":100,"coolMinutes":2}' http://<ip>/api/roast/stop-and-cool`
   -> `{"ok":true}`; `/api/status` then has `coolActive: true`, `coolSpeed: 100`
   and `heaterDuty: 0`, and the fan % must not read 0 in a status poll right
   after it.
2. *The profile does it by itself.* Editor: a short profile (e.g. ramp 1 min +
   hold 1 min at 150 C, fan 50), Kylvarvtal 100, Kylminuter 1, save as
   `kyltest`. `curl -s "http://<ip>/api/profile?name=kyltest"` has to show
   `"coolFan":100,"coolMinutes":1`. Start it and wait: the run should end at the
   end of the last step with no Stop press, and the cooling take over for a
   minute.
3. *The old behaviour is still the old behaviour.* The same profile with 0/0
   (or any profile saved before today): at its end the heat goes off and the fan
   stops, with no cooling and no countdown.
4. *A manual stop is still a manual stop.* Start `kyltest` and press **Stop**
   after ~20 s: the run ends, nothing cools, the fan stays where the step left
   it (Stop has always left the fan alone - it is *Avbryt och kyl* that starts
   air).
5. *The pause is untouched.* During a roast press Pausa: the timer freezes, the
   heat keeps holding the step, and the roast continues where it was on resume.
   Pressed near the end it holds there rather than ending.

Report back what the fan actually does in step 1 and 2 (the values on the Fläkt
card and the cooling countdown), and whether the run in step 2 really ends by
itself at the last step. Those two are the parts a host test cannot answer.

## BT-only build mode, SAFETY_ET_ENABLED 0 (2026-10-17)

**Why.** The ET channel stopped reading entirely. On the machine (FW 0.7.0,
build `eeb8dc5`) `/api/status` reported BT live (24.25-24.50) and `"et": null` -
no valid reading since boot - with `safetyFault: true`, `safetyReason: "bt
sensor fault"`, `heaterDuty: 0` and both run modes idle. The alarm could not be
released: `safety_update()` clears only after `SAFETY_CLEAR_STREAK` (10) samples
with **both** channels known and healthy, and an ET that never reads can never
satisfy that. Nothing else releases it - no route, no MQTT message, no power
cycle (the latch lives in NVS, namespace `safety`, and `safety_init()` restores
it on boot). Manual start answered 409 and profile mode did nothing, so the
machine was un-runnable. The fault is judged to sit in the ET MAX6675 **module**
rather than in the thermocouple (BT is healthy and reads live), and until that
module is replaced the roaster has to run on BT alone.

**What was added.** `SAFETY_ET_ENABLED` in include/config.h - default 1, i.e.
the behaviour documented everywhere else in this file is unchanged, and the
macro is `#ifndef`-guarded so a build can set it:

    PLATFORMIO_BUILD_FLAGS='-D SAFETY_ET_ENABLED=0' pio run -e esp32-ota

With it at 0:

- `src/sensors.cpp` does not read the ET converter at all - not "reads it and
  ignores it". `et` is NaN, the same "no reading" a dead module gives, and
  `etFault` is false: there is nothing to fault, and a fault could only feed a
  detector that is switched off.
- `src/safety.cpp` cannot trip `SAFETY_SENSOR_ET`, `SAFETY_STUCK_ET` or
  `SAFETY_OVER_TEMP_ET`, and `coldSpreadFault()` (the `SENSOR_MAX_SPREAD_C`
  cross-check) is compiled out, because an agreement between two channels cannot
  be evaluated with one. The ET reason texts stay in `codeText()`: a latch
  written to NVS by a full build can still be restored by a BT-only one, and it
  should say what it actually was while it is being cleared.
- The clear streak becomes "10 samples in a row that are healthy **on BT**".
  That is what lets the machine start again at all. A stored ET alarm is released
  by that same streak; a stored *stuck*-ET alarm deliberately does not arm the
  "the probe moved again" proof rule, because the value that would release it can
  never arrive when ET is not read - arming it would lock the heater out for
  good.
- Untouched: the BT plausibility window (`SENSOR_MIN_VALID_C`..`SENSOR_MAX_VALID_C`),
  the BT jump check, the BT stuck detector (`SENSOR_STUCK_MAX_MS`), the BT
  ceiling (`SAFETY_MAX_TEMP_C` 260 C), the fan interlock (`FAN_MIN_FOR_HEATER_PCT`)
  and `heater_emergency_off()` semantics. The latch, its NVS persistence and the
  heater side of it are the same code as before.

**Reduced protection is visible, not implied.** `et: null` alone cannot say "not
watched" - a dead module and a switched-off channel look identical over the API.
A BT-only image therefore also reports:

- `/api/status`: `etDisabled: true` (false in every other build), and
  `safetyReason` reads `et disabled - bt only` whenever the latch is clear,
  instead of `none`.
- Web UI (data/index.html): a standing warning pill, `ET AVSTÄNGD – endast BT`
  (English key `ET DISABLED – BT ONLY`), shown for as long as the firmware says
  `etDisabled`. It is a condition rather than an alarm, so it does not blink with
  the run state.
- Home Assistant: a 12th discovery entity, `binary_sensor`
  `coffee_roaster_et_disabled` ("ET disabled", sv "ET avstängd"),
  `device_class: problem`, driven by `value_json.etDisabled`. It is published by
  every build and is OFF in a full one, so nothing appears or disappears when the
  image changes. Still report-only: no `command_topic`, no subscription - the
  same rule as the rest of the MQTT layer.

RoR guidance is no longer inert in this mode: since 0.11.0 it steers on `rorBt`
instead of `rorEt` (`ROR_GUIDANCE_ON_BT` follows `SAFETY_ET_ENABLED`), so a
profile step with `rorTarget > 0` does steer - on the bean channel, which is the
one this build actually measures. See "RoR guidance source: BT when the ET
channel is off" below for what that changes and which constants still have to be
trimmed.

**Host tests.** The suite runs twice, once per mode
(`sh tools/host-tests/run.sh`, or `--et-enabled` / `--bt-only` for a single
pass). `test_safety` keeps pinning every ET trip in the flag-1 pass; the BT-only
behaviour is pinned by the new `test_safety_bt_only`, and `test_sensors`,
`test_mqtt_discovery`, `test_web_server` and `test_control` are built once per
mode. Two of the new checks are the ones that matter: the bench case (nothing
wired, 0 C on every channel) still trips the latch in a BT-only build - carried
by BT's own plausibility floor, since the ET backstop is gone - and the MAX6675
stub counts (`g_max6675EtReads`) that the ET converter is never read at all,
which no value could show, because the API reports a null ET either way.

**Putting it back.** Build with the flag at 1 as soon as the ET module is
replaced, and check the machine: `/api/status` must show `etDisabled: false` and
a live `et` again. The flag is a stopgap - the cold cross-check and the 300 C ET
ceiling are part of the safety net and should not be missing for longer than the
repair takes.

## RoR guidance source: BT when the ET channel is off (0.11.0, 2026-10-17)

**Why.** The ET module on the machine is dead and the machine runs a BT-only
build. Until this release that made an entire feature inert: the guidance
compared its `rorTarget` against `rorEt`, and `rorEt` is built from a channel
that is not read, so `updateRorGuidance()` returned 0 on every cycle. A profile
step with `rorTarget > 0` was accepted, saved, shown in the UI - and steered
nothing. That is a silent failure: no error, no alarm, and the same visible
behaviour as a step that carries no `rorTarget` at all.

It does not have to be that way, and the missing piece was never the maths.
`rorBt` has been computed and published since 0.6.0 (`rorBt` in `/api/status`,
the "Böntemperaturökning" entity in Home Assistant), `ror_valid()` is a property
of the history window rather than of a channel, and the BT probe sits right above
the beans while the ET probe hangs 4 cm higher in the chamber air. BT is the
denser and more meaningful signal of the two; the question was whether the RoR
loop can run without ET at all, and it can.

**What changed** (0.11.0, `Drive the RoR guidance from BT when the ET channel is
off`):

- `include/config.h`: `ROR_GUIDANCE_ON_BT`, `#ifndef`-guarded, defaulting to
  `(!SAFETY_ET_ENABLED)` - so the standard build (ET channel watched) steers on
  `rorEt` exactly as before, and a BT-only build steers on `rorBt`. The
  combination that fails silently - steering on ET in a build that does not read
  ET - cannot be produced by the default, and a forced
  `-DROR_GUIDANCE_ON_BT=0` next to `SAFETY_ET_ENABLED=0` is a `#error` at build
  time instead of an inert loop.
- `src/main.cpp` (`updateRorGuidance()`): the gate and the measured rate follow
  the source. With BT as the source the gate is `!isnan(currentBT)`, and a NaN
  ET no longer disables anything - which is the whole point of the change. The
  gate is on the live reading rather than on the fitted rate, because `ror.cpp`
  keeps publishing the previous rate while a sample is missing: the number alone
  cannot say whether a measurement is behind it.
- Gain, rate limit and `ROR_WINDOW_MS` are **untouched** in this release (3.0,
  15 %/s, 30 s) - see the trim note below.
- Visible wherever the machine is observed: `/api/status` and the MQTT status
  payload carry `rorSource: "bt" | "et"` (compile-time, the same value the loop
  uses), and the web UI's RoR row has a "RoR source" card next to the guidance
  line, which also names the channel inside its text
  (`12.0 C/min · BT 8.4 C/min · active`).

**The constants are still ET-tuned, and that matters more now.** `ROR_GUIDANCE_GAIN`
(3.0), `ROR_GUIDANCE_MAX_STEP_PCT_PER_S` (15) and `ROR_WINDOW_MS` (30 s) were all
chosen against an *ET* curve - a probe in the chamber air, which reacts faster
and moves further than the bean mass. A BT curve differs in two ways that both
push on the same constants:

- **slower**: the bean probe lags. At the start of a roast BT barely moves while
  the chamber air is already climbing, so a target an ET loop tracked will read
  as a large positive error for longer. The rate limit (15 %/s) is what keeps
  that from arriving as a step, and the gain is what decides how hard it pushes
  once it arrives.
- **noisier early on**: right after the turning point the bean signal is small
  and quantised onto the MAX6675's 0.25 C ladder, and a 30 s least-squares fit
  over a slow ramp sits closer to that resolution floor than the same fit over an
  ET curve. That is a gain question first and a window question second.

So before a BT-driven `rorTarget` profile is trusted, trim the three against a
real BT curve on the machine (the calibration route at the bottom of this file:
baseline noise first, then a roast with the loop off and Artisan on the same
beans for comparison). Do not compensate by nudging the constants until one
roast looks calm: the honest claim of this release is that the loop *runs* on BT,
not that it is tuned for it.

**Host tests.** `test_ror_guidance` is built in BOTH passes now, one source each:
the flag-1 pass pins the `rorEt` behaviour that has shipped since 0.7.0
(unchanged), the flag-0 pass pins the BT source - target curve, sign of the
correction, and a cold-probe transient asserted *per cycle* (the correction never
moves more than `ROR_GUIDANCE_MAX_STEP_PCT_PER_S` per second, and the duty never
passes `HEATER_MAX_DUTY_PCT` even when the fit reads -50 C/min) - with the ET
channel reading nothing at all for the whole run. `test_web_server` asserts that
the new `rorSource` field follows the build mode, in both passes, next to
`etDisabled`.

**What is not claimed.** Nothing has been roasted on BT guidance yet. The loop is
exercised by host tests against synthetic curves, and `rorSource` is verified by
reading `/api/status` back after the push; behaviour on real beans is unmeasured,
and the constants above are the reason to expect it to need work.

## Verified on build / in source

Fan PWM LEDC API (src/fan_control.cpp) - **CLOSED 2026-09-26.** The code uses
the channel-based API (`ledcSetup` + `ledcAttachPin` + `ledcWrite` with
`FAN_LEDC_CHANNEL 0`). The resolved framework is Arduino core **2.0.17**, where
that is the *native* API, not a compatibility layer:
`cores/esp32/esp32-hal-ledc.h:30` declares
`ledcSetup(uint8_t channel, uint32_t freq, uint8_t resolution_bits)` and
`:36` declares `ledcAttachPin(uint8_t pin, uint8_t channel)`. The pin-based
3.x API (`ledcAttach(pin)`) does not exist in this framework at all, so there
was never anything to rewrite - the earlier note had it backwards.
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
exercised by host tests with stubbed Arduino calls - `test_safety` (57 checks),
`test_mqtt_discovery` (214 checks) and `test_control` (103 checks, which drives
the real setup()/loop() and also runs a second thread in the role of the
AsyncTCP task); the same `test_control` source is rebuilt under
ThreadSanitizer. See the Host-side tests section below for the full current
figures.

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
as a red banner in the web UI) and as the `binary_sensor` "Säkerhetslarm" in
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
- **CLOSED 2026-09-26 (host-tested)** Stuck-sensor
  detection exists: `SENSOR_STUCK_MAX_MS` (60 s of bit-identical readings
  while the element is asking for power), conditions 4 above. The window was
  chosen so that neither roasting nor cooling can false-trip - see the two
  rules in include/config.h. Residual: the 60 s number still wants a
  confirmation against the real noise figures from calibration step 2 (record
  the longest run of *identical* consecutive samples there; if a healthy
  static reading ever holds still that long with heat on, raise the constant).
  That is a calibration input, not a missing feature.
- **CLOSED 2026-09-26 (host-tested)** The alarm state is
  persisted to NVS and re-asserted in `setup()` - see "Persistence" above. The
  behaviour change is deliberate: a power cycle no longer
  clears an alarm. Degradation path if NVS is unavailable: RAM-only latch, one
  serial log line.
- **REQUIRES HARDWARE** 260 C is a guess for this popper and these probes.
  Confirm against the hardware before the first real roast (step 7 below), and
  keep in mind that a K-type thermocouple in a hot air stream reads air, not
  bean temperature.

## PID and the control input filter (0.11.1, 2026-10-17)

Three faults in the loop that decides how much power the element gets, all
measured on the machine on 2026-10-17. None of them was a probe calibration
problem, which is what the ~20 C offset first looked like.

### 1. The integral was clamped in the wrong unit (the ~20 C offset)

`SimplePID::compute()` accumulated `_integral += error * dt` - C*s - and then
clamped that raw state against the OUTPUT range (0..100 %). The clamp therefore
capped the I contribution at `KI * 100` = 5 %, and a regulator whose I term can
never exceed 5 % is a P-only loop: it settles where `KP * error = duty`, i.e. at
`error = duty / KP` = 85 / 4.0 ≈ 21 C under the setpoint. That is the measured
behaviour exactly - BT 168-170 C against a 190 C setpoint with `heaterDuty`
82-86 %, and 188-192 C with duty pinned at 100 % and a 203 C setpoint.

Fixed in include/pid.h: the clamp now applies to `KI * integral` - the product
the output is actually built from - and the stored state is written back from
the clamped product, so a fully wound integrator is at most `outMax / KI` in
C*s rather than an unbounded number no clamp ever touched. On top of that,
**conditional integration** as anti-windup: a sample is committed only while it
does not push an already saturated output further out. The opposite case (output
saturated, error pointing back inside) *is* committed - that is what lets a
saturated state unwind instead of pinning.

### 2. KD differentiated the sampling noise

`PID_KD` was 2.0 on a signal sampled every 250 ms and quantised to the MAX6675's
0.25 C ladder, and the machine showed it: `heaterDuty` alternating between 0 and
100 between two readings. A derivative over that much dither is a switch, not a
correction, so KD is **0** - removed, not trimmed down. What the D term was
supposed to do (react to a trend instead of to the noise) is what the input
filter now does.

### 3. The control input is filtered - and the safety path is not

`include/filter.h` is a one-pole filter with `PID_INPUT_FILTER_TAU_S` = 1 s,
applied in src/main.cpp to the PID's input only:

- the PID computes on `btFiltered` (reported in `/api/status`);
- `safety_update()`, the `SENSOR_FAULT_MAX_JUMP_C` jump check and the
  `SENSOR_MIN_VALID_C` plausibility window keep seeing the RAW sample from
  `sensors_read()` - and so do the RoR fit, MQTT/HA and `/api/status`'s `bt`.

The split is a hard requirement, not a taste: a dropped probe behind a 1 s
filter arrives as a plausible ramp, so a filter in front of the jump check would
hide exactly the fault that check exists to catch. `test_control` pins both
halves of it - a 19 C step both probes make together (the raw reading follows it
in the same sample, the control input lags) and then a 21 C jump (the latch
still trips as a BT sensor fault).

### New tuning - a starting point, NOT a tuned set

| gain | old (never tested on hardware) | new (also untested) |
|------|-------------------------------|---------------------|
| KP   | 4.0                           | 2.0                 |
| KI   | 0.05                          | 0.15                |
| KD   | 2.0                           | 0.0                 |

The old set was written before the machine existed, and the first roast on it
showed what it really did. The new set is an estimate with a reason per number
(include/config.h) and **has to be trimmed at the next roast**: KP until the
ramp tracks its setpoint, then KI, and a small KD only if a real overshoot shows
up on the filtered signal. `test_pid` pins the properties that have to hold
whatever the numbers become: error → 0 under a constant load, no windup, and a
dithering input that cannot switch the duty between 0 and 100 (the same signal
is run through the old gains and derivative there as the control group, so the
failure mode is committed rather than described).

### Trimming against a running machine

`/api/status` carries the loop's internals now, so the gains can be watched
while the machine runs instead of between roasts:

| field | meaning |
|-------|---------|
| `pidP` / `pidI` / `pidD` | the three contributions as last applied, in percent |
| `pidIntegral` | the raw integral state in C*s - pinned at `outMax / KI` (667 here) means the loop is asking for more than the element can give, and the offset left is physical, not tuning |
| `btFiltered` | the bean temperature the PID read (compare it with `bt`, the raw sample) |

All of them read 0 while no run is active, so "no run" is distinguishable from
"a run with a silent term". No web-UI change in this release: the fields are for
whoever is trimming, not for the roast view.

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
  ("FLÄKT <10 % - värmen av") and as the `binary_sensor` "Fläktspärr" in HA.
  Kept out of `safetyFault` on purpose: different condition, different fix, and
  it clears on its own.

In profile mode the fan is applied *before* the duty is computed, otherwise the
first sample of a run would be judged against the previous run's fan value.

## Fan duty curve (implemented 2026-10-04, NOT verified on hardware)

A 24 V DC fan driven by PWM through the MOSFET does not answer linearly to the
average voltage: below a start threshold it stands still, and just above it the
speed climbs steeply. On the bench it only starts at about **60 % duty**
(≈14.4 V), which left the bottom 60 % of the requested range dead and the top
40 % too fine to use as a control input. That is normal fan behaviour, not
necessarily a hardware fault.

`fan_set_speed()` (src/fan_control.cpp) therefore keeps its percent API and
changes only the duty that reaches the LEDC channel:

- `percent <= 0` → duty 0, the fan off, exactly as before.
- `1..100` → mapped linearly onto the duty band
  `[FAN_DUTY_MIN_PCT, FAN_DUTY_MAX_PCT]` (include/config.h, default 60..100):
  `duty_pct = MIN + percent * (MAX - MIN) / 100` (integer), then scaled to the
  8-bit duty range. So 1 % → 60 % duty (the fan turns, above the interlock's
  10 % threshold) and 100 % → 100 % duty.
- Guard rail: if `FAN_DUTY_MAX_PCT <= FAN_DUTY_MIN_PCT` the function falls back
  to the old straight 0–100 map, so a misconfigured constant cannot freeze or
  invert the fan.

What is reported everywhere stays the **requested** percent: `/api/status`'s
`fanSpeed`, the MQTT/HA fan entity, the profile steps and the manual fan input
all keep speaking 0–100, so the change moved no entity in Home Assistant.

### The ramp to it, and what the interlock judges on (0.11.1, 2026-10-17)

The 24 V supply cuts out when the fan is switched straight on at 100 %: the
start current of a stalled DC motor is more than it will take. So the duty no
longer jumps to the request - it climbs at `FAN_SLEW_PCT_PER_SEC` (include/
config.h, default 25 %/s, i.e. 0 → 100 % in 4 s). A request that does **not**
ask the fan to go up is still written in the same call: spinning down draws no
start current, and the element has to be cut in the control cycle the fan is
asked to stop.

That splits one number into two, and the split is the whole point:

- `fanSpeed` - the requested speed, unchanged, still what HA/the UI/profiles say;
- `fanActual` (`fan_actual_pct()`) - the speed the fan has actually reached.

`applyHeaterDuty()` now compares the interlock against `fanActual`, not against
the request. Judging on the request is how the element gets lit while the fan is
still spinning up: a profile step asking for 100 % would satisfy a
`FAN_MIN_FOR_HEATER_PCT` (10 %) test instantly while the fan is still at 0 %.
`test_control` drives exactly that - one sample after a 0 → 100 % request the fan
is at ~6 %, and the heat is refused - and `test_fan` pins the ramp itself
(monotone, the documented time, and the fan still under the minimum one sample
in).

The rate is `#ifndef`-guarded, so a build can change it
(`-DFAN_SLEW_PCT_PER_SEC=50`) or remove the ramp entirely
(`-DFAN_SLEW_PCT_PER_SEC=0` - the old immediate write, kept as an escape hatch so
a bisect can tell the ramp apart from everything else in this release;
`test_fan` is built a third time with it at 0).

**NOT verified on hardware** - the fan in this build has never been started
through the ramp. The 25 %/s is a first guess at what the supply tolerates: if
it still cuts out, lower it; if it tolerates more, raise it. What *is* verified
is the behaviour above, host-side.

**NOT verified on hardware** — the 60 % knee is one bench measurement with the
fan in its current wiring, and the band has not been swept at 60/70/80/90/100 %
duty.

Tuning `FAN_DUTY_MIN_PCT` needs no instrument at all: raise it until the fan
visibly starts on the lowest setting, with the real fan in place. It can be set
at build time (`-DFAN_DUTY_MIN_PCT=65`, the macro is `#ifndef`-guarded) or in
include/config.h.

Only the later "should the band carry an exponent" question needs a speed
figure, and a plain 2-wire DC fan has no tach output to read. Three ways to get
one, cheapest first:

- **no instrument**: judge airflow by its effect instead of RPM - at a fixed
  heater duty, how fast does the chamber answer a fan step? That is the quantity
  the roast actually cares about, and it needs nothing but the two probes.
- **handheld optical tachometer**: point it at a mark on a blade; one reading
  per duty step, no wiring.
- **a fan with a tach wire** (FG): its pulse train goes to a spare GPIO and
  counts as revolutions per second, but that means a different fan.

The exponent is a separate change with that data behind it, not part of this
one.

`tools/host-tests/test_fan.cpp` pins the mapping (0 → 0, the band floor at 1 %,
the midpoint, the top, clipping of 101 and negative values, monotonicity, and
the straight-map fallback for a broken band). It is built twice, once per band.

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

## OTA (implemented 2026-09-26, push endpoint added 2026-10-03)

The first flash has to be USB - the firmware has never had an update path, and
adding one does not retroactively make a stock ESP32 updateable.

**Default path now: push, for BOTH partitions.**

```sh
pio run -e esp32-ota                        # app image
sh tools/ota-push.sh 192.168.x.x            # -> POST /api/update
pio run -e esp32-ota -t buildfs             # filesystem image (from data/)
sh tools/ota-push.sh 192.168.x.x fs         # -> POST /api/update/fs
```

The uploading machine connects **to** the roaster and POSTs `firmware.bin` to
`/api/update` (the app slot) or `littlefs.bin` to `/api/update/fs` (the
filesystem partition, i.e. the web UI), token in the `X-OTA-Token` header. That
direction is the whole point: the ArduinoOTA path described below needs the
*device* to open a TCP connection back to the uploader, and in a segmented
network new connections IoT -> LAN are blocked, so that handshake dies right
after `Authenticating...OK` while the device is up and reachable - which is
exactly why the UI never reached the roaster before 0.10.0: the app image went
in over `/api/update`, the filesystem image had only the espota route, and
espota could not complete. LAN -> IoT works, so the uploads were turned around
instead of the firewall being opened for a port that only matters during an
upload (where the espota listener is also unauthenticated, first connection
wins).

**How the two routes are kept apart.** The library does not compare paths for
equality: `server.on(uri, ...)` registers a *BackwardCompatible* matcher with
the rule `uri == path || path.startsWith(uri + "/")` and the **first**
registered handler that matches wins (`ESPAsyncWebServer/src/WebServer.cpp`,
`canHandle` + the `_addHandler` order). The path `/api/update/fs` therefore
matches the URI `/api/update` too. `/api/update/fs` is registered **first**, so
the filesystem image can never be handed to the app handler: that would write
it with `U_FLASH` into the app slot and, on a matching checksum, commit a
filesystem as the boot image. The host test asserts the order
(`AsyncWebServer::routeFor()` in the stub models both halves of the library's
rule), and sizes are checked per route as well - see below.

Rules around the endpoints (`include/ota_push.h`, `src/ota_push.cpp`) - the
same state machine serves both routes, so a rule cannot be present on one and
missing from the other:

- **Only the allowed client.** `OTA_ALLOWED_CLIENT_IP` in `include/secrets.h`
  is the one address whose requests are even considered. A valid token is not
  enough: port 80 is the device's normal web server and IoT devices reach each
  other inside the IoT network, so the network does not isolate this route. A
  request from anywhere else gets **404 - the same answer an unknown path
  gives** - so a probe cannot map the endpoint at all.
- **Token required.** `OTA_TOKEN` in `include/secrets.h` (32 hex characters,
  never committed, never printed). The header is compared before
  `Update.begin()`, so an unauthorised request writes no byte at all. A wrong
  token also answers **404, not 401**: a 401 would confirm the route exists.
  **Empty token disables the endpoint completely** - the same rule as MQTT
  (no user -> disabled) and ArduinoOTA (no password -> not started): 503, not
  a fallback.
- **The checksum is the real protection.** The token travels in clear text
  over Wi-Fi; the image does not have to. `tools/ota-push.sh` computes
  `sha256sum firmware.bin` and sends it as `X-OTA-SHA256`, and the device
  hashes every byte it writes with mbedtls SHA-256 and compares before
  committing. A mismatch answers **403, calls `Update.abort()`** and leaves
  the running image as the boot target - so a wrong or half-written image
  never becomes the firmware. A malformed checksum header is refused before
  the body is read.
- **Refused while the machine is busy**: **409** when a session runs (roast /
  manual / cool-down) **or when the element is still asking for power** - not
  "when a roast is running", because the element is the thing that must never
  be live during a flash.
- **The restart happens after the response.** The handler writes the image,
  answers 200, and only then does `loop()` restart - otherwise the client sees
  a broken connection on an upload that actually succeeded. (Confirmed on the
  device: the connection does sometimes close before curl reads the status
  line, which is why `ota-push.sh` waits for the device to come back instead
  of trusting the exit code.)
- **Nothing else is touched.** `Update` writes to the inactive OTA slot; NVS
  and LittleFS are not part of that, so saved profiles survive an **app** push.
  The host test asserts exactly that against the real handler. (A filesystem
  push is the exception, and it is the point of the next section.)
- **One transfer at a time.** A second body arriving while one is being
  written gets **409** - two uploads interleaving chunks into the same `Update`
  is not survivable, and on the filesystem route the second one would unmount a
  partition that is already being rewritten.
- **Sizes belong to the route.** An image for `/api/update` may not exceed the
  OTA slot (`OTA_PUSH_MAX_BYTES`); an image for `/api/update/fs` may not exceed
  the filesystem partition (`OTA_PUSH_FS_MAX_BYTES`, `0x160000` - the size of
  the `spiffs` partition in the board's default table, which is exactly what
  `littlefs.bin` comes out as) and must be at least `OTA_PUSH_FS_MIN_BYTES`
  (16 KiB), because a body of a few hundred bytes is not a filesystem and
  writing it would leave the partition unmountable. Too large answers **413**,
  too small **400**, both before a byte is written.

### The filesystem route: same checks, one more thing to get right

`POST /api/update/fs` writes `littlefs.bin` with `Update.begin(total, U_SPIFFS)`
- the command comes from the route, never from the body. Everything the app
route does happens here too (client, token, checksum, window, 409, 413/400),
and then the part that only a filesystem transfer has:

- **LittleFS is unmounted before the first byte.** A mounted filesystem holds
  its own metadata and buffers in RAM and goes on writing its view of the same
  blocks underneath the incoming image; the transfer dies part-way, which is
  what an interrupted write looks like from the outside. That was the
  `[ERROR]: Error Uploading` after ~190 chunks on the espota path, and the push
  route follows the same rule (`LittleFS.end()` in `ota_push_begin()`, before
  the transfer is marked authorised).
- **The profile endpoints answer 503 while it is unmounted** instead of reading
  a half-written partition as if it were a filesystem - `web_fs_available()`
  carries both sources (ArduinoOTA's `onStart()` and the push route).
- **The filesystem is mounted again when the transfer ends**, before any
  restart: after a verified image it is mounted as it is, after a failed one it
  is formatted first (the partition is half-written either way, and that is what
  `ArduinoOTA.onError()` does for the same reason). If the mount fails, the
  endpoints keep answering 503 instead of serving whatever is there, and the
  reboot after the transfer mounts it at boot.
- **The transfer is verified before it is committed**, exactly like the app
  image: SHA-256 over every byte on the device, `Update.abort()` on a mismatch,
  and no restart unless `Update.end()` succeeded.

### Filesystem OTA over espota: the command decides, and LittleFS must be unmounted

**The reserve path** - the standard one is the push route above. The rule it
established (unmount before the first byte) is the same rule the push route
follows; what follows here is why, and what went wrong with it first.

`sh tools/ota-upload.sh <ip> fs` sends `littlefs.bin` to the filesystem
partition with `--spiffs`, i.e. through the same espota protocol as an app
upload. It is kept because it works on a network where the device *can* dial
back, but it is no longer the way the UI gets updated: espota needs the device
to open a TCP connection back to the uploader, which is the direction that does
not work here. This is the bug that made the original attempt die: the transfer
started, ran for ~190 chunks, and died with
`[ERROR]: Error Uploading` after about 27 s.

The cause was **not** the network. `ArduinoOTA` passes the transfer command to
`Update.begin(size, command, ...)` itself (`ArduinoOTA.cpp:250`), so `Update`
knew it was writing SPIFFS - but `onStart()` ignored the command, and LittleFS
was still **mounted** on the partition being rewritten. A mounted filesystem
holds its own metadata and buffers in RAM and goes on writing its view of the
same blocks underneath the incoming image. The transfer dies part-way, which is
what an interrupted write looks like from the outside.

The rule in `onStart()` is now: **the command decides what gets out of the
way.**

- `U_SPIFFS` -> `LittleFS.end()` before the first byte lands, and the profile
  endpoints answer **503** for the duration instead of reading a half-written
  partition as if it were a filesystem.
- `U_FLASH` -> LittleFS is left alone. It writes the other partition and never
  shares a block with the filesystem, and the profiles are read for the rest of
  that boot.
- A failed transfer remounts before the restart, so the device comes back
  readable either way.

**Verified against the hardware 2026-10-03:** `Result: OK`, `Success`,
1 441 792 bytes, twice in a row, and the device's `/` afterwards is **51 813
characters and contains `valRorBt`** - with the transfer running on firmware
that has this fix in it. Before the fix the same command never got past the
handshake.

### The upload-flag trap, and why the script calls espota directly

Two different PlatformIO traps sit here, and both were hit while making the
filesystem transfer work:

- **Everything on one line** in `upload_flags` makes PlatformIO pass
  `"-P 32320 -I <address>"` *inside* the `--auth` value: espota answers
  `Authenticating...FAIL`.
- **One flag per line** (the form that gives separate arguments) gives every
  value a **leading space**, so `host_ip` becomes `" <address>"` and espota
  dies on `[ERROR]: Listen Failed` before the transfer even starts. That
  message reads like a network problem; it is a stray blank.

There is no `upload_command` key in PlatformIO, `extra_scripts` set on the env
overwrite the one inherited from `extends`, and `UPLOADERFLAGS` is re-set by
`builder/main.py` after every script hook has run - so no script can clean it
up reliably. `tools/ota-upload.sh` therefore **calls `espota.py` directly**
with the arguments as a list: no middleman, no space that can be added.
`-I <your machine's LAN address>` (read from `secrets.h`) and `-P 32320` are
both still required, and the password is read from `secrets.h` as before.

`platformio.ini` keeps its `upload_flags` for anyone who runs `pio run -e
esp32-ota -t upload` by hand, with the trap documented next to it.

### Branch state (2026-10-04)

`main` and `ota-hardening` are now the same line of history: `main` was a strict
ancestor of the branch, so the join was a fast-forward rather than a merge
commit - one parent, no empty merge, easier to read. Both contents live on
`main`:

- from `main`'s own commits: `LICENSE`, `SAFETY.md`, the per-module SO/CS sensor
  pinout (`PIN_MAX6675_MISO_BT`/`_ET`), and the docs work
- from the branch: push OTA, the filesystem-OTA `U_SPIFFS` fix, the build
  identity in `/api/status`, and the sensor-offset work with its tests

**Comparing firmware images across directories is a trap.** Same tree built
twice differs by 66 bytes - the `FW_BUILD_TIME` string and nothing else, so a
hash never matches between two build minutes. Worse, a build in a `git worktree`
has no git context, so `tools/fw_build_id.py` stamps `FW_BUILD_SHA` as
`unknown` and the image is a **different size** (938 752 vs 979 360 bytes) for
identical source. Build both revisions in the main working tree before
concluding anything from a binary, or compare `src/`/`include/` and the file
size instead of the hash.

**Verified on the merged tree:** 916 host checks, 0 failures; `pio run` green
with RAM 16.1% and Flash 74.2%; build id `a7aa818` with no `+dirty`.

### Proving a push landed: the build identity in `/api/status`

`FW_VERSION` says which *release* is running. It cannot say which *build* of
it, and that is the question after a push: did the image I just sent actually
take, or did the endpoint answer 200 for something else? A version number that
did not change answers nothing.

So `tools/fw_build_id.py` runs as a PlatformIO `pre` script, stamps the git sha
and the build time into `build_flags`, and `/api/status` reports them:

```sh
curl -s http://192.168.x.x/api/status | python3 -m json.tool | grep -E 'fw|build|built'
#   "fw":    "0.9.0"                    <- same string HA shows as sw_version
#   "build": "82888dc"                  <- git rev-parse --short HEAD
#   "built": "2026-10-03T11:38:27Z"
```

One request, and the check is a comparison:

```sh
git rev-parse --short HEAD        # on the machine that sent the image
```

If they differ, the push did not land and the device is still running
something else - no guessing from entity counts. `build` carries a `+dirty`
suffix when the working tree had uncommitted changes at build time, because
then the image is not exactly that commit and the answer should not pretend
otherwise. Outside a git checkout (a zip download) both fields read `unknown`
rather than failing the build.

Keeping `fw` equal to `FW_VERSION` - the same value `mqtt_client.cpp` publishes
as HA's `sw_version` - is deliberate: the web API and Home Assistant must never
disagree about which release is running.

### Verified on hardware 2026-10-17 (0.10.0): the filesystem push

The first run of the new route, on the roaster at 192.168.x.x, from the
server. This is the run that proves the claim the route exists for: the page
the device serves is the page in `data/`.

```text
app push first (the fs route only exists in the new app):
POST /api/update, 980 768 bytes, clean tree at ba004e6 -> 200/reboot
/api/status afterwards: fw "0.10.0", build "ba004e6", etDisabled true,
                         safetyFault false, safetyReason "et disabled - bt only"
                         (SAFETY_ET_ENABLED 0, the build mode the machine runs)

filesystem push:
page the device served before:
GET /                                     -> 51 813 bytes
POST /api/update/fs, 1 441 792 bytes, sha256 announce -> 200/reboot
page the device serves after:
GET /                                     -> 52 745 bytes, byte-identical to
                                             data/index.html (52 745), and it
                                             contains both pill texts
                                             ("ET AVSTÄNDD", "ET DISABLED")
GET /api/profiles                         -> HTTP 200 (LittleFS mounted again
                                             after the reboot)
GET /api/update/fs with token             -> HTTP 405
GET /api/update/fs without token          -> HTTP 404
GET /api/update with token                -> HTTP 405
GET /api/update without token             -> HTTP 404
```

Before this run the device had been serving 51 813 bytes for a week while
`data/index.html` had grown to 52 745: the app image went in over
`/api/update`, and the filesystem image had no route that worked. That is the
whole reason for the second route.

Two notes from the run:

- The app image was first pushed with the **wrong build mode** (the default
  `SAFETY_ET_ENABLED 1`) by accident, and `/api/status` immediately showed the
  difference: `etDisabled false`, `safetyFault true`, `safetyReason "et sensor
  fault"` - the ET channel is dead on this machine, so a full build latches the
  safety alarm and refuses to roast. Rebuilding with
  `PLATFORMIO_BUILD_FLAGS='-D SAFETY_ET_ENABLED=0' pio run -e esp32-ota` and
  pushing again restored `etDisabled true`, `safetyFault false` (the BT streak
  clears the restored ET latch within 2.5 s, as designed). Build the BT-only
  image for this machine, and read `etDisabled` back to confirm it.
- `GET /api/update/fs/bogus` with the token answers 405 too: the library's
  BackwardCompatible matcher treats any path **under** a registered URI as a
  match. For the update routes that is harmless in the safe direction - a POST
  to `/api/update/fs/anything` still reaches the filesystem handler and writes
  the filesystem partition, and `/api/update/anything` reaches the app one.

**Verified on hardware 2026-10-03** against the roaster at its LAN address:

```text
POST /api/update, 974 512 bytes, sha256 announce  ->  HTTP 200, device reboots
wrong token (right client, right shape)           ->  HTTP 404 "Not Found"
right token, checksum 0000...0000                 ->  HTTP 403, device keeps running
GET /api/update with token (trusted client)       ->  HTTP 405 "POST here"
GET /api/update without token                     ->  HTTP 404 "Not Found"
```

The first hardware run exposed a bug the host test had missed: the library
keeps feeding the remaining chunks of a body to the handler **after** the
refusal was sent, so chunk two reached `Update.write()` with no transfer
started, `Update.write()` failed, and the client saw **500 "write failed"
instead of the hiding 404**. `dispatch()` in the host test stopped at the
first chunk, which is why it passed. The handler now remembers a refusal for
the rest of the body, and the test sends every chunk - thirteen checks fail
without that guard.

### ArduinoOTA (kept, no longer the default)

Still in the firmware, harmless, and it may well work on a flat network where
the device can dial back. With a password it is not a liability, so it stays
as a second option - but the documented way in is the push above.

**It does work here, and there was never a firewall rule involved.** What
actually had to be in place, in the order it was found:

1. **`U_SPIFFS` handling for filesystem transfers.** See the section above: the
   transfer died part-way because LittleFS stayed mounted on the partition
   being rewritten. This, not the network, is what killed the upload.
2. **No stray whitespace in the flag.** PlatformIO's `upload_flags` can pass an
   `-I` value with a leading space, and `espota` then fails with
   `[ERROR]: Listen Failed`. That reads like a network fault and is a blank.
3. **`-P 32320`** pins the return port (espota otherwise picks a random one
   between 10000 and 60000) so the traffic is predictable. Hygiene, not a
   requirement: no firewall rule is needed either way. A killed run can leave
   the port occupied (espota sets no `SO_REUSEADDR`), and the next bind then
   fails with `[ERROR]: Listen Failed` - pick another port.
4. **`-I <your machine's LAN address>`** is in the command as **hygiene, not as
   a cause**: it
   makes the listener unambiguous in a container with virtual interfaces. Measured
   2026-10-03: a full transfer goes through **both with and without** `-I`
   (without it espota binds `0.0.0.0:<random port>` and everything works), so
   it does not explain the old `No response from device` - that disappeared
   when the `U_SPIFFS` handling went in. Do not write it in as a cause.

All of them live in `tools/ota-upload.sh`, which sets the arguments itself, and
`platformio.ini` keeps its `upload_flags` for a hand-run `pio` with the traps
documented beside it. **Do not call `espota` by hand without reading that
script first** - the whitespace trap is invisible in the command line and shows
up as a network-sounding error.

**Chosen approach: ArduinoOTA with a password**, not an upload page. In order
of weight: it ships with the ESP32 Arduino core (no new entry in `lib_deps`),
PlatformIO drives it natively (`-e esp32-ota -t upload`), and it opens no new
HTTP endpoint that accepts binaries.

Two properties make it safe to run next to a live heater:

- **No transfer is served while a session is running.** `serviceOta()`
  simply does not call `handle()` during a roast, a manual session or the
  cool-down, so `espota` times out instead of interrupting half a roast. The
  reason is logged once on the serial console ("paused: a session is
  running"), so a timeout during a run is explicable rather than mysterious.
- **`onStart()` latches the heater off before the first byte.** No control
  cycle runs while the transfer does, and a time-proportioning heater left at
  its last state would hold the element *on* for the whole upload. The latch
  is only cleared by `heater_clear_emergency()`, which never runs afterwards:
  a finished transfer reboots, and a failed one restarts too, so the device
  always comes back with the heater off and no session running.

Configuration: `OTA_PASSWORD` in `include/secrets.h` (32 hex characters,
never committed, never printed). Empty means `startOta()` does not start the
service at all - the same rule MQTT follows: no password, no service, and
nothing listening on the network. The upload helper reads the value from
`secrets.h` at upload time, so it appears nowhere in `platformio.ini`, on a
command line, or in a shell history.

**What does *not* travel with a firmware update: `data/`.** `ota-push.sh` on
its own sends `firmware.bin` to the app slot and nothing else - the web UI
lives in the LittleFS image on its own partition, so a UI change needs its own
push. Since 0.10.0 that push is the **second route on the same endpoint**:

```sh
pio run -e esp32-ota -t buildfs       # rebuild the filesystem image from data/
sh tools/ota-push.sh <ip> fs          # POST /api/update/fs
```

Changing `data/` and pushing only the app image is the failure this route
exists to prevent, and it is silent: the device keeps serving the old page, and
the app answers `/api/status` with fields the page it serves knows nothing
about. `ota-push.sh fs` therefore compares the size of the page the device
hands out with `data/index.html` **after** the transfer and fails loudly on a
mismatch - "the image went in" and "the page changed" are two different claims.

The espota route (`sh tools/ota-upload.sh <ip> fs`, which runs
`pio run -e esp32-ota -t uploadfsota` and sends the image with `--spiffs`) is
kept as the reserve for a network where the device can dial back: PlatformIO
adds the flag for that target (`builder/main.py:460-461`), `espota.py` switches
the command to SPIFFS (`tools/espota.py:345-347`), and the ESP32 ArduinoOTA
accepts `U_SPIFFS` (`libraries/ArduinoOTA/src/ArduinoOTA.cpp:176`). `uploadfs`
over USB remains the fallback, not the first hand: the first flash and any
recovery have to be USB, but nothing routine does.

Host-tested: OTA starts exactly once when the link comes up, is served while
idle, is *not* served while a run is active, latches the element low on
`onStart()` while a manual heat is conducting, and requests a restart on both
`onEnd()` and `onError()`.

The push path has its own host test (`tools/host-tests/test_ota_push.cpp`,
against the real handler and the real state machine): a request from any
address other than `OTA_ALLOWED_CLIENT_IP` is refused with 404, and a valid
token does not change that; a wrong, missing or short token and a malformed or
missing checksum header are all refused with 404 *before* `Update.begin()`, with
the rest of the body arriving too (the chunks after a refusal must not turn the
answer into a 500 - the bug the first hardware run found); a run in progress or
a live element is refused with 409; a missing Content-Length is refused with
400 and an oversized image with 413; an image whose SHA-256 does not match is
refused with 403 and discarded with `Update.abort()`, leaving the running image
as the boot target; a half-sent image aborts instead of committing; a complete
transfer writes exactly `Content-Length` bytes, ends with `Update.end()`, leaves
every profile byte-identical and only then asks for the restart.

The filesystem route is driven through the same handler and the same state
machine, so the whole matrix above is asserted on `/api/update/fs` too - plus
the four things that only that route can get wrong:

- **The route decides the partition.** The app route is asserted to ask
  `Update.begin()` for `U_FLASH` and to leave LittleFS mounted; the filesystem
  route for `U_SPIFFS`, whatever the body looks like. And because the library's
  BackwardCompatible matcher also matches the URI `/api/update` against the path
  `/api/update/fs`, the test asserts which handler the *device* would pick
  (`AsyncWebServer::routeFor()` in the stub models both the matcher rule and the
  first-match-wins order). With the app route registered first, that check
  fails: a filesystem image would be written to the app slot and committed as
  the boot image.
- **Unmounted before the first byte.** Checked at the first chunk: the transfer
  is in progress, one chunk is written, `Update.lastCommand` is `U_SPIFFS`, and
  `LittleFS.mounted()` is already false - with `end:/` in the filesystem stub's
  operation log, so it is the real `end()` and not just a flag.
- **503 while unmounted, and mounted again afterwards.** The profile endpoints
  answer 503 during the transfer (never a half-written partition served as a
  filesystem) and 200 again after it; a rejected transfer, a wrong checksum and
  a dropped connection each mount the filesystem again before any restart.
- **Sizes per target.** The filesystem route refuses a body larger than the
  partition (413) and one smaller than any filesystem can be (400), both before
  `Update.begin()`, and neither unmounts anything.

The endpoint also has a second binary, `test_ota_push_disabled.cpp`, built with
`-DOTA_TOKEN=""`: both routes answer **503** to a well-formed, authenticated
POST and open no partition at all, and both GET probes answer 404. That is the
build a device without a configured token runs, and the one place where the
route is allowed to admit it exists.

The checksum test hashes with the same SHA-256 the device runs - the host
stub in `stub/sha256.cpp` is a real implementation, checked against the
published vectors for the empty string and `"abc"` first, so a broken hasher
cannot make the test agree with itself and pass.

**Verified against real hardware 2026-10-03** (see the OTA section): a full
974 896-byte push answered 200 and the device rebooted into it; the trusted
client's GET probe answered 405 and a stranger's answered 404; a wrong token
answered 404; a wrong checksum answered 403 and the device kept running. The
device's own `/api/status` then reported `"build":"82888dc"`, matching
`git rev-parse --short HEAD` on the machine that sent it - that is the push
proving itself, which is the whole point of the build identity above.

Known unknown: whether an upload reaches the roaster from the machine that runs
the script depends on the network, not the firmware. Push needs LAN -> IoT only
(which works here); if the guest network is different, run it from a machine
that can reach the roaster.

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

Broker for this build: any MQTT broker that requires authentication. The
firmware treats an anonymous CONNECT as fatal for the feature (the broker
answers CONNACK rc=5 "not authorized"), so it refuses to enable MQTT when
`MQTT_USER` is empty instead of retrying a connection that can never succeed.
Credentials are not in the repo: they go in include/secrets.h,
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

Entities published in discovery (12 configs, all read-only, all under
`homeassistant/<component>/coffee_roaster_<object>/config`): bean temperature,
environment temperature, rate of rise for bean and for environment
temperature, heater duty, fan speed, mode, elapsed seconds and profile
(sensors), plus the safety alarm, the fan interlock and the ET-disabled flag
(binary_sensors, all device_class problem). Every config is retained and carries
the device block, unique_id and availability topic. The ET-disabled entity is
OFF in a build that reads the ET channel and ON in a BT-only build (see
"BT-only build mode" above) - it is published by every build on purpose, so a
change of image cannot add or remove an entity in Home Assistant.

The two rate-of-rise sensors carry `unit_of_measurement` "C/min",
`state_class` measurement and deliberately NO `device_class`: HA's
device_class "temperature" only accepts a plain temperature unit, so a rate
would have to lie to get one. That is the same shape the "%" sensors use.

Topic separation: everything the roaster owns is under `coffee_roaster/` plus
its own `homeassistant/` configs, so it cannot collide with anything else
publishing to the same broker.

Verified from a workstation (2026-09-26, read-only, nothing published): the
broker accepts the credentials in include/secrets.h (CONNACK Success).
Nothing from the roaster exists in HA yet: the firmware has never been
flashed and the hardware is not assembled (MAX6675 still in
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
- **REQUIRES RUNTIME** MQTT reconnects every 5 s while the link is down.
  Brief dropouts are exactly what this is meant to ride out, so it has to be
  exercised in practice - it needs the device on a network, not a host test.

## Rate of Rise (implemented 2026-09-27)

`rorBt` / `rorEt` in `/api/status` and in the MQTT status payload, as two
read-only sensors in HA ("Bean temp rise" / "Environment temp rise", object ids
`ror_bt` / `ror_et`), and as two text cards in the status row of the web UI (no
chart). Unit C/min, one decimal, and a negative value passes through untouched
- cooling and the turning point are readings, not errors.

The estimate is a least-squares fit (linear regression), not a difference
between two readings: every 250 ms sample inside a sliding window of
`ROR_WINDOW_MS` (30 s) takes part, ~121 of them, and the fitted slope times
60000 is the rate. Fitting the whole series instead of differencing two
endpoints is deliberate - the 0.25 C ladder and the MAX6675's noise average
out along the fit instead of landing in the answer, which is what holds a
quantised reading to +-0.3 C/min rather than the full +-0.25 C/min a single
endpoint difference would cost. History is a 128-entry circular buffer in
src/ror.cpp, one entry per sample (32 s of them, always more than the window);
the fit is redone on every sample, under the state lock, and published like
every other value. Below `ROR_MIN_SPAN_MS` (10 s) of history nothing is
published (reports 0); between 10 s and a full window the fit runs over the
span that exists, which is already a correct C/min because the slope is per
millisecond.

- **CLOSED 2026-09-27 (host-tested)** The estimate: `test_ror` drives the module
  at the real 250 ms cadence - constant ramps hold to +-0.1 C/min over several
  windows (which wraps the ring), cooling keeps its sign, a 0.25 C-quantised
  signal stays inside the documented +-0.3 C/min for a 30 s fit (worst
  observed: 0.000 at 10 C/min, 0.100 on a slow 2 C/min ramp, and the test
  proves the simulated ladder really truncates, up to 0.208 C off the ideal
  line), warm-up measures over the span it has (20 s gives the rate, 9.75 s
  gives 0, 10 s - exactly ROR_MIN_SPAN_MS - gives the rate, 5 s gives 0), a
  frozen sensor reads under 0.1 C/min, and NaN samples are left out of the fit
  rather than fitted as zero. `test_control` also ramps both probes at 60
  C/min through the real `loop()` and sees 60 C/min out.
- **OPEN, decided at the first test roast** The window length. 30 s is the chosen value
  (all samples go into the fit); 15 s was tried as the alternative at the
  first test roast and stays on the table if the data argues for it. Revisit
  with real numbers: compare against Artisan on the same roast and adjust
  `ROR_WINDOW_MS` / `ROR_MIN_SPAN_MS` in include/config.h.
- **REQUIRES HARDWARE** The two HA entities appearing at all, with unit C/min,
  no device_class and the build language's name. Nothing here has run against a
  broker yet - no OTA, no live MQTT (the device is not connected).

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

- **INLAGD 2026-10-03 - ET-offset mot BT:** mätning 2026-10-03, båda proberna
  intill varandra i rumstemperatur, elementet av. BT läste 24,25-24,75 C
  (median 24,75), ET läste 29,5-32 C (median 30,75). Median-skillnad ET-BT =
  6,0 C. Offseten `SENSOR_OFFSET_ET_C -6.0f` är inlagd i include/config.h och
  tillämpas i src/sensors.cpp efter plausibilitetskontrollen. BT lämnas
  oförändrad (`SENSOR_OFFSET_BT_C 0.0f`). Orsaken till ET:s avvikelse är inte
  utredd (kandidater: modulens cold-junction, klonchip, probe/placering).
  Offseten är bara giltig kring rumstemperatur och måste omprövas vid
  rostningstemperatur. Den gamla mätningen 2026-09-27 (+1,2 C) gjordes inte på
  samma hårdvara/koppling och stämmer inte med dagens värden - använd inte båda
  samtidigt.

- **INLAGD 2026-10-04 - ET-offset ned till -6,5 C:** live-mätning i
  rumstemperatur/idle, 2-min-medelvärden ur Home Assistant, visade ET ~24,7 C
  mot BT ~24,2 C - ET låg alltså 0,5 C över BT efter den då gällande -6,0 C.
  Mätningen ovan (2026-10-03) satte ET i nivå med BT vid 6,0 C råskillnad; den
  här mätningen visar att 0,5 C återstod, så `SENSOR_OFFSET_ET_C` är nu -6.5f
  (BT oförändrad 0,0f). Mätningen 2026-10-03 står kvar som historik men är
  inaktuell som siffra - använd bara -6,5 C.
  Fortfarande bara verifierad kring rumstemperatur: ingen referensmätning vid
  rostningstemperatur finns, så vid 200-260 C är offseten en gissning.
  Sidoeffekt att känna till: BT/ET-korsskontrollen jämför kalibrerade värden,
  så en ET-offset på -6,5 C flyttar larmgränsen i råa tal med 0,5 C (ett rått
  par måste nu skilja > 21,5 C, mot > 21,0 C med -6,0 C, för att slå
  `SENSOR_MAX_SPREAD_C`). Det är inneboende i offseten, inte en bugg, och
  host-testet `test_sensors` täcker båda riktningarna.

## Still open, not done

Tagged the same way as above: what it takes, not just what is left.

- **ASSIGNED, REQUIRES HARDWARE CONFIRMATION** GPIO pins in include/config.h.
  These are no longer TBD placeholders - the current assignment is CLK 18,
  SO-BT 19, SO-ET 21, CS-BT 4, CS-ET 17, SSR 26, fan PWM 27, written down and
  cross-checked against the code in docs/wiring.md (revised 2026-10-03 for the
  custom board: only the clock is shared, each module has its own SO and CS,
  and CS-BT moved off the strapping pin GPIO5 to GPIO4 via GPIO13). What has *not*
  happened is the physical check: the board does not exist yet. Re-check on
  wiring that nothing sits on a strapping pin (0, 2, 5, 12, 15) - GPIO5 and
  GPIO2 are deliberately unused now, so the old GPIO5-high-at-reset question
  is gone.
- **REQUIRES HARDWARE, now with a measured reason** PID values
  (PID_KP/PID_KI/PID_KD in include/config.h): **2.0 / 0.15 / 0.0 as of 0.11.1**,
  a starting point and not a tuned set. What they replace (4.0 / 0.05 / 2.0) was
  never tested against hardware either, and its ~20 C offset turned out to be
  the integral's clamp (C*s clamped against the 0-100 % output range) rather
  than the gain - see "PID and the control input filter" above. KD 0 is settled
  (a derivative on a 250 ms, 0.25 C-quantised signal is a switch); what is still
  unmeasured is the real thermal response, so trim KP, then KI, at the next
  roast, watching `pidP`/`pidI`/`pidIntegral` in `/api/status` while it runs.
- **KNOWN LIMITATION, accepted on purpose** The web API has no authentication:
  access control is whatever the network around the device provides. Closing it
  means a token or basic-auth check on every handler in src/web_server.cpp plus
  a login page in data/index.html - doable without hardware, deliberately not
  done. MQTT is the same: access control there belongs to the broker.
- **CLOSED 2026-09-26 (host-tested)** WiFi connection is
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
- **CLOSED 2026-09-26 (host-tested + ThreadSanitizer)**
  Concurrency: ESPAsyncWebServer callbacks and `loop()` are separated by one
  recursive mutex, see the Concurrency section above. The residual note is
  about MQTT: `mqtt_update()` stays outside the lock on purpose because it can
  block for seconds - see the known limitation above for what that costs.
- **CLOSED 2026-10-03 (host-tested)** OTA: `tools/ota-push.sh` POSTs the image
  from the uploading machine to the roaster, so no connection has to come back
  out of the IoT network. Token from `secrets.h`, refused unless the machine is
  idle, restart after the response, NVS and profiles untouched. ArduinoOTA
  remains in the firmware as a second option. See the OTA section above.
  Open only as far as reachability goes: that depends on the network the
  roaster is installed on.

## Host-side tests

`tools/host-tests/run.sh` compiles the firmware logic against stubbed Arduino/
WiFi/PubSubClient/ArduinoOTA headers and runs it on the host - no ESP32 and no
broker. It runs two passes, one per build mode of `SAFETY_ET_ENABLED` (see
"BT-only build mode" above); `--et-enabled` or `--bt-only` runs a single one.
Last run 2026-10-19 (0.12.0, stop-and-cool and the per-profile cool-down):
**1323 checks, 0 failures** with the flag at 1 and **1191 checks, 0
failures** with it at 0, exit 0 both times. (The run before it, on 0.11.1 -
the PID clamp, KD 0, the input filter and the fan ramp - read 1201 / 1069.)
The only compiler warnings are the
pre-existing `-Wunused-parameter` ones from `web_server.cpp`'s handler
signatures (they mirror the library's, so they are compiled with
`-Wno-unused-parameter` in the builds that use them); nothing from the files
this change touched. The totals count every execution:

- flag 1, 14 binaries: 923 checks counted once each, plus the two the suite runs
  twice - `test_mqtt_discovery` once per build language (233 again) and
  `test_control` under ThreadSanitizer (167 again).
- flag 0, 9 binaries: 787 checks counted once each, plus `test_mqtt_discovery`
  (233 again) and `test_control` under ThreadSanitizer (171 again). `test_safety`
  is flag-1 only (it asserts exactly the ET trips the flag switches off), and
  `test_ror`, `test_fan`, `test_fan_fallback`, `test_fan_immediate` and
  `test_pid` link neither safety.cpp nor sensors.cpp, so the flag cannot reach
  them - they run once, in the default pass. `test_ota_push` and
  `test_ota_push_disabled` are flag-independent too,
  but run in BOTH passes on purpose: the endpoint answers the same in either
  build mode, and a mode-dependent endpoint would be a trap.

- `test_safety` - safety latch, heater interlock and the stuck-probe detector
  (57 checks, default pass only): trip on the hard limit, on sustained sensor
  faults and on BT/ET disagreement while cold; commands refused while latched;
  clear only after the healthy streak. Plus persistence: a latch survives a
  reboot with its reason intact, a corrupted or unreadable NVS record is
  repaired rather than obeyed, and the degraded RAM-only path still trips. Plus
  the stuck probe: idle frozen probes never trip, moving probes under heat never trip,
  the exact window boundary trips on the right sample, a stalled reading while
  cooling never trips, a frozen channel trips while the other one moves, and a
  stuck alarm survives a power cycle without being laundered by healthy-looking
  samples.
- `test_safety_bt_only` - the BT-only build (39 checks, flag-0 pass only): no ET
  reading can latch anything - missing, faulted for far longer than any streak,
  above the 300 C ET limit, 0 C from a floating data line, or frozen for the
  whole stuck window with the element asking for power - and the cold BT/ET
  disagreement that trips a full build is not evaluated at all. BT still carries
  the alarm: a never-connected BT trips on the 5th faulted sample, 260 C trips
  the BT ceiling, a frozen BT under heat trips the stuck detector, and
  `heater_set_duty(100)` cannot get past the latch. The clear streak counts BT:
  nine healthy samples are not enough, the tenth clears it while ET stays dead,
  and an ET fault arriving afterwards cannot re-latch it. Plus the NVS cases that
  matter when this image is flashed over a latched machine: a stored ET sensor
  alarm and a stored stuck-ET alarm are both restored, report their own reason,
  and are released by the BT streak - the stuck one must *not* wait for an ET
  value that can never arrive, which would lock the heater out for good.
- `test_sensors` - the sensor layer (18 checks with the flag at 1, 14 with it at
  0): the offset is applied exactly once and only *after* the plausibility check,
  so a floating or open probe is still a fault and a faulted channel returns its
  last good value with the offset **not** applied. The same BT behaviour is
  asserted in both modes - a 0 C BT is a fault, a step above
  `SENSOR_FAULT_MAX_JUMP_C` is a fault, and the next plausible value is accepted
  again. The ET half is per mode: with the flag at 1 the ET offset and the
  cross-check preconditions are pinned from the constants rather than a pinned
  measurement (a raw pair separated by exactly the fitted ET-BT difference
  calibrates onto one temperature, and a raw disagreement far enough above
  `SENSOR_MAX_SPREAD_C` still trips after the offset was trimmed to -6.5 C); with
  it at 0 the ET channel carries no value and no fault whatever the module does,
  and the stub's read counter proves the converter is not read at all.
- `test_ror` - rate of rise (22 checks): a constant 10 C/min and 6.5 C/min ramp
  holds its value to +-0.1 C/min across several windows (which wraps the
  128-entry sample ring); cooling keeps its sign (-5 stays -5); a signal
  quantised to the MAX6675's 0.25 C ladder stays inside the documented
  +-0.3 C/min for the 30 s fit - worst observed 0.000 at 10 C/min and 0.100 on
  a slow 2 C/min ramp, and the test first proves the simulated ladder really
  truncates (0.208 C off the ideal line) so the tolerance means something;
  warm-up measures across the span it has (20 s of history gives the right
  rate, 9.75 s gives 0, 10 s - exactly ROR_MIN_SPAN_MS - gives the rate, 5 s
  gives 0); a frozen sensor reads below 0.1 C/min after a window; and NaN
  samples are left out of the fit instead of being fitted as zero.
- `test_fan` - the fan duty curve and the ramp to it (29 checks in the default
  build, 28 in the fallback one, 24 with the ramp disabled):
  `fan_set_speed()` keeps its percent API while the duty written to the LEDC
  channel is mapped onto the band the fan actually responds in. 0 is off
  (duty 0); 1 % lands on the band floor (60 % duty with the default band, the
  measured start threshold, and above the 10 % interlock threshold); 50 % and
  100 % land at the configured midpoint and top; 101 clips and a negative
  request is off; the duty is non-decreasing across the whole request range;
  and every nonzero request stays at or above the floor. Then the ramp
  (`FAN_SLEW_PCT_PER_SEC`): a request of 100 % writes nothing by itself, the
  reached speed walks to it at the configured rate, it is monotone in both the
  speed and the duty, 0 → 100 % takes the time the rate says, and one control
  sample in the fan is still below `FAN_MIN_FOR_HEATER_PCT` - the state
  `test_control` uses to check that the heat is refused. A decrease is written
  immediately, which is what keeps the interlock able to cut the element in the
  cycle the fan is asked to stop. The file is built twice more: with a broken
  band (`FAN_DUTY_MIN_PCT == FAN_DUTY_MAX_PCT`, the straight-map fallback - a
  misconfigured constant can never freeze or invert the fan) and with
  `FAN_SLEW_PCT_PER_SEC` 0 (the escape hatch that restores the old immediate
  write). The regression that matters is elsewhere: the fan interlock tests in
  `test_control` still speak the requested percent through `/api/status` and
  still pass unchanged, which is what guarantees the reporting side did not
  move.
- `test_pid` - the PID and the control-input filter (20 checks, default pass
  only, links no other translation unit): a first-order plant with a real load
  settles at zero steady-state error with the I term holding the duty, while the
  same signal through a P-only loop parks ~18 C under the setpoint (the measured
  failure, committed as a regression); `KI * integral` is clamped to the output
  range and the stored state follows the clamp (the old clamp on the raw C*s
  state would read 1000 % here); the integral does not grow while the output is
  saturated, nothing is stored to overshoot with on arrival, and a saturated
  state still integrates when the error turns back inside; a +-10 C dither on
  the input makes the OLD gains and derivative swing the duty over 80 points
  sample to sample while KD 0 plus the filter holds it inside ~6 points; KD 0
  contributes exactly nothing; and the filter itself seeds from its first
  sample, covers ~63 % of a step in one time constant and takes the dither down
  about 8x. The safety half of the same story - that the latch still trips on a
  jump the filter would smooth - is in `test_control`, because it only exists in
  the path that runs `loop()`.
- `test_mqtt_discovery` - MQTT layer (233 checks): all 12 discovery configs are
  valid JSON with unique_id, device block and availability; the status payload
  carries the expected fields (including `fanFault`, `etDisabled` and the signed
  `rorBt` / `rorEt`); the ET-disabled entity is a `binary_sensor` with
  device_class problem driven by `value_json.etDisabled`, published by both
  builds so nothing appears or disappears when the image changes; the two
  rate-of-rise sensors carry unit C/min and state_class
  measurement with no device_class; every payload fits the PubSubClient buffer
  (largest 648 B against the 900 B limit); and the report-only guarantees hold
  - no subscriptions, no message callback, no `command_topic` on any entity, no
  controllable entity types, and every published topic under `coffee_roaster/`
  or `homeassistant/`.
- `test_web_server` - the profile HTTP API (88 checks): the real handlers,
  dispatched through a stubbed ESPAsyncWebServer onto an in-memory LittleFS
  that logs every operation. `name=../config` and `name=../../etc/passwd` over
  GET and DELETE answer 400 and provably cause **no** filesystem call at all,
  with a honeytoken file outside `/profiles` proving the traversal would
  otherwise have found something. It also asserts that `/api/status` reports
  `etDisabled` matching the mode it was built in, since that field is the only
  thing that separates a dead ET module from a channel that is not watched -
  `rorSource`, which has to follow the build mode the same way - and the PID
  diagnostics (`pidP`, `pidI`, `pidD`, `pidIntegral`, `btFiltered`) and
  `fanActual`, each with a value of its own so a field wired to the wrong getter
  cannot pass by accident. Since 0.12.0 it also covers the cooling contract:
  `POST /api/roast/stop-and-cool` is registered, is not shadowed by
  `/api/roast/stop`, hands the control layer exactly one call with the speed and
  the time in seconds, answers 400 to the six out-of-range/absent/garbage bodies
  **without reaching that call at all**, answers 409 when the control layer
  refuses, and the profile's `coolFan`/`coolMinutes` round-trip through a real
  file, are clamped to the limits, and leave a profile without them
  byte-for-byte unchanged.
- `test_control` - the real src/main.cpp against stubbed hardware (167 checks
  with the flag at 1, 171 with it at 0):
  the bench case (probes disconnected, 0 C on every channel) trips the latch
  and denies manual start; `heater_set_duty(100)` cannot get past a held alarm;
  the fan interlock in manual *and* profile mode, both directions - including
  **while the fan ramps**: one sample after a 0 → 100 % request the fan is at
  ~6 %, and the heat is refused (judging on the requested percent instead is how
  the element would be lit while the fan spins up); a step both probes make
  together moves the raw reading at once while the filtered control input lags,
  and a 21 C jump still trips the latch - the filter is in the control path, not
  in `sensors_read()`; a probe
  pulled mid-run aborts the running heat; and the MQTT status payload carries
  both fault flags. This is the test that would have caught the 0 C bug. On
  top of that: booting with WiFi down returns from `setup()` promptly, still
  trips the safety latch, and retries the connect after
  `WIFI_RETRY_INTERVAL_MS`; the web callbacks provably take the state lock
  (acquisition counter, including the rate-of-rise, fan-actual, PID and
  filtered-temperature getters); and a second
  thread plays the AsyncTCP task against `loop()` for 20 s of simulated control
  time, checking that nothing deadlocks and that the state it leaves behind
  still makes sense. Then the rate of rise end to end: both probes ramped at
  exactly 60 C/min for 62 s of real `loop()` time, reported as 60 C/min. And
  the OTA path:
  it starts exactly once when the link comes up with the configured hostname,
  port and password, is served while idle, is *not* served while a run is
  active, latches the element low on `onStart()` while a manual heat is
  conducting (and aborts that run), and asks for a restart on `onEnd()` and
  on `onError()`. The filesystem path is covered here too: `U_SPIFFS` unmounts
  LittleFS before the image lands (and really calls `end()`, not just a flag),
  `U_FLASH` leaves it mounted, the profile endpoints answer 503 while it is
  unmounted, and a failed transfer remounts before the restart. That is the
  regression test for the bug that made filesystem OTA die part-way. Since
  0.12.0 it also covers the cooling end to end: stop-and-cool is **one** state
  lock acquisition with the fan re-commanded before the call returns and never
  0 % on any later sample; the same command refuses 0 % / 0 s and leaves the
  roast running; plain Stop still starts no cooling; a 5 s profile carrying
  `coolFan`/`coolMinutes` ends by itself and hands over to the cool timer; the
  0/0 and half-filled profiles end with the heat off and the fan stopped and no
  cooling; a manual Stop mid-profile starts none, then or later; and the pause
  is unchanged - clock frozen, heat holding the step, no end and no cooling
  while paused, both once it resumes.
- `test_ota_push` - the two push OTA routes (163 checks, built and run in both
  passes): the real `POST /api/update` and `POST /api/update/fs` handlers and
  the real state machine, against a stubbed `Update` that records every call -
  including the command, so `U_FLASH` and `U_SPIFFS` are distinguishable - and
  an in-memory LittleFS that logs every operation. Dispatched with every chunk
  of the body the way the library really does. A valid token from an address
  other than `OTA_ALLOWED_CLIENT_IP` is refused with 404, and a wrong, missing
  or short token and a malformed or missing checksum header likewise - always
  *before* `Update.begin()`, so no byte of an unauthorised image is written, no
  file is touched and nothing is unmounted, and never as a 500 when the
  remaining chunks arrive. A run in progress *or a live element* is refused with
  409 with `Update.begin()` never reached, and so is a second transfer while one
  is being written. A missing Content-Length is refused with 400; an image
  larger than the OTA slot (app route) or than the filesystem partition (fs
  route) with 413; a body too small to be a filesystem with 400. An image whose
  SHA-256 does not match is refused with 403 and discarded with `Update.abort()`,
  so the running image stays the boot target - the rollback; a half-sent image
  aborts the same way. A complete transfer writes exactly `Content-Length` bytes
  in order, ends with `Update.end()` (never `abort()`), reads back
  byte-identical, leaves a stored profile in LittleFS untouched and only then
  asks for the restart. A failed `Update.begin()` or `Update.end()` answers 500.
  The filesystem route repeats that matrix and adds: which handler the device
  would pick for `/api/update/fs` (route order, see the OTA section),
  `U_SPIFFS` as the command, LittleFS unmounted when the first byte lands (with
  `end:/` in the operation log, not just a flag), the profile endpoints answering
  503 during the transfer and 200 again afterwards, and a remount after a
  rejected transfer, a wrong checksum and a dropped connection.
- `test_ota_push_disabled` - the same routes with NO token configured
  (21 checks, built and run in both passes): both routes answer 503 to a
  well-formed, authenticated POST, open no partition, write no byte and touch no
  file; both GET probes answer 404. The build a device without a token runs.
- `test_ror_guidance` - optional rate-of-rise guidance (53 checks with the flag
  at 1, 58 with it at 0): the target
  curve, the sign of the correction, a zero correction when disabled, the rate
  limit and clamp that keep a cold-probe transient under `HEATER_MAX_DUTY_PCT`,
  every duty request routed through `applyHeaterDuty()`, the off-by-default
  regression when a profile carries no `rorTarget`, and that the MQTT status
  payload names the channel the guidance steers on. Built in both passes, one
  source per pass: with the flag at 1 it steers on `rorEt` (the behaviour that
  has shipped since 0.7.0, asserted unchanged); with it at 0 on `rorBt`, with
  the ET channel reading nothing for the whole run - where the BT transient case
  also pins the rate limit per cycle (the correction moves at most
  `ROR_GUIDANCE_MAX_STEP_PCT_PER_S` per second, the duty never passes
  `HEATER_MAX_DUTY_PCT`, and the clamp really is reached: the peak is 100.00 %
  with the fit reading -50 C/min).
- **ThreadSanitizer pass**: the `test_control` source is compiled a second
  time with `-fsanitize=thread` and run as part of the suite. That is what
  found the profile-name race (a `char*` handed to the MQTT payload builder
  outside the lock), which is fixed. It runs clean now. It needs
  `setarch -R` on this kernel (TSAN cannot map its shadow under the current
  ASLR entropy) - `run.sh` handles that, and only the address layout
  changes, not the threads.

The MQTT test needs an enabled config: without `include/secrets.h`, `run.sh`
passes throwaway `-DMQTT_HOST/-DMQTT_USER/-DMQTT_PASSWORD` values to both
translation units (macros do not cross translation units). The PubSubClient stub
refuses anonymous connects, so the credential path is exercised too.
