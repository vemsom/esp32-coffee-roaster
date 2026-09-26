#pragma once
// Minimal Arduino stub so safety.cpp, heater_control.cpp and mqtt_client.cpp can
// be compiled and exercised on the host (no ESP32 needed).
// Note: this String is deliberately NOT registered with ArduinoJson - the
// production code only hands ArduinoJson plain char pointers and numbers, so a
// host build that breaks that rule fails to compile here too.
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

struct SerialStub {
  void begin(unsigned long) {}
  template <typename T> void print(T) {}
  template <typename T> void println(T) {}
  void println() {}
  void printf(const char *, ...) {}
};
extern SerialStub Serial;

// The subset of the Arduino String API that the firmware actually uses.
class String {
 public:
  String() {}
  String(const char *s) : _s(s ? s : "") {}
  String(const std::string &s) : _s(s) {}

  String &operator=(const char *s) { _s = s ? s : ""; return *this; }

  void reserve(size_t n) { _s.reserve(n); }
  size_t length() const { return _s.size(); }
  const char *c_str() const { return _s.c_str(); }

  String &operator+=(char c) { _s += c; return *this; }
  String &operator+=(const char *s) { _s += (s ? s : ""); return *this; }
  String operator+(const char *s) const { return String(_s + (s ? s : "")); }

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
