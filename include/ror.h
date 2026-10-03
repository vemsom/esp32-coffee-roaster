#pragma once

// Rate of Rise (RoR): the temperature change per minute, estimated by a
// least-squares fit (linear regression) over every sample inside a sliding
// window:
//
//   slope of y = a + b*x fitted through the samples, reported as b * 60000
//   -> C/min
//
// Fitting the whole series rather than differencing two endpoints is the
// point: quantisation (the MAX6675's 0.25 C ladder) and reading noise average
// out along the fit, and the sign is the slope's own - negative means cooling
// or just past the turning point, and is never clamped.
//
// Window and warm-up live in include/config.h: ROR_WINDOW_MS (30 s) selects
// which samples take part, ROR_MIN_SPAN_MS (10 s) is how much history is
// needed before anything is published at all. In between, the fit runs over
// the shorter span that exists - and because the slope is per millisecond, a
// warm-up rate is already a correct C/min.
//
// Call ror_update() from the control loop at the sensor sample rate. The
// getters are called from the web/MQTT task, so - exactly like the
// temperature getters in main.cpp - both sides take the state lock; this
// module keeps no lock of its own.

// Clears the history and both rates (boot state, and the host tests).
void ror_reset();

// One control-loop sample. nowMs is millis(); bt/et may be NaN (a probe that
// has never read) - those samples are simply left out of the fit.
void ror_update(unsigned long nowMs, float bt, float et);

float ror_get_bt();  // C/min, one decimal, negative = cooling
float ror_get_et();  // C/min, one decimal, negative = cooling

// True once at least ROR_MIN_SPAN_MS of history exists. The warm-up value 0
// is intentionally indistinguishable from a flat rate, so guidance code must
// check this flag before acting on ror_get_et().
bool ror_valid();
