#pragma once
// ---------------------------------------------------------------------------
// One lock for the state that src/main.cpp owns and that two tasks touch:
//
//   * loop() runs the sensor sample, the safety latch, the PID and the mode
//     state machine on the Arduino loop task;
//   * every web callback in main.cpp runs on the AsyncTCP task, because
//     ESPAsyncWebServer dispatches its handlers there.
//
// Before this existed, both tasks wrote controlMode / currentFanSpeed /
// currentHeaterDuty / the cool and manual timers with nothing in between: a
// clipped value or a refused start at best, a torn read of the profile name
// at worst.
//
// The lock is recursive on purpose. A command callback legitimately calls
// other command callbacks (cbStopManual -> maybeStartAutoCool ->
// cbStartCool), and the loop task takes it once per sample around code that
// calls those same helpers. Lock order is always: state lock first, then NVS
// or LittleFS if a callback needs them. Nothing that takes NVS or the
// filesystem ever comes back for the state lock, so there is no cycle.
//
// Deliberately NOT held by mqtt_update(): it can block for seconds in a
// synchronous TCP connect, and freezing every HTTP request for that long
// would be worse than the inconsistency it prevents. It reads through the
// same callbacks, which take the lock themselves - each getter is a
// consistent single read, though a status payload can straddle a change.
//
// The branch is chosen by whether the FreeRTOS headers exist, not by a
// platform macro: main.cpp is compiled by both PlatformIO and the host test
// suite, and only the ESP32 build has freertos/ on its include path.
// ---------------------------------------------------------------------------
#if defined(__has_include) && __has_include(<freertos/FreeRTOS.h>)
#define STATE_LOCK_ON_FREERTOS 1
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#else
#define STATE_LOCK_ON_FREERTOS 0
#include <mutex>
#endif

class StateLock {
 public:
  StateLock() {
#if STATE_LOCK_ON_FREERTOS
    _sem = xSemaphoreCreateRecursiveMutex();
#endif
  }

  void lock() {
#if STATE_LOCK_ON_FREERTOS
    if (_sem) xSemaphoreTakeRecursive(_sem, portMAX_DELAY);
#else
    _mutex.lock();
#endif
    // Counted while the lock is held, so it needs no atomics on either side.
    acquisitions()++;
  }

  void unlock() {
#if STATE_LOCK_ON_FREERTOS
    if (_sem) xSemaphoreGiveRecursive(_sem);
#else
    _mutex.unlock();
#endif
  }

  // Diagnostic: how many times the lock has been taken. The host test uses it
  // to prove that the web callbacks really do take it, which is otherwise
  // invisible from a single-threaded test.
  static unsigned long &acquisitions() {
    static unsigned long n = 0;
    return n;
  }

 private:
#if STATE_LOCK_ON_FREERTOS
  SemaphoreHandle_t _sem = nullptr;
#else
  std::recursive_mutex _mutex;
#endif
};

// One instance for the whole firmware: a function-local static inside an
// inline function is shared by every translation unit (C++ ODR).
inline StateLock &stateLock() {
  static StateLock lock;
  return lock;
}

inline unsigned long state_lock_acquisitions() { return StateLock::acquisitions(); }

// RAII: every scope that touches shared state opens one of these.
class StateLockGuard {
 public:
  StateLockGuard() { stateLock().lock(); }
  ~StateLockGuard() { stateLock().unlock(); }
  StateLockGuard(const StateLockGuard &) = delete;
  StateLockGuard &operator=(const StateLockGuard &) = delete;
};
