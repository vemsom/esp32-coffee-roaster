#!/bin/sh
# Host-side tests: compiles the safety latch and the heater interlock against a
# stubbed Arduino.h and asserts the behaviour. No ESP32 and no PlatformIO
# toolchain needed - just g++.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$(mktemp -d)

g++ -std=c++17 -Wall -Wextra \
    -I "$here/stub" -I "$root/include" \
    "$here/test_safety.cpp" "$root/src/safety.cpp" "$root/src/heater_control.cpp" \
    -o "$out/test_safety"

"$out/test_safety"
status=$?
rm -rf "$out"
exit $status
