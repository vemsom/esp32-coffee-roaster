#pragma once
// Host-side WiFi stub for compiling mqtt_client.cpp without an ESP32.
#include <Arduino.h>

#define WL_CONNECTED 3

class IPAddress {
 public:
  IPAddress(uint8_t a = 192, uint8_t b = 168, uint8_t c = 1, uint8_t d = 173)
      : _b{a, b, c, d} {}
  uint8_t operator[](int i) const { return _b[i]; }

 private:
  uint8_t _b[4];
};

class WiFiClient {};

class WiFiClass {
 public:
  int status() { return WL_CONNECTED; }
  IPAddress localIP() { return IPAddress(192, 168, 1, 173); }
  int32_t RSSI() { return -55; }
};

extern WiFiClass WiFi;
