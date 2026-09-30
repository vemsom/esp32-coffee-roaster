#!/bin/sh
# Host-side tests (no ESP32 and no PlatformIO toolchain needed - just g++, plus
# node for the web check which is skipped when node is absent):
#   check_web_i18n       the language table in data/index.html: both languages
#                         carry every key and nothing is hard-coded (node)
#   test_safety          safety latch + heater interlock, stubbed Arduino
#   test_ror             rate of rise: window, warm-up, sign and the quantised
#                         tolerance, fed at the real 250 ms sample cadence
#   test_mqtt_discovery  MQTT discovery payloads + command handling, using a
#                        recording PubSubClient stub (no broker needed).
#                        Built twice - English and Swedish - and the unique_id
#                        dumps of the two are diffed, so a language switch can
#                        never move an entity in Home Assistant.
#   test_web_server      the profile HTTP API: the real handlers dispatched
#                        through a stubbed ESPAsyncWebServer onto an in-memory
#                        LittleFS that logs every operation, so a refused
#                        name= traversal provably never reaches the filesystem
#   test_control         the real setup()/loop(), including a second thread in
#                        the role of the AsyncTCP task; the same source is
#                        built a second time under ThreadSanitizer
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
json_inc="$root/.pio/libdeps/esp32dev/ArduinoJson/src"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

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

g++ -std=c++17 -Wall -Wextra \
    -I "$here/stub" -I "$root/include" \
    "$here/test_safety.cpp" "$root/src/safety.cpp" "$root/src/heater_control.cpp" \
    -o "$out/test_safety"

# The rate-of-rise test is pure maths over a stubbed clock - no Arduino stub,
# no hardware: ror.cpp only includes include/config.h for its constants.
g++ -std=c++17 -Wall -Wextra \
    -I "$here/stub" -I "$root/include" \
    "$here/test_ror.cpp" "$root/src/ror.cpp" \
    -o "$out/test_ror"

# The MQTT discovery test is built ONCE PER BUILD LANGUAGE (FW_LANG_EN): it
# pins the friendly name of the language it was built with, and it dumps the
# set of unique_ids. The two dumps are diffed further down - byte-identical
# across languages is exactly the promise include/strings.h makes: a language
# switch may move the pretty name, never where the entity lives.
g++ -std=c++17 -Wall -Wextra \
    -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs \
    "$here/test_mqtt_discovery.cpp" "$root/src/mqtt_client.cpp" \
    -o "$out/test_mqtt_en"
g++ -std=c++17 -Wall -Wextra \
    -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs -DFW_LANG_EN=0 \
    "$here/test_mqtt_discovery.cpp" "$root/src/mqtt_client.cpp" \
    -o "$out/test_mqtt_sv"

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
    -I "$here/stub" -I "$root/include" -I "$json_inc" \
    "$here/test_web_server.cpp" "$root/src/web_server.cpp" "$root/src/roast_profile.cpp" \
    -o "$out/test_web_server"

# The control test links the real main.cpp with stubbed hardware, so it can
# drive setup()/loop() and the callbacks the web server calls. roast_profile.cpp
# is deliberately NOT linked - the test supplies its own canned profile.
# -pthread because the test runs a second thread in the role of the AsyncTCP
# task against loop(), which is the concurrency case the state lock exists for.
g++ -std=c++17 -Wall -Wextra -pthread \
    -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs \
    "$here/test_control.cpp" "$root/src/main.cpp" "$root/src/safety.cpp" \
    "$root/src/sensors.cpp" "$root/src/heater_control.cpp" \
    "$root/src/fan_control.cpp" "$root/src/ror.cpp" "$root/src/mqtt_client.cpp" \
    -o "$out/test_control"

# Same test under ThreadSanitizer: it is the only way to see a shared field
# that the lock forgot. Rides on the same binary source, so a race anywhere in
# the loop-vs-web-callback path fails the suite.
g++ -std=c++17 -Wall -Wextra -pthread -fsanitize=thread \
    -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs \
    "$here/test_control.cpp" "$root/src/main.cpp" "$root/src/safety.cpp" \
    "$root/src/sensors.cpp" "$root/src/heater_control.cpp" \
    "$root/src/fan_control.cpp" "$root/src/ror.cpp" "$root/src/mqtt_client.cpp" \
    -o "$out/test_control_tsan"

"$out/test_safety"
"$out/test_ror"
"$out/test_mqtt_en" "$out/unique_ids_en"
"$out/test_mqtt_sv" "$out/unique_ids_sv"
# The whole point of the two builds above: the same entities, in the same
# place, whichever language the firmware was built with.
if diff -u "$out/unique_ids_en" "$out/unique_ids_sv"; then
  echo "ok   unique_ids are identical in both build languages"
else
  echo "FAIL unique_ids differ between build languages - a language switch" >&2
  echo "     would move existing entities in Home Assistant" >&2
  exit 1
fi
"$out/test_web_server"
"$out/test_control"
# ThreadSanitizer cannot map its shadow under this kernel's ASLR entropy (it
# aborts with "unexpected memory mapping"), so the sanitized binary runs with
# address randomisation turned off for that one process. That only changes
# the address layout - the threads, the interleavings and the report are the
# same, which is the whole point of the run.
if command -v setarch >/dev/null 2>&1; then
  setarch "$(uname -m)" -R "$out/test_control_tsan"
else
  "$out/test_control_tsan"
fi
