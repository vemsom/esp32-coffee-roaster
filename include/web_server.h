#pragma once
#include <Arduino.h>

// Callbacks from main.cpp so the web server can talk to the rest of the
// system without web_server.cpp needing to know about main.cpp internals.
struct WebServerCallbacks {
  // Status getters
  float (*getBT)();
  float (*getET)();
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
