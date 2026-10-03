#pragma once
// Host stub for include/roast_profile.h.
//
// test_control and test_ror_guidance link the REAL main.cpp (and the real
// web_server.cpp and ota_push.cpp) but deliberately not roast_profile.cpp, so
// the profile they drive is a canned one instead of a file on LittleFS. The
// test translation units define every method themselves, which means the
// declarations here have to match src/roast_profile.cpp exactly - a method
// main.cpp or web_server.cpp calls but nobody defines is an undefined
// reference at link time, which is the only thing this header is for.
//
// test_web_server links the real roast_profile.cpp and never includes this
// file: only the two tests that need a canned profile put this directory first
// on their include path.
#include <Arduino.h>

#include <vector>

struct ProfileStep {
  unsigned long rampSeconds;
  unsigned long holdSeconds;
  float temp;
  float fan;
  float rorTarget = 0;
  float rorStart = 0;
  float rorEnd = 0;
};

class RoastProfile {
public:
  bool loadFromFile(const String &path);
  bool saveToFile(const String &path) const;
  void addStep(unsigned long rampSeconds, unsigned long holdSeconds, float temp, float fan);
  void addStep(unsigned long rampSeconds, unsigned long holdSeconds, float temp, float fan,
               float rorTarget, float rorStart, float rorEnd);
  void clear();
  float targetAt(unsigned long elapsedSeconds) const;
  float fanAt(unsigned long elapsedSeconds) const;
  float rorTargetAt(unsigned long elapsedSeconds) const;
  int stepIndexAt(unsigned long elapsedSeconds) const;
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
