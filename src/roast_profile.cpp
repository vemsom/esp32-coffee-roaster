#include "roast_profile.h"
#include "config.h"
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

void RoastProfile::clear() {
  _steps.clear();
  _hasFan = false;
  _startTemp = 20;
  _coolFanPct = 0;
  _coolMinutes = 0;
  _beanWeight = 0;
}

// Cooling is a pair: a fan speed without a time (or the other way round) is
// not a cool-down, it is a half-filled form, so either value being 0 turns the
// whole thing off. Anything else is clamped to the same limits the web UI and
// the cooling panel work with, so a hand-edited profile cannot carry a value
// that no other part of the firmware would accept.
void RoastProfile::setCool(float fanPct, float minutes) {
  if (!(fanPct > 0.0f) || !(minutes > 0.0f)) {  // also catches NaN
    _coolFanPct = 0;
    _coolMinutes = 0;
    return;
  }
  if (fanPct < (float)COOL_SPEED_MIN_PCT) fanPct = (float)COOL_SPEED_MIN_PCT;
  if (fanPct > (float)COOL_SPEED_MAX_PCT) fanPct = (float)COOL_SPEED_MAX_PCT;
  if (minutes < COOL_MINUTES_MIN) minutes = COOL_MINUTES_MIN;
  if (minutes > COOL_MINUTES_MAX) minutes = COOL_MINUTES_MAX;
  _coolFanPct = fanPct;
  _coolMinutes = minutes;
}

// The charge in the drum. 0 (or anything <= 0) means "not stated", which turns
// the duty ceiling off - that is what every profile written before the field
// existed says, so those keep behaving as they did. A stated weight is clamped
// to BEAN_WEIGHT_MIN_G..BEAN_WEIGHT_MAX_G for the same reason setCool() clamps:
// a hand-edited file must not be able to carry a value the editor could not
// produce, and a 3 g or 5000 g profile would otherwise silently get a ceiling
// nobody asked for.
void RoastProfile::setBeanWeight(int grams) {
  if (grams <= 0) {
    _beanWeight = 0;
    return;
  }
  if (grams < BEAN_WEIGHT_MIN_G) grams = BEAN_WEIGHT_MIN_G;
  if (grams > BEAN_WEIGHT_MAX_G) grams = BEAN_WEIGHT_MAX_G;
  _beanWeight = grams;
}

// The end of the last step. Used by src/main.cpp to decide that the profile is
// over - the curve functions themselves keep holding their last value past the
// end, which is what the roast graph draws.
unsigned long RoastProfile::totalSeconds() const {
  unsigned long total = 0;
  for (const auto &s : _steps) total += s.rampSeconds + s.holdSeconds;
  return total;
}

void RoastProfile::addStep(unsigned long rampSeconds, unsigned long holdSeconds,
                           float temp, float fan) {
  ProfileStep s;
  s.rampSeconds = rampSeconds;
  s.holdSeconds = holdSeconds;
  s.temp = temp;
  s.fan = fan;
  _steps.push_back(s);
  if (fan > 0) _hasFan = true;
}

void RoastProfile::addStep(unsigned long rampSeconds, unsigned long holdSeconds,
                           float temp, float fan,
                           float rorTarget, float rorStart, float rorEnd) {
  ProfileStep s;
  s.rampSeconds = rampSeconds;
  s.holdSeconds = holdSeconds;
  s.temp = temp;
  s.fan = fan;
  s.rorTarget = rorTarget;
  s.rorStart = rorStart;
  s.rorEnd = rorEnd;
  _steps.push_back(s);
  if (fan > 0) _hasFan = true;
}

// Walk the step timeline, returning the interpolated value at 'elapsed'.
// During a ramp the value moves linearly from the previous value to the
// step value; during a hold it stays at the step value.
template <typename F>
static float valueAt(const std::vector<ProfileStep> &steps, float start,
                     unsigned long elapsed, F value) {
  float prev = start;
  unsigned long t = 0;
  for (const auto &s : steps) {
    unsigned long rampEnd = t + s.rampSeconds;
    unsigned long holdEnd = rampEnd + s.holdSeconds;

    if (s.rampSeconds > 0 && elapsed < rampEnd) {
      float ratio = (float)(elapsed - t) / (float)s.rampSeconds;
      return prev + ratio * (value(s) - prev);
    }
    if (elapsed < holdEnd) return value(s);

    prev = value(s);
    t = holdEnd;
  }
  return prev;
}

float RoastProfile::targetAt(unsigned long elapsedSeconds) const {
  if (_steps.empty()) return NAN;
  return valueAt(_steps, _startTemp, elapsedSeconds,
                 [](const ProfileStep &s) { return s.temp; });
}

float RoastProfile::rorTargetAt(unsigned long elapsedSeconds) const {
  if (_steps.empty()) return 0.0f;
  // Use the same interpolation as targetAt(), but driven by rorStart..rorEnd
  // and gated on rorTarget > 0. A disabled step returns 0.
  float prevTarget = 0.0f;
  unsigned long t = 0;
  for (const auto &s : _steps) {
    unsigned long rampEnd = t + s.rampSeconds;
    unsigned long holdEnd = rampEnd + s.holdSeconds;
    if (s.rorTarget <= 0.0f) {
      prevTarget = 0.0f;
      t = holdEnd;
      continue;
    }
    float prev = (t == 0) ? s.rorStart : prevTarget;
    if (s.rampSeconds > 0 && elapsedSeconds < rampEnd) {
      float ratio = (float)(elapsedSeconds - t) / (float)s.rampSeconds;
      return prev + ratio * (s.rorEnd - prev);
    }
    if (elapsedSeconds < holdEnd) return s.rorEnd;
    prevTarget = s.rorEnd;
    t = holdEnd;
  }
  return 0.0f;
}

int RoastProfile::stepIndexAt(unsigned long elapsedSeconds) const {
  if (_steps.empty()) return -1;
  unsigned long t = 0;
  for (size_t i = 0; i < _steps.size(); i++) {
    const auto &s = _steps[i];
    unsigned long rampEnd = t + s.rampSeconds;
    unsigned long holdEnd = rampEnd + s.holdSeconds;
    if (elapsedSeconds < holdEnd) return (int)i;
    t = holdEnd;
  }
  return -1;
}

// The fan does not ramp: it steps to the step's value at the start of the
// step and holds it until the next step begins.
float RoastProfile::fanAt(unsigned long elapsedSeconds) const {
  if (_steps.empty()) return NAN;
  float fan = 0;
  unsigned long t = 0;
  for (const auto &s : _steps) {
    if (elapsedSeconds < t) break;
    fan = s.fan;
    t += s.rampSeconds + s.holdSeconds;
  }
  return fan;
}

bool RoastProfile::loadFromFile(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;  // ArduinoJson v7 - dynamic sizing
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  clear();
  _startTemp = doc["startTemp"] | 20.0f;
  // The cool-down that follows the last step. Absent, 0 or a half-filled pair
  // all mean "no automatic cooling", which is what every profile written
  // before these fields existed says - so those keep behaving as they did.
  setCool(doc["coolFan"] | 0.0f, doc["coolMinutes"] | 0.0f);
  // The charge. Absent or 0 = no duty ceiling, i.e. the pre-0.14.0 behaviour;
  // a stated weight is clamped like every other profile value.
  setBeanWeight(doc["beanWeight"] | 0);

  if (doc["steps"].is<JsonArray>()) {
    for (JsonObject s : doc["steps"].as<JsonArray>()) {
      float rorTarget = s["rorTarget"] | 0.0f;
      if (rorTarget > 0.0f) {
        addStep(s["ramp"] | 0, s["hold"] | 0, s["temp"] | 0.0f, s["fan"] | 0.0f,
                rorTarget,
                s["rorStart"] | 0.0f,
                s["rorEnd"] | 0.0f);
      } else {
        addStep(s["ramp"] | 0, s["hold"] | 0, s["temp"] | 0.0f, s["fan"] | 0.0f);
      }
    }
  } else if (doc["points"].is<JsonArray>()) {
    // Backwards compatibility: an old point list becomes one ramp step per
    // point, ramping from the previous point's time to this one.
    unsigned long prevT = 0;
    for (JsonObject p : doc["points"].as<JsonArray>()) {
      unsigned long t = p["t"] | 0;
      addStep(t - prevT, 0, p["temp"] | 0.0f, p["fan"] | 0.0f);
      prevT = t;
    }
  }
  return true;
}

bool RoastProfile::saveToFile(const String &path) const {
  JsonDocument doc;
  doc["startTemp"] = _startTemp;
  JsonArray steps = doc["steps"].to<JsonArray>();
  for (const auto &s : _steps) {
    JsonObject o = steps.add<JsonObject>();
    o["ramp"] = s.rampSeconds;
    o["hold"] = s.holdSeconds;
    o["temp"] = s.temp;
    o["fan"] = s.fan;
    // Only write RoR guidance when the step uses it, so old profiles stay
    // byte-for-byte identical unless the user explicitly enables RoR.
    if (s.rorTarget > 0.0f) {
      o["rorTarget"] = s.rorTarget;
      o["rorStart"] = s.rorStart;
      o["rorEnd"] = s.rorEnd;
    }
  }

  // The profile's own cool-down, only when it asks for one: a profile without
  // it has to stay byte-for-byte identical to what it was before the fields
  // existed (same rule as rorTarget above).
  if (_coolFanPct > 0.0f && _coolMinutes > 0.0f) {
    doc["coolFan"] = _coolFanPct;
    doc["coolMinutes"] = _coolMinutes;
  }

  // The charge, only when the profile states one (same rule as above): a
  // profile without it has to stay byte-for-byte identical to what it was
  // before the field existed. 0 means "no ceiling", so writing it would say
  // the same thing at the cost of touching every old file.
  if (_beanWeight > 0) {
    doc["beanWeight"] = _beanWeight;
  }

  File f = LittleFS.open(path, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}
