#pragma once
#include <cmath>

// One-pole (exponential) low-pass filter, for a sensor signal on its way into
// a controller. Header-only and clock-agnostic: the caller passes the sample
// interval, so the same class runs from loop() and from a host test.
//
// Why it exists: the BT thermocouple + MAX6675 chain is quantised to 0.25 C
// and, over a 250 ms sample, jitters by several degrees around the real
// temperature. That is fine for a P term and fatal for a derivative - and the
// closed loop cannot tell dither from a real step. Smoothing the input is the
// honest fix; a derivative gain big enough to survive the noise is not.
//
// IMPORTANT - this belongs in the CONTROL path only. The safety layer
// (safety_update(), SENSOR_FAULT_MAX_JUMP_C, SENSOR_MIN_VALID_C) must keep
// seeing the raw sample: a filter would smear a dropped probe into a plausible
// ramp and hide exactly the fault those checks exist to catch. So it is applied
// where the PID reads its input (src/main.cpp), never in sensors_read().
class EmaFilter {
 public:
  explicit EmaFilter(double tauSeconds) : _tau(tauSeconds) {}

  // Forget the history: the next update() seeds from its own sample.
  void reset() {
    _value = 0;
    _initialised = false;
  }

  // Start from a known value instead of decaying from a stale one - used when a
  // run starts, so the first control cycle already sees the real temperature
  // and the filter does not pull the loop for a second.
  void seed(double sample) {
    _value = sample;
    _initialised = !isnan(sample);
  }

  double update(double sample, double dtSeconds) {
    if (!_initialised) {
      _value = sample;
      _initialised = !isnan(sample);
      return _value;
    }
    if (isnan(sample)) return _value;
    if (dtSeconds <= 0.0 || _tau <= 0.0) return _value;
    const double alpha = 1.0 - std::exp(-dtSeconds / _tau);
    _value += alpha * (sample - _value);
    return _value;
  }

  double value() const { return _value; }
  bool initialised() const { return _initialised; }

 private:
  double _tau;
  double _value = 0;
  bool _initialised = false;
};
