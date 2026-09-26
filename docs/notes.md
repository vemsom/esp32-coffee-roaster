# Ongoing Notes

- SPI pins for MAX6675 (CLK 18, MISO 19, CS-BT 5, CS-ET 17 - no MOSI, read-only) assigned 2026-09-26 and written down in docs/wiring.md; still to be confirmed against the physical board
- Firmware sanity check for MAX6675: flag if a reading jumps >20C between samples; the "gets stuck" half is implemented as of 2026-09-26 (60 s of identical readings while the element is on - SENSOR_STUCK_MAX_MS, see docs/firmware-notes.md), and the 60 s number still wants confirming against the real noise figures
- Hard safety limits live in include/config.h (SAFETY_MAX_TEMP_C etc.) and the latch logic in src/safety.cpp - see docs/firmware-notes.md
- Host tests for the latch, the interlock, the MQTT layer and the control loop: tools/host-tests/run.sh (252 checks, 0 failures, 2026-09-26; test_control also built under ThreadSanitizer)
