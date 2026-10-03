# Ongoing Notes

- 2026-10-03: sensor-pinnarna omlagda för **det egna kretskortet** (eget beslut). Bara klockan delas (CLK 18); varje MAX6675-modul har nu egen SO och egen CS — SO-BT 19, SO-ET 21, CS-BT 13, CS-ET 17. CS-BT flyttad från GPIO5 (strapping-pinne, lämnas helt) och GPIO2 undviks (strapping + inbyggd lysdiod). Skälet till egna SO-linjer: en delad SO vilar på att den oselekterade modulens SO går i tre läge, vilket inte gick att bekräfta i databladet — på ett kort kostar den egna returlinjen ingenting. Se docs/wiring.md.
- SPI pins for MAX6675 (CLK 18, SO-BT 19, SO-ET 21, CS-BT 13, CS-ET 17 - no MOSI, read-only) assigned 2026-09-26 and revised 2026-10-03 for the custom board; still to be confirmed against the physical board
- Firmware sanity check for MAX6675: flag if a reading jumps >20C between samples; the "gets stuck" half is implemented as of 2026-09-26 (60 s of identical readings while the element is on - SENSOR_STUCK_MAX_MS, see docs/firmware-notes.md), and the 60 s number still wants confirming against the real noise figures
- Hard safety limits live in include/config.h (SAFETY_MAX_TEMP_C etc.) and the latch logic in src/safety.cpp - see docs/firmware-notes.md
- Host tests for the latch, the interlock, the MQTT layer and the control loop: tools/host-tests/run.sh (293 checks, 0 failures, 2026-09-27; test_control also built under ThreadSanitizer)
