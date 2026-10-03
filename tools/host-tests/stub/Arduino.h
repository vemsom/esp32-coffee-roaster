#pragma once
// Minimal Arduino stub so safety.cpp, heater_control.cpp and mqtt_client.cpp can
// be compiled and exercised on the host (no ESP32 needed).
// Note on ArduinoJson: this String is not a registered ArduinoJson type - it
// does not need to be. ArduinoJson's generic string adapter accepts anything
// with c_str() and length(), so doc["x"] = someString works the same way the
// real Arduino String does. What it cannot do on its own is be a serializeJson
// DESTINATION, so String carries the two write() entry points that
// ArduinoJson's generic Writer drives - see below.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using std::isnan;
using std::isinf;
using std::fabs;

typedef uint8_t byte;

#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int digitalRead(int pin);
unsigned long millis();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);

// LEDC PWM + map(), used by fan_control.cpp.
void ledcSetup(int channel, double freq, int resolution);
void ledcAttachPin(int pin, int channel);
void ledcWrite(int channel, int duty);
long map(long x, long inMin, long inMax, long outMin, long outMax);

struct SerialStub {
  void begin(unsigned long) {}
  template <typename T> void print(T) {}
  template <typename T> void println(T) {}
  void println() {}
  void printf(const char *, ...) {}
};
extern SerialStub Serial;

// Restart, reached from the OTA path: a finished or failed transfer reboots
// into a clean state (and clears the heater latch the transfer start set).
// The host stub counts the calls instead of taking the process down.
class EspClass {
 public:
  void restart();
  int restartCount = 0;
};
extern EspClass ESP;

// The subset of the Arduino String API that the firmware actually uses.
class String {
 public:
  String() {}
  String(const char *s) : _s(s ? s : "") {}
  String(const std::string &s) : _s(s) {}

  String &operator=(const char *s) { _s = s ? s : ""; return *this; }

  void reserve(size_t n) { _s.reserve(n); }
  size_t length() const { return _s.size(); }
  bool isEmpty() const { return _s.empty(); }
  const char *c_str() const { return _s.c_str(); }
  // The real String has it, and index-wise reads are the only sane way to walk
  // one without copying it.
  char charAt(size_t i) const { return i < _s.size() ? _s[i] : '\0'; }
  char operator[](size_t i) const { return charAt(i); }

  // ArduinoJson's generic Writer serialises through these two, the same way
  // it drives a Print on the real target - without them serializeJson(doc,
  // out) has no destination (the real Arduino String is served by a
  // dedicated writer that host builds do not enable).
  size_t write(uint8_t c) { _s += (char)c; return 1; }
  size_t write(const uint8_t *buf, size_t n) {
    _s.append(reinterpret_cast<const char *>(buf), n);
    return n;
  }

  String &operator+=(char c) { _s += c; return *this; }
  String &operator+=(const char *s) { _s += (s ? s : ""); return *this; }
  String operator+(const char *s) const { return String(_s + (s ? s : "")); }
  String operator+(const String &o) const { return String(_s + o._s); }

  bool operator==(const String &o) const { return _s == o._s; }
  bool operator==(const char *s) const { return _s == (s ? s : ""); }
  bool operator!=(const String &o) const { return !(*this == o); }
  bool operator!=(const char *s) const { return !(*this == s); }

  // Replaces every occurrence, like the Arduino original.
  void replace(const String &find, const String &repl) {
    if (find._s.empty()) return;
    size_t pos = 0;
    while ((pos = _s.find(find._s, pos)) != std::string::npos) {
      _s.replace(pos, find._s.size(), repl._s);
      pos += repl._s.size();
    }
  }

  long toInt() const { return strtol(_s.c_str(), nullptr, 10); }
  float toFloat() const { return strtof(_s.c_str(), nullptr); }

  void trim() {
    size_t b = _s.find_first_not_of(" \t\r\n");
    size_t e = _s.find_last_not_of(" \t\r\n");
    _s = (b == std::string::npos) ? "" : _s.substr(b, e - b + 1);
  }

  bool endsWith(const String &suffix) const {
    if (suffix._s.size() > _s.size()) return false;
    return _s.compare(_s.size() - suffix._s.size(), suffix._s.size(), suffix._s) == 0;
  }
  bool equalsIgnoreCase(const String &other) const {
    if (_s.size() != other._s.size()) return false;
    for (size_t i = 0; i < _s.size(); i++) {
      if (tolower((unsigned char)_s[i]) != tolower((unsigned char)other._s[i])) return false;
    }
    return true;
  }

 private:
  std::string _s;
};
