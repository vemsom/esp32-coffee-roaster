#pragma once
// Host stub for the Arduino Update library. ota_push.cpp calls exactly four
// methods, and the stub records what they were called with, so a test can
// prove: an accepted transfer writes exactly Content-Length bytes, a refused
// one writes nothing, and no path ever reaches NVS (the stub has no NVS to
// reach, which is the point - see test_ota_push.cpp).
#include <Arduino.h>
#include <cstring>
#include <vector>

class UpdateClass {
 public:
  bool begin(size_t size = 0, int command = 0, int ledPin = -1, uint8_t ledOn = 0,
             const char *label = nullptr) {
    (void)command; (void)ledPin; (void)ledOn; (void)label;
    beginCalls++;
    if (failBegin) return false;
    _size = size;
    _progress = 0;
    _begun = true;
    _finished = false;
    bytes.clear();
    return true;
  }

  size_t write(uint8_t *data, size_t len) {
    if (!_begun) return 0;
    writeCalls++;
    if (_progress + len > _size) return 0;
    bytes.insert(bytes.end(), data, data + len);
    _progress += len;
    return len;
  }

  bool end(bool evenIfRemaining = false) {
    (void)evenIfRemaining;
    endCalls++;
    if (failEnd) return false;
    _begun = false;
    _finished = true;
    return true;
  }

  void abort() {
    abortCalls++;
    _begun = false;
  }

  bool isFinished() { return _progress == _size && _size > 0; }
  size_t progress() { return _progress; }

  // ---- what the test reads back / sets up ----
  void reset() {
    _size = 0; _progress = 0; _begun = false; _finished = false;
    beginCalls = writeCalls = endCalls = abortCalls = 0;
    bytes.clear();
    failBegin = failEnd = false;
  }

  bool failBegin = false;
  bool failEnd = false;
  int beginCalls = 0, writeCalls = 0, endCalls = 0, abortCalls = 0;
  std::vector<uint8_t> bytes;

 private:
  size_t _size = 0;
  size_t _progress = 0;
  bool _begun = false;
  bool _finished = false;
};

extern UpdateClass Update;
