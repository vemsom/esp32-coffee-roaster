#pragma once
#include <Arduino.h>
#include <vector>

struct ProfilePoint {
  unsigned long t;    // seconds since roast start
  float targetTemp;   // degrees C
};

// A roast profile: a curve of (time, target temperature) points.
// targetAt() linearly interpolates between points so the heater
// control always has a setpoint, even between saved points.
class RoastProfile {
public:
  bool loadFromFile(const String &path);
  bool saveToFile(const String &path) const;
  void addPoint(unsigned long t, float temp);
  void clear();
  float targetAt(unsigned long elapsedSeconds) const;
  size_t pointCount() const { return _points.size(); }

private:
  std::vector<ProfilePoint> _points;
};
