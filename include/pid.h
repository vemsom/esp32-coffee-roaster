#pragma once
#include <Arduino.h>

// Enkel PID-regulator, header-only.
// Anvands av varmestyrningen for att rakna ut duty-cycle mot borvardet
// under en aktiv rostning.
class SimplePID {
public:
  SimplePID(double kp, double ki, double kd, double outMin, double outMax)
    : _kp(kp), _ki(ki), _kd(kd), _outMin(outMin), _outMax(outMax) {}

  void reset() {
    _integral = 0;
    _lastError = 0;
    _lastTime = millis();
    _firstRun = true;
  }

  double compute(double setpoint, double input) {
    unsigned long now = millis();
    double dt = (now - _lastTime) / 1000.0;

    if (_firstRun || dt <= 0) {
      _lastTime = now;
      _firstRun = false;
      _lastError = setpoint - input;
      return 0;
    }

    double error = setpoint - input;
    _integral += error * dt;

    if (_integral > _outMax) _integral = _outMax;
    if (_integral < _outMin) _integral = _outMin;

    double derivative = (error - _lastError) / dt;
    double output = _kp * error + _ki * _integral + _kd * derivative;

    if (output > _outMax) output = _outMax;
    if (output < _outMin) output = _outMin;

    _lastError = error;
    _lastTime = now;
    return output;
  }

private:
  double _kp, _ki, _kd;
  double _outMin, _outMax;
  double _integral = 0;
  double _lastError = 0;
  unsigned long _lastTime = 0;
  bool _firstRun = true;
};
