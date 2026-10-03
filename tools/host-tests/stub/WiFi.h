#pragma once
// Host-side WiFi stub for compiling mqtt_client.cpp and the WiFi service in
// main.cpp without an ESP32. statusValue is writable so a test can put the
// link up or down, and beginCount records every connect attempt - that is how
// the host test proves setup() does not wait for the network and that the
// retry kick really fires.
#include <Arduino.h>

#define WL_IDLE_STATUS 0
#define WL_NO_SSID_AVAIL 1
#define WL_CONNECTED 3
#define WL_CONNECT_FAILED 4
#define WL_DISCONNECTED 6

#define WIFI_STA 1

class IPAddress {
 public:
  IPAddress(uint8_t a = 192, uint8_t b = 168, uint8_t c = 0, uint8_t d = 10)
      : _b{a, b, c, d} {}
  // The real class parses a dotted quad, which is how a test hands the stub a
  // caller address. Anything that is not four numbers becomes 0.0.0.0, so a
  // malformed address in a test fails the address check instead of silently
  // matching it.
  explicit IPAddress(const String &text) {
    _b[0] = _b[1] = _b[2] = _b[3] = 0;
    int part = 0, value = -1, digits = 0;
    for (size_t i = 0; i <= text.length(); i++) {
      const char c = (i < text.length()) ? text.charAt(i) : '.';
      if (c >= '0' && c <= '9' && digits < 3) {
        value = (value < 0 ? 0 : value) * 10 + (c - '0');
        digits++;
      } else if (c == '.') {
        if (part > 3 || digits == 0 || value > 255) break;
        _b[part++] = (uint8_t)value;
        value = -1;
        digits = 0;
      } else {
        break;
      }
    }
  }
  uint8_t operator[](int i) const { return _b[i]; }
  // The name the firmware calls, and the name the real class has.
  String toString() const {
    char out[16];
    snprintf(out, sizeof(out), "%u.%u.%u.%u", _b[0], _b[1], _b[2], _b[3]);
    return String(out);
  }

 private:
  uint8_t _b[4];
};

class WiFiClient {};

class WiFiClass {
 public:
  int begin(const char *, const char *) {
    beginCount++;
    return 0;  // non-blocking on the real chip too: the STA connect is started
               // and the caller returns without waiting for an association
  }
  void mode(int) {}
  void setAutoReconnect(bool) {}
  int status() { return statusValue; }
  IPAddress localIP() { return IPAddress(192, 168, 0, 10); }
  int32_t RSSI() { return -55; }

  // Test controls / observations.
  int statusValue = WL_CONNECTED;
  int beginCount = 0;
};

extern WiFiClass WiFi;
