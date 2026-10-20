#pragma once
#include <Arduino.h>

// Enkel PID-regulator, header-only.
// Anvands av varmestyrningen for att rakna ut duty-cycle mot borvardet
// under en aktiv rostning.
//
// The output is a heater duty in percent, so outMin/outMax are 0..100.
//
// Two things about the integral, because getting them wrong is what made this
// loop look like a 20 C calibration error (2026-10-17):
//
//   * The state is accumulated in C*s (error * dt), but it is only ever USED
//     as KI * integral, and that product is a percentage. Clamping the raw
//     state against the 0..100 output range (what this used to do) capped the I
//     contribution at KI * 100 = 5 %, so the regulator was a P-only loop
//     wearing a PID's name: it settles where KP * error = duty, i.e. at
//     error = duty / KP - with KP 4.0 and 82-86 % duty that is the ~20 C the
//     bean temperature sat under its setpoint. The clamp now applies to
//     KI * integral, in the unit the output is actually made of.
//   * Windup: the I term must not keep growing while the output is already
//     saturated, or the loop pays for it with an overshoot the moment the
//     setpoint is reached. Conditional integration instead: a sample is
//     committed only while it does not push an already saturated output
//     further out.
class SimplePID {
public:
  SimplePID(double kp, double ki, double kd, double outMin, double outMax)
    : _kp(kp), _ki(ki), _kd(kd), _outMin(outMin), _outMax(outMax) {}

  void reset() {
    _integral = 0;
    _lastError = 0;
    _lastTime = millis();
    _firstRun = true;
    _pTerm = _iTerm = _dTerm = 0;
    _output = 0;
  }

  double compute(double setpoint, double input) {
    unsigned long now = millis();
    double dt = (now - _lastTime) / 1000.0;

    if (_firstRun || dt <= 0) {
      _lastTime = now;
      _firstRun = false;
      _lastError = setpoint - input;
      _pTerm = _iTerm = _dTerm = 0;
      _output = 0;
      return 0;
    }

    double error = setpoint - input;
    double pTerm = _kp * error;

    // KD is 0 in the shipped configuration (config.h): this term differentiates
    // a signal sampled every 250 ms and quantised to the MAX6675's 0.25 C
    // ladder, and a derivative over that much noise is a switch rather than a
    // correction - it drove duty between 0 and 100 on the machine. The term is
    // kept here (and reported) so a zero KD is a configuration, not a missing
    // feature.
    double dTerm = _kd * (error - _lastError) / dt;

    double iTerm = clampOutput(_ki * (_integral + error * dt));

    // Conditional integration (anti-windup). While the output the three terms
    // would produce is already past a limit and the error pushes further that
    // way, the integral stands still: the P term alone is driving the output
    // there, and letting I grow against a saturated output is exactly the
    // windup that used to slam duty to 0 the moment the setpoint was reached.
    // The opposite case (saturated, error pointing back inside) is committed,
    // which is what lets a wound-up state unwind.
    double candidateOutput = pTerm + iTerm + dTerm;
    if ((candidateOutput > _outMax && error > 0.0) ||
        (candidateOutput < _outMin && error < 0.0)) {
      iTerm = clampOutput(_ki * _integral);
    } else if (_ki != 0.0) {
      _integral += error * dt;
      iTerm = clampOutput(_ki * _integral);
      // Keep the stored state inside the clamp too, so a fully wound integrator
      // is at most outMax / KI in C*s and unwinds from there instead of from an
      // unbounded number no clamp ever touched.
      _integral = iTerm / _ki;
    }

    double output = pTerm + iTerm + dTerm;
    if (output > _outMax) output = _outMax;
    if (output < _outMin) output = _outMin;

    _pTerm = pTerm;
    _iTerm = iTerm;
    _dTerm = dTerm;
    _output = output;
    _lastError = error;
    _lastTime = now;
    return output;
  }

  // The upper output limit is movable at run time. The bean-weight duty
  // ceiling (include/duty_ceiling.h) is a decision the controller has to know
  // about, not a clamp applied after it: if only the request were clipped, the
  // integral would keep growing against a ceiling it cannot see, and the loop
  // would pay for that with an overshoot the moment the setpoint is reached.
  // Moving _outMax here reuses the anti-windup above unchanged - it already
  // tests the output against _outMax.
  void setOutMax(double outMax) {
    if (outMax < _outMin) outMax = _outMin;
    _outMax = outMax;
  }
  double outMax() const { return _outMax; }

  // The contributions as they were last applied (percent, except integral,
  // which is the raw C*s state). Reported by /api/status so the loop can be
  // trimmed against a machine that is running, and so a stuck or clamped
  // integral is visible instead of having to be deduced from the duty.
  double pTerm() const { return _pTerm; }
  double iTerm() const { return _iTerm; }
  double dTerm() const { return _dTerm; }
  double integral() const { return _integral; }
  double output() const { return _output; }

private:
  double clampOutput(double v) const {
    if (v > _outMax) return _outMax;
    if (v < _outMin) return _outMin;
    return v;
  }

  double _kp, _ki, _kd;
  double _outMin, _outMax;
  double _integral = 0;
  double _lastError = 0;
  unsigned long _lastTime = 0;
  bool _firstRun = true;
  double _pTerm = 0, _iTerm = 0, _dTerm = 0, _output = 0;
};
