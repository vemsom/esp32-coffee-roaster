#pragma once
#include <Arduino.h>

// Callbacks from main.cpp so the web server can talk to the rest of the
// system without web_server.cpp needing to know about main.cpp internals.
struct WebServerCallbacks {
  // Status getters
  float (*getBT)();
  float (*getET)();
  float (*getRorBt)();    // rate of rise, C/min (see include/ror.h)
  float (*getRorEt)();    // rate of rise, C/min, negative while cooling
  float (*getHeaterDuty)();
  int   (*getFanSpeed)();
  bool  (*getRoastActive)();
  unsigned long (*getElapsedSeconds)();
  bool  (*getRoastPaused)();
  bool  (*getManualActive)();
  float (*getManualTargetTemp)();
  unsigned long (*getManualRemainingSeconds)();
  bool  (*getManualAutoCool)();
  bool  (*getCoolActive)();
  int   (*getCoolSpeed)();
  unsigned long (*getCoolRemainingSeconds)();
  bool  (*getSafetyFault)();
  const char *(*getSafetyReason)();
  bool  (*getFanFault)();   // fan interlock: heat withheld, fan below minimum
  bool  (*getWifiConnected)();  // device-side WiFi state, shown in the UI
  int   (*getRorGuidance)();    // index of the RoR-driven step, -1 = none
  float (*getRorTarget)();      // RoR target in C/min
  float (*getRorError)();       // RoR target - rorEt
  bool  (*getRorActive)();      // true when correction is being applied
  bool  (*isRunActive)();       // roast, manual or cooling - push OTA refuses then

  // Commands
  void (*setFanSpeed)(int percent);
  bool (*startManual)(float targetTemp, unsigned long durationSeconds,
                      bool autoCool, int coolSpeed, unsigned long coolSeconds);
  void (*stopManual)();
  bool (*startCool)(int speed, unsigned long durationSeconds);
  void (*stopCool)();
  bool (*startRoast)(const String &profileName);
  void (*stopRoast)();
  void (*pauseRoast)();
  void (*resumeRoast)();
};

void web_server_init(WebServerCallbacks callbacks);

// True once a push OTA upload has been written and verified: main.cpp restarts
// on it, from loop(), so the HTTP response gets out first.
bool web_ota_reboot_pending();

// Called by main.cpp: false while the filesystem partition is being rewritten
// by a filesystem OTA transfer. web_server.cpp provides a weak "true" default
// and main.cpp overrides it, so the server can be linked without main.
bool web_fs_available();
