#pragma once
#include "config.h"
#include <math.h>

// The rate-of-rise guidance as a pure law: how many degrees the measured rise
// moves the SETPOINT the PID chases (0.16.0).
//
// WHY. Up to 0.15.0 the same law ended in a duty offset added on top of the
// PID's output, so profile mode had two regulators with integral action on one
// actuator, one of them fed a derivative of the temperature. On a real roast
// (2026-10-25, 60 g) the error sat at -38 and +67 C/min - both past the
// +-33 C/min a gain of 3 %/(C/min) needs to saturate - so the guidance owned the
// element and duty slammed between 0 % and 100 %. See include/config.h for the
// measured numbers.
//
// WHAT IT IS NOW. The error is a setpoint trim in C, dead-banded, clipped and
// slew-limited:
//
//   err  = rorTarget - rorActual                       [C/min]
//   trim = 0                                           while |err| <= deadband
//   trim = clip(gain * err, -max, +max)                [C]
//
// and the PID sees (profile target + trim). The trim cannot take the element
// away from the loop: the loop still owns the duty, and the largest thing the
// guidance can do is move its setpoint by ROR_TRIM_MAX_C.
//
// The deadband is where the trim is EXACTLY 0, not where a ramp is smoothed:
// the 30 s estimate is noisy, and a guidance that chases every degree of that
// noise on a derivative is a dither generator.

// The trim the current error asks for, before the slew limit. A NaN on either
// side (a probe that never read, an estimate that has not warmed up) yields 0:
// no trim at all is the honest answer there, and the caller refuses to steer on
// a number it does not have.
inline float rorTrimRequestC(float rorTargetCPerMin, float rorActualCPerMin) {
  if (!(rorTargetCPerMin == rorTargetCPerMin) || !(rorActualCPerMin == rorActualCPerMin))
    return 0.0f;  // NaN
  if (rorTargetCPerMin <= 0.0f) return 0.0f;  // no RoR-driven step
  const float err = rorTargetCPerMin - rorActualCPerMin;
  if (fabsf(err) <= ROR_TRIM_DEADBAND_C_PER_MIN) return 0.0f;
  float trim = ROR_TRIM_GAIN_C_PER_C_PER_MIN * err;
  if (trim > ROR_TRIM_MAX_C) trim = ROR_TRIM_MAX_C;
  if (trim < -ROR_TRIM_MAX_C) trim = -ROR_TRIM_MAX_C;
  return trim;
}

// Clamp a delta to a per-second limit, accounting for the time since the last
// update (dt in milliseconds). A zero dt yields 0, so a stalled or wrapped clock
// cannot unlock a step.
inline float slewLimitedStep(float delta, unsigned long dtMs, float limitPerSecond) {
  if (dtMs == 0) return 0.0f;
  const float maxDelta = limitPerSecond * (dtMs / 1000.0f);
  if (delta > maxDelta) return maxDelta;
  if (delta < -maxDelta) return -maxDelta;
  return delta;
}
