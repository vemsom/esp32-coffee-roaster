#pragma once
#include <Arduino.h>

// Callbacks from main.cpp so the web server can talk to the rest of the
// system without web_server.cpp needing to know about main.cpp internals.
struct WebServerCallbacks {
  float (*getBT)();
  float (*getET)();
  float (*getHeaterDuty)();
  int   (*getFanSpeed)();
  bool  (*getRoastActive)();
  unsigned long (*getElapsedSeconds)();

  void (*setFanSpeed)(int percent);
  void (*setManualHeaterDuty)(float percent);
  bool (*startRoast)(const String &profileName);
  void (*stopRoast)();
};

void web_server_init(WebServerCallbacks callbacks);
