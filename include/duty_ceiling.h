#pragma once
#include "config.h"

// The duty ceiling that follows the bean weight, as a pure function so it can
// be tested without a machine (tools/host-tests/test_duty_ceiling.cpp).
//
// WHY: the element is the same 1.1 kW whatever sits in the drum. A 20 g charge
// therefore sees ~5x the heat per gram that a 100 g charge does, and the loop
// cannot tell - it only sees the bean temperature, which lags by the probe and
// the thermal mass (~19 s). Full duty on 20 g overshoots the target curve
// before the loop has any evidence that it should back off, so it saturates,
// overshoots and swings (measured 2026-10-20: +-15-20 C at a 70-80 s period).
// Capping the request at a duty the charge can actually absorb removes the
// worst of that without touching the PID gains.
//
// THIS IS A FIRST MEASURE, not a controller. It does not steer the ramp, it
// only stops asking for more heat than the batch can take. Following a rate of
// rise (rorTarget, already supported per step) is the next step if the curve
// still does not track.
//
// WHAT IT IS NOT: a safety limit. It never overrides the safety latch, the fan
// interlock or applyHeaterDuty()'s own limits, and it applies to the control
// loop's request in profile mode only - a manual run and a cool-down are
// unchanged by construction (they never consult it).
//
// Linear between DUTY_CEIL_REF_WEIGHT_G (28 %) and DUTY_CEIL_FULL_WEIGHT_G
// (100 %) - and the same line continues below the reference weight, so a 5 g
// charge gets less heat than a 20 g one - floored at DUTY_CEIL_FLOOR_PCT so a
// tiny charge cannot end up with a ceiling that never lets the element heat.
//
// 0 (or a profile without the field) means "no ceiling": every profile written
// before the field existed keeps behaving exactly as it did.
inline float dutyCeilingPct(int beanWeightGrams) {
  if (beanWeightGrams <= 0) return HEATER_MAX_DUTY_PCT;  // no weight = no ceiling

  const float w = (float)beanWeightGrams;
  const float wRef = (float)DUTY_CEIL_REF_WEIGHT_G;
  const float wFull = (float)DUTY_CEIL_FULL_WEIGHT_G;

  float pct = DUTY_CEIL_AT_REF_PCT;
  if (w >= wFull) {
    pct = HEATER_MAX_DUTY_PCT;
  } else {
    // One straight line through (20 g, 28 %) and (100 g, 100 %), continuing
    // below 20 g as well: a 5 g charge should get less heat than a 20 g one,
    // not the same. Where that line runs under DUTY_CEIL_FLOOR_PCT the floor
    // takes over, so the element can still heat at all.
    pct = DUTY_CEIL_AT_REF_PCT + (HEATER_MAX_DUTY_PCT - DUTY_CEIL_AT_REF_PCT) *
                                    (w - wRef) / (wFull - wRef);
  }
  if (pct < DUTY_CEIL_FLOOR_PCT) pct = DUTY_CEIL_FLOOR_PCT;
  if (pct > HEATER_MAX_DUTY_PCT) pct = HEATER_MAX_DUTY_PCT;
  return pct;
}
