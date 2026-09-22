#include "roast_profile.h"
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

void RoastProfile::clear() {
    _points.clear();
}

void RoastProfile::addPoint(unsigned long t, float temp) {
    _points.push_back({t, temp});
}

float RoastProfile::targetAt(unsigned long elapsedSeconds) const {
    if (_points.empty()) return NAN;
  if (elapsedSeconds <= _points.front().t) return _points.front().targetTemp;
  if (elapsedSeconds >= _points.back().t) return _points.back().targetTemp;

  for (size_t i = 0; i + 1 < _points.size(); i++) {
    const ProfilePoint &a = _points[i];
    const ProfilePoint &b = _points[i + 1];
    if (elapsedSeconds >= a.t && elapsedSeconds <= b.t) {
      float ratio = (float)(elapsedSeconds - a.t) / (float)(b.t - a.t);
      return a.targetTemp + ratio * (b.targetTemp - a.targetTemp);
    }
  }
  return _points.back().targetTemp;
}

bool RoastProfile::loadFromFile(const String &path) {
    File f = LittleFS.open(path, "r");
  if (!f) return false;

  JsonDocument doc;  // ArduinoJson v7 - dynamic sizing
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  clear();
  for (JsonObject p : doc["points"].as<JsonArray>()) {
    addPoint(p["t"].as<unsigned long>(), p["temp"].as<float>());
  }
  return true;
}

bool RoastProfile::saveToFile(const String &path) const {
    JsonDocument doc;
  JsonArray points = doc["points"].to<JsonArray>();
  for (const auto &p : _points) {
    JsonObject o = points.add<JsonObject>();
    o["t"] = p.t;
    o["temp"] = p.targetTemp;
  }

  File f = LittleFS.open(path, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}
