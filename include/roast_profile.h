#pragma once
#include <Arduino.h>
#include <vector>

// One step of a roast profile: an optional linear ramp from the previous
// temperature up to 'temp', followed by an optional hold at 'temp'. The fan
// ramps/holds the same way, from the previous step's fan value.
struct ProfileStep {
  unsigned long rampSeconds;
  unsigned long holdSeconds;
  float temp;   // target temperature at the end of the ramp
  float fan;    // fan speed 0-100 % for the step
};

// A roast profile: an ordered list of steps, always starting from a
// room-temperature start point (default 20 C at 0 s).
class RoastProfile {
public:
  bool loadFromFile(const String &path);
  bool saveToFile(const String &path) const;
  void addStep(unsigned long rampSeconds, unsigned long holdSeconds, float temp, float fan);
  void clear();
  float targetAt(unsigned long elapsedSeconds) const;
  float fanAt(unsigned long elapsedSeconds) const;
  bool hasFan() const { return _hasFan; }
  float startTemp() const { return _startTemp; }
  void setStartTemp(float t) { _startTemp = t; }
  size_t stepCount() const { return _steps.size(); }
  const std::vector<ProfileStep> &steps() const { return _steps; }

private:
  std::vector<ProfileStep> _steps;
  float _startTemp = 20;
  bool _hasFan = false;
};
