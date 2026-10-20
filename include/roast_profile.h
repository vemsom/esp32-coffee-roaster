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
  // Rate-of-rise guidance for this step, optional. rorTarget > 0 makes the
  // step RoR-driven; rorStart and rorEnd describe the target slope in C/min
  // at the beginning and end of the step. The curve is interpolated the same
  // way as temperature. Default 0 means RoR guidance is disabled for the step.
  float rorTarget = 0;
  float rorStart = 0;
  float rorEnd = 0;
};

// A roast profile: an ordered list of steps, always starting from a
// room-temperature start point (default 20 C at 0 s).
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
  // RoR target in C/min at elapsed seconds, or 0 if the current step has no
  // RoR guidance. Interpolates linearly over ramps and holds the end value
  // during holds, exactly like targetAt().
  float rorTargetAt(unsigned long elapsedSeconds) const;
  // Index of the step that is active at elapsed seconds, or -1 if none.
  int stepIndexAt(unsigned long elapsedSeconds) const;
  // Length of the whole profile: the sum of every step's ramp and hold. 0 for
  // an empty profile (and for a profile whose steps add up to 0 s), which
  // src/main.cpp reads as "this profile has no end to detect".
  unsigned long totalSeconds() const;
  // Cool-down that follows the profile by itself: fan-only, started when the
  // last step has run out (src/main.cpp). coolFanPct is 1-100 %, coolMinutes
  // is minutes. Both are 0 by default, and 0 (or anything below the limits in
  // include/config.h) means "no automatic cooling" - the profile ends the way
  // it always did, with the heat off and the fan stopped.
  float coolFanPct() const { return _coolFanPct; }
  float coolMinutes() const { return _coolMinutes; }
  // Clamped to the limits in include/config.h (0 = off, otherwise
  // COOL_SPEED_MIN_PCT..COOL_SPEED_MAX_PCT and COOL_MINUTES_MIN..MAX), so a
  // hand-edited profile cannot carry a value the UI could never produce.
  void setCool(float fanPct, float minutes);
  // The charge in the drum, in grams. 0 = not stated, which means "no duty
  // ceiling" - exactly the behaviour every profile had before the field
  // existed. Anything else is clamped to BEAN_WEIGHT_MIN_G..BEAN_WEIGHT_MAX_G
  // in setBeanWeight(), so a hand-edited profile cannot ask for a weight the
  // editor could not produce. See include/duty_ceiling.h for what it does.
  int beanWeight() const { return _beanWeight; }
  void setBeanWeight(int grams);
  bool hasFan() const { return _hasFan; }
  float startTemp() const { return _startTemp; }
  void setStartTemp(float t) { _startTemp = t; }
  size_t stepCount() const { return _steps.size(); }
  const std::vector<ProfileStep> &steps() const { return _steps; }

private:
  std::vector<ProfileStep> _steps;
  float _startTemp = 20;
  bool _hasFan = false;
  float _coolFanPct = 0;
  float _coolMinutes = 0;
  int _beanWeight = 0;   // grams, 0 = not stated (no duty ceiling)
};
