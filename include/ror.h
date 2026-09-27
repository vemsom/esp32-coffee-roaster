#pragma once

// Rate of Rise (RoR): the temperature change per minute, endpoint delta over a
// sliding window - the arithmetic Artisan calls "Delta Span".
//
//   ROR = (T_now - T_ref) * 60000 / elapsed_ms      -> C/min
//
// T_ref is the newest 1 Hz snapshot that is at least ROR_WINDOW_MS old. The
// delta is divided by the ACTUAL time between the two endpoints, not by the
// nominal window, so the result is C/min even while the history is shorter
// than the window (warm-up, down to ROR_MIN_SPAN_MS). A negative result is a
// legitimate value - cooling, or just past the turning point - and is never
// clamped.
//
// Call ror_update() from the control loop at the sensor sample rate. The
// getters are called from the web/MQTT task, so - exactly like the
// temperature getters in main.cpp - both sides take the state lock; this
// module keeps no lock of its own.

// Clears the history and both rates (boot state, and the host tests).
void ror_reset();

// One control-loop sample. nowMs is millis(); bt/et may be NaN (a probe that
// has never read) - that channel then keeps its previous rate.
void ror_update(unsigned long nowMs, float bt, float et);

float ror_get_bt();  // C/min, one decimal, negative = cooling
float ror_get_et();  // C/min, one decimal, negative = cooling
