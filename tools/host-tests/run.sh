#!/bin/sh
# Host-side tests (no ESP32 and no PlatformIO toolchain needed - just g++):
#   test_safety          safety latch + heater interlock, stubbed Arduino
#   test_mqtt_discovery  MQTT discovery payloads + command handling, using a
#                        recording PubSubClient stub (no broker needed)
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
  mqtt_defs='-DMQTT_HOST="192.168.1.173" -DMQTT_USER="host-test-user" -DMQTT_PASSWORD="host-test-pass"'
fi

if [ ! -d "$json_inc" ]; then
  echo "ArduinoJson not found at $json_inc - run 'pio run' once first" >&2
  exit 2
fi

g++ -std=c++17 -Wall -Wextra \
    -I "$here/stub" -I "$root/include" \
    "$here/test_safety.cpp" "$root/src/safety.cpp" "$root/src/heater_control.cpp" \
    -o "$out/test_safety"

g++ -std=c++17 -Wall -Wextra \
    -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs \
    "$here/test_mqtt_discovery.cpp" "$root/src/mqtt_client.cpp" \
    -o "$out/test_mqtt"

# The control test links the real main.cpp with stubbed hardware, so it can
# drive setup()/loop() and the callbacks the web server calls. roast_profile.cpp
# is deliberately NOT linked - the test supplies its own canned profile.
# -pthread because the test runs a second thread in the role of the AsyncTCP
# task against loop(), which is the concurrency case the state lock exists for.
g++ -std=c++17 -Wall -Wextra -pthread \
    -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs \
    "$here/test_control.cpp" "$root/src/main.cpp" "$root/src/safety.cpp" \
    "$root/src/sensors.cpp" "$root/src/heater_control.cpp" \
    "$root/src/fan_control.cpp" "$root/src/mqtt_client.cpp" \
    -o "$out/test_control"

# Same test under ThreadSanitizer: it is the only way to see a shared field
# that the lock forgot. Rides on the same binary source, so a race anywhere in
# the loop-vs-web-callback path fails the suite.
g++ -std=c++17 -Wall -Wextra -pthread -fsanitize=thread \
    -I "$here/stub" -I "$root/include" -I "$json_inc" $mqtt_defs \
    "$here/test_control.cpp" "$root/src/main.cpp" "$root/src/safety.cpp" \
    "$root/src/sensors.cpp" "$root/src/heater_control.cpp" \
    "$root/src/fan_control.cpp" "$root/src/mqtt_client.cpp" \
    -o "$out/test_control_tsan"

"$out/test_safety"
"$out/test_mqtt"
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
