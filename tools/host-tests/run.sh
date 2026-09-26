#!/bin/sh
# Host-side tests (no ESP32 and no PlatformIO toolchain needed - just g++):
#   test_safety          safety latch + heater interlock, stubbed Arduino
#   test_mqtt_discovery  MQTT discovery payloads + command handling, using a
#                        recording PubSubClient stub (no broker needed)
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

"$out/test_safety"
"$out/test_mqtt"
