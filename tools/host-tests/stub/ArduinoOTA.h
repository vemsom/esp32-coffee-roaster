#pragma once
// Host stub for ArduinoOTA. The callbacks are stored rather than dropped so a
// test can fire them exactly where the library would: that is how the test
// proves the transfer start latches the element off before a single byte has
// landed, and that a finished or failed transfer really reboots the device.
#include <Arduino.h>
#include <functional>
#include <string>

typedef enum {
  OTA_AUTH_ERROR = 0,
  OTA_BEGIN_ERROR,
  OTA_CONNECT_ERROR,
  OTA_RECEIVE_ERROR,
  OTA_END_ERROR
} ota_error_t;

class ArduinoOTAClass {
 public:
  void setHostname(const char *h) { hostname = h; }
  void setPort(uint16_t p) { port = p; }
  void setPassword(const char *p) { password = p; }

  void onStart(std::function<void()> f) { startFn = f; }
  void onEnd(std::function<void()> f) { endFn = f; }
  void onError(std::function<void(ota_error_t)> f) { errorFn = f; }

  void begin() { beginCount++; }
  void handle() { handleCount++; }

  // Test controls: the real library calls these from its own task.
  void fireStart() { if (startFn) startFn(); }
  void fireEnd() { if (endFn) endFn(); }
  void fireError(ota_error_t e) { if (errorFn) errorFn(e); }

  // Test observations.
  std::string hostname;
  std::string password;
  uint16_t port = 0;
  int beginCount = 0;
  int handleCount = 0;

 private:
  std::function<void()> startFn;
  std::function<void()> endFn;
  std::function<void(ota_error_t)> errorFn;
};

extern ArduinoOTAClass ArduinoOTA;
