#include "ror.h"
#include "config.h"
#include <cmath>

namespace {

// Circular buffer of EVERY control-loop sample (250 ms). ROR_BUFFER_LEN = 128
// holds 32 s of them - always more than ROR_WINDOW_MS - so the window can
// never outrun the history it is fitted over.
struct Sample {
  unsigned long t;
  float bt;
  float et;
};

Sample ring[ROR_BUFFER_LEN];
int count = 0;                 // valid entries, 0..ROR_BUFFER_LEN
int next = 0;                  // ring slot the next sample is written to
float rorBt = 0.0f;
float rorEt = 0.0f;
bool rorValid = false;         // true once ROR_MIN_SPAN_MS of history exists

// index 0 = newest sample. The arithmetic stays inside one buffer length so
// the unsigned wrap of millis() every 49.7 days does not need special casing.
const Sample &ago(int age) { return ring[(next + ROR_BUFFER_LEN - 1 - age) % ROR_BUFFER_LEN]; }
const Sample &oldest() { return ring[(next + ROR_BUFFER_LEN - count) % ROR_BUFFER_LEN]; }

// Published with one decimal so /api/status and the MQTT payload show the same
// number the web UI renders.
float round1(float value) { return std::round(value * 10.0f) / 10.0f; }

// True when there is enough real history to publish a non-warm-up rate.
// The answer is deliberately conservative: 0 means "we do not know yet", which
// the RoR-guidance layer must treat as "do not steer on this number".
bool valid(unsigned long nowMs) {
  if (count == 0) return false;
  return (nowMs - oldest().t) >= ROR_MIN_SPAN_MS;
}

// Least-squares slope over every usable sample inside the window, in C/min.
// Fitting the whole series instead of differencing two endpoints is what keeps
// the 0.25 C ladder and MAX6675 noise from landing in the answer: the
// stair-case of a quantised ramp averages out along the fit instead of
// showing up as one endpoint's rounding error.
//
// x is milliseconds relative to now, so the newest sample sits at 0 and the
// sums stay small; everything accumulates in double because n * x^2 reaches
// ~1e10 over a window. previous is carried over when there is nothing honest
// to publish - fewer than two usable samples, or all of them NaN - because a
// missing reading must never masquerade as a measured 0 C/min swing.
float compute(unsigned long nowMs, float Sample::*field, float nowValue, float previous) {
  if (std::isnan(nowValue)) return previous;
  if (count == 0) return 0.0f;

  // Below the minimum span there is nothing to fit yet - publish 0. Above it,
  // but short of a full window, the fit runs over the span that exists: the
  // slope is per millisecond either way, so the warm-up rate is already
  // correct C/min rather than a scaled one.
  if (!valid(nowMs)) return 0.0f;

  double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
  int n = 0;
  for (int age = 0; age < count; age++) {
    const Sample &s = ago(age);
    if (nowMs - s.t > ROR_WINDOW_MS) continue;   // older than the window
    float y = s.*field;
    if (std::isnan(y)) continue;                 // a probe that has never read
    double x = (double)s.t - (double)nowMs;
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
    n++;
  }

  if (n < 2) return previous;
  double denom = n * sxx - sx * sx;
  if (denom <= 0.0) return previous;             // every sample at one instant
  double slopePerMs = (n * sxy - sx * sy) / denom;
  return round1((float)(slopePerMs * 60000.0));  // C/ms -> C/min, any sign
}

}  // namespace

void ror_reset() {
  count = 0;
  next = 0;
  rorBt = 0.0f;
  rorEt = 0.0f;
  rorValid = false;
}

// Called at SENSOR_READ_INTERVAL_MS: every sample is stored, and both rates
// are refitted on each of them.
void ror_update(unsigned long nowMs, float bt, float et) {
  ring[next] = Sample{nowMs, bt, et};
  next = (next + 1) % ROR_BUFFER_LEN;
  if (count < ROR_BUFFER_LEN) count++;
  rorValid = valid(nowMs);
  rorBt = compute(nowMs, &Sample::bt, bt, rorBt);
  rorEt = compute(nowMs, &Sample::et, et, rorEt);
}

float ror_get_bt() { return rorBt; }
float ror_get_et() { return rorEt; }

// True once at least ROR_MIN_SPAN_MS of history exists. Used by the RoR
// guidance layer to refuse to steer on a freshly-reset or just-booted rate.
bool ror_valid() { return rorValid; }
