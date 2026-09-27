#include "ror.h"
#include "config.h"
#include <cmath>

namespace {

// Circular buffer of per-second snapshots. 64 entries = 64 s of history, which
// always keeps a sample at least ROR_WINDOW_MS (60 s) old plus the ~1 s that
// 1 Hz sampling can add on either side of the mark, so a full window can never
// be evicted before it is used.
constexpr int ROR_HISTORY_LEN = 64;

struct Sample {
  unsigned long t;
  float bt;
  float et;
};

Sample ring[ROR_HISTORY_LEN];
int count = 0;                 // valid entries, 0..ROR_HISTORY_LEN
int next = 0;                  // ring slot the next snapshot is written to
unsigned long lastSnapshot = 0;
bool haveSnapshot = false;
float rorBt = 0.0f;
float rorEt = 0.0f;

// index 0 = newest snapshot. The arithmetic stays inside one buffer length so
// the unsigned wrap of millis() every 49.7 days does not need special casing.
const Sample &ago(int age) { return ring[(next + ROR_HISTORY_LEN - 1 - age) % ROR_HISTORY_LEN]; }

// Published with one decimal so /api/status and the MQTT payload show the same
// number the web UI renders.
float round1(float value) { return std::round(value * 10.0f) / 10.0f; }

// One channel's rate. previous is carried over when there is nothing honest to
// publish (no history yet, or a NaN at either end of the delta) - a missing
// reading must not masquerade as a sudden 0 C/min swing.
float compute(unsigned long nowMs, float Sample::*field, float nowValue, float previous) {
  if (std::isnan(nowValue)) return previous;
  if (count == 0) return 0.0f;

  // Newest snapshot that is at least ROR_WINDOW_MS old.
  int ref = -1;
  for (int age = 0; age < count; age++) {
    if (nowMs - ago(age).t >= ROR_WINDOW_MS) {
      ref = age;
      break;
    }
  }

  if (ref < 0) {
    // History shorter than the window: below the minimum span a rate would be
    // mostly quantisation noise, so publish 0. Above it, measure across what
    // there is - the normalisation below makes that a correct C/min anyway.
    if (nowMs - ago(count - 1).t < ROR_MIN_SPAN_MS) return 0.0f;
    ref = count - 1;
  }

  const Sample &s = ago(ref);
  float refValue = s.*field;
  if (std::isnan(refValue)) return previous;

  unsigned long elapsed = nowMs - s.t;
  if (elapsed == 0) return 0.0f;
  return round1((nowValue - refValue) * 60000.0f / (float)elapsed);
}

}  // namespace

void ror_reset() {
  count = 0;
  next = 0;
  lastSnapshot = 0;
  haveSnapshot = false;
  rorBt = 0.0f;
  rorEt = 0.0f;
}

// Called at SENSOR_READ_INTERVAL_MS. The snapshot cadence is separate: history
// is sampled once a second (1 Hz), the rate is recomputed on every sample so
// it moves with the live reading between snapshots.
void ror_update(unsigned long nowMs, float bt, float et) {
  if (!haveSnapshot || nowMs - lastSnapshot >= ROR_SNAPSHOT_INTERVAL_MS) {
    ring[next].t = nowMs;
    ring[next].bt = bt;
    ring[next].et = et;
    next = (next + 1) % ROR_HISTORY_LEN;
    if (count < ROR_HISTORY_LEN) count++;
    lastSnapshot = nowMs;
    haveSnapshot = true;
  }
  rorBt = compute(nowMs, &Sample::bt, bt, rorBt);
  rorEt = compute(nowMs, &Sample::et, et, rorEt);
}

float ror_get_bt() { return rorBt; }
float ror_get_et() { return rorEt; }
