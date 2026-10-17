#!/bin/sh
# Host-side tests (no ESP32 and no PlatformIO toolchain needed - just g++, plus
# node for the web check which is skipped when node is absent).
#
# TWO PASSES, because the firmware has two build modes: SAFETY_ET_ENABLED 1
# (both probes, the full protection) and 0 (BT-only: the ET channel is not read
# at all and its trips and the cold BT/ET cross-check are switched off - see
# include/config.h). The flag decides which channels the safety latch watches,
# so every test that is meaningful in both modes is built once per mode and has
# to pass in both:
#
#   sh run.sh                both passes - the normal run
#   sh run.sh --et-enabled   only SAFETY_ET_ENABLED=1
#   sh run.sh --bt-only      only SAFETY_ET_ENABLED=0
#
#   check_web_i18n       the language table in data/index.html: both languages
#                        carry every key and nothing is hard-coded (node).
#                        Mode-independent, so it runs once.
#   test_safety          safety latch + heater interlock, stubbed Arduino.
#                        DEFAULT PASS ONLY: it asserts the ET trips (ET
#                        over-temp, an ET sensor fault, a frozen ET and the
#                        cold BT/ET cross-check), which the flag switches off by
#                        design - the file #errors when built with it at 0.
#   test_safety_bt_only  the BT-only build: no ET reading can latch anything,
#                        and a latch that BT sets clears on BT's streak even
#                        though ET stays dead. BT-ONLY PASS ONLY.
#   test_sensors         sensor offset + fault masking, and - per mode - that
#                        the ET converter is read (mode 1) or never read at all
#                        (mode 0, counted in the stub, because both report a
#                        null ET over the API and no value can tell them apart).
#   test_ror             rate of rise: window, warm-up, sign and the quantised
#                        tolerance, fed at the real 250 ms sample cadence.
#                        Links ror.cpp only: no flag.
#   test_fan             the fan duty curve: request 1-100 % mapped onto the
#                        band the fan responds in (and the straight-map
#                        fallback for a misconfigured band), via a recording
#                        ledcWrite stub. Built twice, one per band. No flag.
#   test_mqtt_discovery  MQTT discovery payloads + command handling, using a
#                        recording PubSubClient stub (no broker needed).
#                        Built twice - English and Swedish - and the unique_id
#                        dumps of the two are diffed, so a language switch can
#                        never move an entity in Home Assistant. The dumps are
#                        diffed across the two MODES as well, so a BT-only
#                        build cannot move an entity either.
#   test_web_server      the profile HTTP API: the real handlers dispatched
#                        through a stubbed ESPAsyncWebServer onto an in-memory
#                        LittleFS that logs every operation, so a refused
#                        name= traversal provably never reaches the filesystem.
#                        Also checks that /api/status reports the etDisabled
#                        field for the mode it was built in.
#   test_control         the real setup()/loop(), including a second thread in
#                        the role of the AsyncTCP task; the same source is
#                        built a second time under ThreadSanitizer. In the
#                        BT-only pass its bench case has to keep tripping, on
#                        BT's own checks, and a garbage ET reading has to reach
#                        nothing.
#   test_ror_guidance    optional RoR-guidance per profile step: curve, sign,
#                        clamps, interlock and the off-by-default regression.
#                        DEFAULT PASS ONLY: the guidance steers on rorEt, and a
#                        BT-only build has no ET reading to steer on (main.cpp
#                        refuses to steer on a NaN ET, so it is inert there -
#                        nothing to assert, and nothing that could steer).
#   test_ota_push        the push-OTA endpoints. Links web_server.cpp but drives
#                        only /api/update and /api/update/fs: it never links
#                        safety.cpp or sensors.cpp, so the flag cannot reach it,
#                        but it is built and run in BOTH passes anyway - an
#                        endpoint whose behaviour depended on the build mode
#                        would be a trap.
#   test_ota_push_disabled  the same sources with NO token configured: the
#                        build where both routes must answer 503 and open no
#                        partition at all. Both passes.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
json_inc="$root/.pio/libdeps/esp32dev/ArduinoJson/src"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Every binary that links ota_push.cpp (or main.cpp, which calls it) also needs
# the SHA-256 it hashes with. The host tests compile the real implementation
# from stub/sha256.cpp rather than stubbing it, so the checksum they exercise
# is a checksum that actually verifies images.
sha_src="$here/stub/sha256.cpp"

# Which passes to run: et = SAFETY_ET_ENABLED 1, bt = SAFETY_ET_ENABLED 0.
case "${1:-}" in
  ""|--both)     passes="et bt" ;;
  --et-enabled)  passes="et" ;;
  --bt-only)     passes="bt" ;;
  *) echo "usage: $0 [--both|--et-enabled|--bt-only]" >&2; exit 2 ;;
esac

# The MQTT test needs an "enabled" MQTT config to exercise the connection and
# discovery path. Without include/secrets.h we hand both translation units the
# same throwaway definitions (the stub never talks to anything). With a real
# secrets.h we pass nothing and the test picks up the real configuration.
mqtt_defs=""
if [ ! -f "$root/include/secrets.h" ]; then
  mqtt_defs='-DMQTT_HOST="192.168.0.x" -DMQTT_USER="host-test-user" -DMQTT_PASSWORD="host-test-pass"'
fi

if [ ! -d "$json_inc" ]; then
  echo "ArduinoJson not found at $json_inc - run 'pio run' once first" >&2
  exit 2
fi

# The web UI keeps its own translation table (a LittleFS file cannot include
# include/strings.h), so it gets its own guard: a key that exists in only one
# of the two languages, or a Swedish string hard-coded outside the table,
# fails here. Needs nothing but node, and is skipped when node is absent.
if command -v node >/dev/null 2>&1; then
  node "$here/check_web_i18n.js" "$root/data/index.html"
else
  echo "skip web UI i18n check (no node on this machine)"
fi

# Runs a test binary, shows its output and adds its tally to the pass log, so
# the run can end with the totals for the pass. The exit status is the binary's
# own - a failing test still stops the suite (set -e), pipe or no pipe.
run_test() {
  if "$@" > "$out/last_output" 2>&1; then
    cat "$out/last_output"
    cat "$out/last_output" >> "$out/pass_log"
  else
    cat "$out/last_output"
    exit 1
  fi
}

# One full pass. $1 is the pass name (et|bt); the flag and the output-name
# suffix follow from it, so the same source lines below build either mode.
run_pass() {
  pass="$1"
  case "$pass" in
    et) et_flag="-DSAFETY_ET_ENABLED=1"; sfx="" ;;
    bt) et_flag="-DSAFETY_ET_ENABLED=0"; sfx="_bt" ;;
  esac
  if [ "$pass" = "et" ]; then flag_value=1; else flag_value=0; fi

  echo ""
  echo "=== pass: SAFETY_ET_ENABLED=$flag_value ==="
  : > "$out/pass_log"

  # ------------------------------------------------------------ safety ------
  if [ "$pass" = "et" ]; then
    g++ -std=c++17 -Wall -Wextra \
        -I "$here/stub" -I "$root/include" \
        "$here/test_safety.cpp" "$root/src/safety.cpp" "$root/src/heater_control.cpp" \
        -o "$out/test_safety"
  else
    g++ -std=c++17 -Wall -Wextra \
        -I "$here/stub" -I "$root/include" \
        $et_flag \
        "$here/test_safety_bt_only.cpp" "$root/src/safety.cpp" "$root/src/heater_control.cpp" \
        -o "$out/test_safety_bt_only"
  fi

  # Sensor test: the offset must be applied exactly once, after the
  # plausibility check, so an open/floating probe stays a fault and calibrated
  # values reach the cross-check - and, per mode, whether the ET converter is
  # read at all.
  g++ -std=c++17 -Wall -Wextra \
      -I "$here/stub" -I "$root/include" \
      $et_flag \
      "$here/test_sensors.cpp" "$root/src/sensors.cpp" \
      -o "$out/test_sensors$sfx"

  # The real setup()/loop() with stubbed hardware, so the path the web UI takes
  # - manual mode -> PID -> heater - is the code that actually ships.
  # -pthread because the test runs a second thread in the role of the AsyncTCP
  # task against loop(), which is the concurrency case the state lock exists
  # for, and roast_profile.cpp is deliberately NOT linked - the test supplies
  # its own canned profile.
  g++ -std=c++17 -Wall -Wextra -pthread \
      -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs \
      -DOTA_TOKEN=\"host-test-push-token\" \
      $et_flag \
      "$here/test_control.cpp" "$root/src/main.cpp" "$root/src/safety.cpp" \
      "$root/src/sensors.cpp" "$root/src/heater_control.cpp" \
      "$root/src/fan_control.cpp" "$root/src/ror.cpp" "$root/src/mqtt_client.cpp" \
      "$root/src/web_server.cpp" "$root/src/ota_push.cpp" "$sha_src" \
      -o "$out/test_control$sfx"

  # Same test under ThreadSanitizer: it is the only way to see a shared field
  # that the lock forgot. Rides on the same binary source, so a race anywhere in
  # the loop-vs-web-callback path fails the suite.
  g++ -std=c++17 -Wall -Wextra -pthread -fsanitize=thread \
      -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs \
      -DOTA_TOKEN=\"host-test-push-token\" \
      $et_flag \
      "$here/test_control.cpp" "$root/src/main.cpp" "$root/src/safety.cpp" \
      "$root/src/sensors.cpp" "$root/src/heater_control.cpp" \
      "$root/src/fan_control.cpp" "$root/src/ror.cpp" "$root/src/mqtt_client.cpp" \
      "$root/src/web_server.cpp" "$root/src/ota_push.cpp" "$sha_src" \
      -o "$out/test_control_tsan$sfx"

  # The MQTT discovery test is built ONCE PER BUILD LANGUAGE (FW_LANG_EN): it
  # pins the friendly name of the language it was built with, and it dumps the
  # set of unique_ids. The two dumps are diffed further down - byte-identical
  # across languages is exactly the promise include/strings.h makes: a language
  # switch may move the pretty name, never where the entity lives.
  g++ -std=c++17 -Wall -Wextra \
      -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs $et_flag \
      "$here/test_mqtt_discovery.cpp" "$root/src/mqtt_client.cpp" \
      -o "$out/test_mqtt_en$sfx"
  g++ -std=c++17 -Wall -Wextra \
      -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs $et_flag -DFW_LANG_EN=0 \
      "$here/test_mqtt_discovery.cpp" "$root/src/mqtt_client.cpp" \
      -o "$out/test_mqtt_sv$sfx"

  # The profile-API test links the REAL web_server.cpp - and the real
  # roast_profile.cpp behind it - against a stubbed ESPAsyncWebServer and an
  # in-memory LittleFS, so the handlers run exactly as they ship. The LittleFS
  # stub logs every operation, which is the point: the path-traversal cases
  # (name=../config and friends) assert not just a 400 but that no filesystem
  # operation happened at all, with a honeytoken file outside /profiles proving
  # the traversal would otherwise have found something. -Wno-unused-parameter:
  # web_server.cpp's body handlers have to mirror the library's chunked-upload
  # signature (index/total) whether they use it or not, and -Wextra would print
  # that on every run without saying anything new.
  g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -I "$here/stub" -I "$root/include" -I "$json_inc" $et_flag \
      -DOTA_TOKEN=\"host-test-push-token\" \
      "$here/test_web_server.cpp" "$root/src/web_server.cpp" "$root/src/roast_profile.cpp" \
      "$root/src/ota_push.cpp" "$here/stub/sha256.cpp" \
      -o "$out/test_web_server$sfx"

  if [ "$pass" = "et" ]; then
    # The rate-of-rise test is pure maths over a stubbed clock - no Arduino
    # stub, no hardware: ror.cpp only includes include/config.h for its
    # constants.
    g++ -std=c++17 -Wall -Wextra \
        -I "$here/stub" -I "$root/include" \
        "$here/test_ror.cpp" "$root/src/ror.cpp" \
        -o "$out/test_ror"

    # The fan curve test is built twice: once with the configured duty band,
    # once with a deliberately broken band (min == max) so the straight-map
    # fallback is pinned too. Both use the same source - the band is
    # compile-time.
    g++ -std=c++17 -Wall -Wextra \
        -I "$here/stub" -I "$root/include" \
        "$here/test_fan.cpp" "$root/src/fan_control.cpp" \
        -o "$out/test_fan"
    g++ -std=c++17 -Wall -Wextra \
        -I "$here/stub" -I "$root/include" \
        -DFAN_DUTY_MIN_PCT=60 -DFAN_DUTY_MAX_PCT=60 \
        "$here/test_fan.cpp" "$root/src/fan_control.cpp" \
        -o "$out/test_fan_fallback"
  fi

  # The push-OTA test links the REAL web_server.cpp (and both handlers: the app
  # image and the filesystem image), the real ota_push.cpp, and a stubbed
  # Update library that records every call - including the command it was
  # given, so the test can tell U_FLASH from U_SPIFFS. OTA_TOKEN and
  # OTA_ALLOWED_CLIENT_IP are throwaway values so the endpoint is enabled
  # exactly as on a device with a token configured.
  #
  # Built in BOTH passes: it links neither safety.cpp nor sensors.cpp, so the
  # flag cannot reach it, but the same binary has to pass whichever mode the
  # image is built with - a mode-dependent endpoint would be a trap.
  g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -I "$here/stub" -I "$root/include" -I "$json_inc" $et_flag \
      -DOTA_TOKEN=\"host-test-push-token\" -DOTA_ALLOWED_CLIENT_IP=\"192.0.2.10\" \
      "$here/test_ota_push.cpp" "$root/src/web_server.cpp" "$root/src/ota_push.cpp" \
      "$root/src/roast_profile.cpp" "$sha_src" \
      -o "$out/test_ota_push$sfx"

  # The same endpoint with NO token configured: the build where it has to
  # refuse everything with 503 and never open a partition at all. The token is
  # a compile-time constant, hence a second binary.
  g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -I "$here/stub" -I "$root/include" -I "$json_inc" $et_flag \
      -DOTA_TOKEN=\"\" -DOTA_ALLOWED_CLIENT_IP=\"192.0.2.10\" \
      "$here/test_ota_push_disabled.cpp" "$root/src/web_server.cpp" "$root/src/ota_push.cpp" \
      "$root/src/roast_profile.cpp" "$sha_src" \
      -o "$out/test_ota_push_disabled$sfx"

  if [ "$pass" = "et" ]; then
    # The RoR-guidance test links the real main.cpp and roast_profile.cpp, so
    # the profile curve and guidance logic run exactly as they ship.
    g++ -std=c++17 -Wall -Wextra -pthread \
        -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs \
        -DOTA_TOKEN=\"host-test-push-token\" \
        "$here/test_ror_guidance.cpp" "$root/src/main.cpp" "$root/src/safety.cpp" \
        "$root/src/roast_profile.cpp" "$root/src/sensors.cpp" "$root/src/heater_control.cpp" \
        "$root/src/fan_control.cpp" "$root/src/ror.cpp" "$root/src/mqtt_client.cpp" \
        "$root/src/web_server.cpp" "$root/src/ota_push.cpp" "$sha_src" \
        -o "$out/test_ror_guidance"
  fi

  # ---------------------------------------------------------------- run -----
  if [ "$pass" = "et" ]; then
    run_test "$out/test_safety"
  else
    run_test "$out/test_safety_bt_only"
  fi
  run_test "$out/test_sensors$sfx"
  run_test "$out/test_mqtt_en$sfx" "$out/unique_ids_en$sfx"
  run_test "$out/test_mqtt_sv$sfx" "$out/unique_ids_sv$sfx"
  # The whole point of the two builds above: the same entities, in the same
  # place, whichever language the firmware was built with.
  if diff -u "$out/unique_ids_en$sfx" "$out/unique_ids_sv$sfx"; then
    echo "ok   unique_ids are identical in both build languages"
  else
    echo "FAIL unique_ids differ between build languages - a language switch" >&2
    echo "     would move existing entities in Home Assistant" >&2
    exit 1
  fi
  run_test "$out/test_web_server$sfx"
  run_test "$out/test_ota_push$sfx"
  run_test "$out/test_ota_push_disabled$sfx"
  run_test "$out/test_control$sfx"
  # ThreadSanitizer cannot map its shadow under this kernel's ASLR entropy (it
  # aborts with "unexpected memory mapping"), so the sanitized binary runs with
  # address randomisation turned off for that one process. That only changes
  # the address layout - the threads, the interleavings and the report are the
  # same, which is the whole point of the run.
  if command -v setarch >/dev/null 2>&1; then
    run_test setarch "$(uname -m)" -R "$out/test_control_tsan$sfx"
  else
    run_test "$out/test_control_tsan$sfx"
  fi

  if [ "$pass" = "et" ]; then
    run_test "$out/test_ror"
    run_test "$out/test_fan"
    run_test "$out/test_fan_fallback"
    run_test "$out/test_ror_guidance"
  fi

  # Every binary prints its own "N checks, M failures" line; the tally over the
  # pass is what goes into docs/firmware-notes.md.
  awk -F'[ ,]+' -v flag="$flag_value" \
      '/^[0-9]+ checks, [0-9]+ failures$/ {c += $1; f += $3}
       END {printf "pass SAFETY_ET_ENABLED=%s: %d checks, %d failures\n", flag, c, f}' \
      "$out/pass_log"
}

for p in $passes; do
  run_pass "$p"
done

# The entity set may also not move when the BUILD MODE changes: the BT-only
# image has to look like the full one in Home Assistant, one entity more and
# nothing relocated - an entity that only exists in one of the two modes would
# silently vanish (or appear) when the other image is flashed.
if [ -f "$out/unique_ids_en" ] && [ -f "$out/unique_ids_en_bt" ]; then
  if diff -u "$out/unique_ids_en" "$out/unique_ids_en_bt"; then
    echo "ok   unique_ids are identical in both build modes"
  else
    echo "FAIL unique_ids differ between build modes - flashing one image over" >&2
    echo "     the other would move an entity in Home Assistant" >&2
    exit 1
  fi
fi
