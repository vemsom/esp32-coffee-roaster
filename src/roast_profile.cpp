#include "roast_profile.h"
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

void RoastProfile::clear() {
  _steps.clear();
  _hasFan = false;
  _startTemp = 20;
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

  File f = LittleFS.open(path, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}
