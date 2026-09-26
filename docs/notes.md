# Ongoing Notes

- SPI pins for MAX6675 (CLK 18, MISO 19, CS-BT 5, CS-ET 17 - no MOSI, read-only) assigned 2026-09-26 and written down in docs/wiring.md; still to be confirmed against the physical board
- Firmware sanity check for MAX6675: flag if a reading jumps >20C between samples; the "gets stuck" half is still not implemented (needs real noise figures first, see docs/firmware-notes.md)
- Hard safety limits live in include/config.h (SAFETY_MAX_TEMP_C etc.) and the latch logic in src/safety.cpp - see docs/firmware-notes.md
- Host tests for the latch, the interlock and the MQTT layer: tools/host-tests/run.sh (210 checks, 0 failures, 2026-09-26)
