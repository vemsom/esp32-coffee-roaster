#pragma once
#include "config.h"

// Feed-forward over the head of the PID: the duty the roast asks for BEFORE any
// error exists, so the loop does not have to build one up first.
//
// WHY. Measured on three real roasts (2026-10-20, 20 / 40 / 62.8 g, same curve):
// BT trails the target curve by +12..+14 C at 30 s and +8..+10 C at 120 s, and
// the lag is the SAME at every charge. That is not the loop being mistuned and
// it is not the mass: a PID only sees the temperature error, so on a linear ramp
// it cannot ask for heat until the roast is already behind, and the integral has
// to catch up with a ramp that keeps moving. The element is also the same ~1.1
// kW set of losses whatever is in the drum, so most of the duty on a ramp is not
// a correction at all - it is the heat needed to climb at the requested rate
// plus the heat needed to sit at the current temperature. That part is known in
// advance: it is the profile's own slope and the bean temperature.
//
// THE MODEL. Fitted to all three roasts, 313 samples at 10 s intervals, ramps
// only (holds, pauses and the cool-down excluded):
//
//   duty_ff = FF_SLOPE_GAIN * slope + FF_LOSS_GAIN * (BT - FF_AMBIENT_C)
//             + FF_OFFSET_PCT                                    [percent]
//
//   slope = the REQUESTED rate, C/min (the derivative of the profile's own
//           targetAt(), not the measured rise)
//   BT    = the bean temperature the control loop reads (currentBTFiltered)
//
// Mean error 2.5 percentage points, R^2 0.97 over those 313 samples. The three
// constants are in include/config.h with the fit behind them; the control points
// that pin them are asserted in tools/host-tests/test_feed_forward.cpp
// (45 C/min at BT 60 -> 40 %, matching 38-42 % measured; 12 C/min at BT 180 ->
// 86 %, matching 81-91 %).
//
// WHERE IT GOES. Added to the PID's output and clipped once, to the same
// 0..HEATER_MAX_DUTY_PCT range every duty request has always had, in
// updateControl() (src/main.cpp), profile mode only. It is a base, not a
// decision: it never bypasses the safety latch, the fan interlock or
// applyHeaterDuty()'s own limits - those judge the total, after this is added.
//
// WHAT IT IS NOT. It does not replace the PID: the PID still trims on top, and
// its anti-windup works against 100 %, not against this number. On a hold the
// requested slope is 0, so the model reduces to the loss term (the duty that
// holds the drum at BT) - which is exactly the base a hold needs.

// The model as a pure function, so it can be tested without a machine.
// A NaN input (a probe that has never read) yields 0: no feed-forward at all is
// the honest answer there, and the caller refuses to heat on a NaN anyway.
inline float feedForwardDutyPct(float slopeCPerMin, float btC) {
  if (!(slopeCPerMin == slopeCPerMin) || !(btC == btC)) return 0.0f;  // NaN
  float duty = FF_SLOPE_GAIN_PCT_PER_C_PER_MIN * slopeCPerMin +
               FF_LOSS_GAIN_PCT_PER_C * (btC - FF_AMBIENT_C) + FF_OFFSET_PCT;
  if (duty < 0.0f) duty = 0.0f;
  if (duty > HEATER_MAX_DUTY_PCT) duty = HEATER_MAX_DUTY_PCT;
  return duty;
}

// The requested rate in C/min at 'elapsed', taken from the curve itself: the
// profile's target one second ahead minus the target now, x 60. That is the
// derivative of the REQUESTED curve - a ramp gives the ramp's own slope, a hold
// gives exactly 0, and a pause gives 0 for free because the roast clock does not
// advance while paused.
//
// A finite difference rather than an analytic per-step slope on purpose: it is
// the same one line for ramps, holds, the transition between them and any curve
// shape a profile can express, and there is no second copy of the profile's
// timeline logic to keep in step with targetAt(). The one-second window is short
// against the ~19 s the machine takes to answer (so it is not a delay in the
// loop) and it lands on the true ramp slope everywhere except within the last
// second before a hold, where it blends the two - a step of at most
// FF_SLOPE_GAIN * slope (4 pp at a 45 C/min ramp) rather than a spike.
template <typename TargetAtFn>
inline float requestedSlopeCPerMin(TargetAtFn targetAt, unsigned long elapsedSeconds) {
  const float now = targetAt(elapsedSeconds);
  const float next = targetAt(elapsedSeconds + 1);
  if (!(now == now) || !(next == next)) return 0.0f;  // NaN: no curve to read
  return (next - now) * 60.0f;
}
