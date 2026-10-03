# Firmware notes - assumptions and what is verified

Status: **verified items below were re-confirmed on 2026-10-03** with a clean
`pio run` (zero warnings) and the host-side test suite (869 checks, 0
failures). Push-OTA and filesystem OTA are both verified against real hardware.
Everything still listed as open is untested against real hardware.

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
`test_mqtt_discovery` (161 checks) and `test_control` (75 checks, which drives
the real setup()/loop() and also runs a second thread in the role of the
AsyncTCP task). 293 checks, 0 failures, as of the 2026-09-27 run of
`tools/host-tests/run.sh`, plus the same `test_control` source rebuilt under
ThreadSanitizer (also 75/0, zero race reports). See `tools/host-tests/`.

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

**Default path now: push. `sh tools/ota-push.sh <ip>`.**

```sh
pio run                        # build
sh tools/ota-push.sh 192.168.x.x
```

The uploading machine connects **to** the roaster and POSTs `firmware.bin` to
`/api/update`, token in the `X-OTA-Token` header. That direction is the whole
point: the ArduinoOTA path described below needs the *device* to open a TCP
connection back to the uploader, and in a VLAN-split home network new
connections IoT -> LAN are blocked, so that handshake dies right after
`Authenticating...OK` while the device is up and reachable. LAN -> IoT works,
so the upload was turned around instead of the firewall being opened for it.
A permanent rule for a port that only matters during an upload - where the
espota listener is also unauthenticated, first connection wins - is the wrong
trade.

Rules around the endpoint (`include/ota_push.h`, `src/ota_push.cpp`):

- **Only the allowed client.** `OTA_ALLOWED_CLIENT_IP` in `include/secrets.h`
  is the one address whose requests are even considered. A valid token is not
  enough: port 80 is the device's normal web server and IoT devices reach each
  other inside the IoT VLAN, so the network does not isolate this route. A
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
  and LittleFS are not part of that, so saved profiles survive a push. The
  host test asserts exactly that against the real handler.

### Filesystem OTA: the command decides, and LittleFS must be unmounted

`sh tools/ota-upload.sh <ip> fs` sends `littlefs.bin` to the filesystem
partition with `--spiffs`. This was broken, and the failure looked like a
network problem: the transfer started, ran for ~190 chunks, and died with
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
  `"-P 32320 -I 192.168.1.x"` *inside* the `--auth` value: espota answers
  `Authenticating...FAIL`.
- **One flag per line** (the form that gives separate arguments) gives every
  value a **leading space**, so `host_ip` becomes `" 192.168.1.x"` and espota
  dies on `[ERROR]: Listen Failed` before the transfer even starts. That
  message reads like a network problem; it is a stray blank.

There is no `upload_command` key in PlatformIO, `extra_scripts` set on the env
overwrite the one inherited from `extends`, and `UPLOADERFLAGS` is re-set by
`builder/main.py` after every script hook has run - so no script can clean it
up reliably. `tools/ota-upload.sh` therefore **calls `espota.py` directly**
with the arguments as a list: no middleman, no space that can be added.
`-I 192.168.1.x` (the server's LAN address) and `-P 32320` are both still
required, and the password is read from `secrets.h` as before.

`platformio.ini` keeps its `upload_flags` for anyone who runs `pio run -e
esp32-ota -t upload` by hand, with the trap documented next to it.

### Proving a push landed: the build identity in `/api/status`

`FW_VERSION` says which *release* is running. It cannot say which *build* of
it, and that is the question after a push: did the image I just sent actually
take, or did the endpoint answer 200 for something else? A version number that
did not change answers nothing.

So `tools/fw_build_id.py` runs as a PlatformIO `pre` script, stamps the git sha
and the build time into `build_flags`, and `/api/status` reports them:

```sh
curl -s http://192.168.x.x/api/status | python3 -m json.tool | grep -E 'fw|build|built'
#   "fw":    "0.7.0"                    <- same string HA shows as sw_version
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

**Verified on hardware 2026-10-03** against the roaster at 192.168.2.x:

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
4. **`-I 192.168.1.x`** is in the command as **hygiene, not as a cause**: it
   makes the listener unambiguous in an LXC with virtual interfaces. Measured
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

**What does *not* travel with a firmware update: `data/`.** `ota-upload.sh`
sends `firmware.bin` to the OTA slot and nothing else - the web UI lives in
the LittleFS image on its own partition, so a UI change needs its own push.
That push does not need USB either: `sh tools/ota-upload.sh <ip> fs` runs
`pio run -e esp32-ota -t uploadfsota`, which rebuilds the image from `data/`
and sends it with the same espota protocol using `--spiffs`. All three
layers carry it: PlatformIO adds the flag for that target
(`builder/main.py:460-461`), `espota.py` switches the command to SPIFFS
(`tools/espota.py:345-347`), and the ESP32 ArduinoOTA accepts `U_SPIFFS`
(`libraries/ArduinoOTA/src/ArduinoOTA.cpp:176`) - and the device runs a
build of exactly that framework version. Verified in code on all three
layers, **not yet run on hardware**: the first real UI change is where that
gets proven. `uploadfs` over USB remains the fallback, not the first hand.

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

Entities published in discovery (11 configs, all read-only, all under
`homeassistant/<component>/coffee_roaster_<object>/config`): bean temperature,
environment temperature, rate of rise for bean and for environment
temperature, heater duty, fan speed, mode, elapsed seconds and profile
(sensors), plus the safety alarm and the fan interlock (binary_sensors, both
device_class problem). Every config is retained and carries the device block,
unique_id and availability topic.

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
- **REQUIRES HARDWARE** PID values (PID_KP/KI/KD in config.h): unguessed
  starting values. Will need tuning against the real thermal response once the
  machine is testable.
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
  out of the IoT VLAN. Token from `secrets.h`, refused unless the machine is
  idle, restart after the response, NVS and profiles untouched. ArduinoOTA
  remains in the firmware as a second option. See the OTA section above.
  Open only as far as reachability goes: that depends on the network the
  roaster is installed on.

## Host-side tests

`tools/host-tests/run.sh` compiles the firmware logic against stubbed Arduino/
WiFi/PubSubClient/ArduinoOTA headers and runs it on the host - no ESP32 and no
broker. Last run 2026-10-03: **461 checks, 0 failures**, exit 0, no compiler
warnings. That figure counts each test once; 869 checks execute in total,
because `test_mqtt_discovery` runs once per build language and `test_control`
also runs under ThreadSanitizer:

- `test_safety` - safety latch, heater interlock and the stuck-probe detector
  (57 checks): trip on the hard limit, on sustained sensor faults and on
  BT/ET disagreement while cold; commands refused while latched; clear only
  after the healthy streak. Plus persistence: a latch survives a reboot with
  its reason intact, a corrupted or unreadable NVS record is repaired rather
  than obeyed, and the degraded RAM-only path still trips. Plus the stuck
  probe: idle frozen probes never trip, moving probes under heat never trip,
  the exact window boundary trips on the right sample, a stalled reading while
  cooling never trips, a frozen channel trips while the other one moves, and a
  stuck alarm survives a power cycle without being laundered by healthy-looking
  samples.
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
- `test_mqtt_discovery` - MQTT layer (210 checks): all 11 discovery configs are
  valid JSON with unique_id, device block and availability; the status payload
  carries the expected fields (including `fanFault` and the signed `rorBt` /
  `rorEt`); the two rate-of-rise sensors carry unit C/min and state_class
  measurement with no device_class; every payload fits the PubSubClient buffer
  (largest 648 B against the 900 B limit); and the report-only guarantees hold
  - no subscriptions, no message callback, no `command_topic` on any entity, no
  controllable entity types, and every published topic under `coffee_roaster/`
  or `homeassistant/`.
- `test_control` - the real src/main.cpp against stubbed hardware (79 checks):
  the bench case (probes disconnected, 0 C on every channel) trips the latch
  and denies manual start; `heater_set_duty(100)` cannot get past a held alarm;
  the fan interlock in manual *and* profile mode, both directions; a probe
  pulled mid-run aborts the running heat; and the MQTT status payload carries
  both fault flags. This is the test that would have caught the 0 C bug. On
  top of that: booting with WiFi down returns from `setup()` promptly, still
  trips the safety latch, and retries the connect after
  `WIFI_RETRY_INTERVAL_MS`; the web callbacks provably take the state lock
  (acquisition counter, including the rate-of-rise getters); and a second
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
  regression test for the bug that made filesystem OTA die part-way.
- `test_ota_push` - the push OTA endpoint (59 checks): the real
  `POST /api/update` handler and the real state machine, against a stubbed
  `Update` that records every call, dispatched with every chunk of the body the
  way the library really does. A valid token from an address other than
  `OTA_ALLOWED_CLIENT_IP` is refused with 404, and a wrong, missing or short
  token and a malformed or missing checksum header likewise - always *before*
  `Update.begin()`, so no byte of an unauthorised image is written and no file
  is touched, and never as a 500 when the remaining chunks arrive. A run in
  progress *or a live element* is refused with 409 with `Update.begin()` never
  reached. A missing Content-Length is refused with 400 and an image larger
  than the OTA slot with 413. An image whose SHA-256 does not match is refused
  with 403 and discarded with `Update.abort()`, so the running image stays the
  boot target - the rollback; a half-sent image aborts the same way. A complete
  transfer writes exactly `Content-Length` bytes in order, ends with
  `Update.end()` (never `abort()`), reads back byte-identical, leaves a stored
  profile in LittleFS untouched and only then asks for the restart. A failed
  `Update.begin()` or `Update.end()` answers 500.
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
