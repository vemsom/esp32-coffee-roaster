# Ongoing Notes

- SPI pins for MAX6675 (CLK, CS, MISO - no MOSI, read-only) TBD once the layout is decided
- Firmware sanity check for MAX6675: flag if a reading jumps >20C between samples, or gets stuck
- Hard safety limits live in include/config.h (SAFETY_MAX_TEMP_C etc.) and the latch logic in src/safety.cpp - see docs/firmware-notes.md
- Host test for the latch: tools/host-tests/run.sh
