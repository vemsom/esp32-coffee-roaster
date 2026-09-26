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
  IPAddress(uint8_t a = 192, uint8_t b = 168, uint8_t c = 1, uint8_t d = 173)
      : _b{a, b, c, d} {}
  uint8_t operator[](int i) const { return _b[i]; }

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
  IPAddress localIP() { return IPAddress(192, 168, 1, 173); }
  int32_t RSSI() { return -55; }

  // Test controls / observations.
  int statusValue = WL_CONNECTED;
  int beginCount = 0;
};

extern WiFiClass WiFi;
