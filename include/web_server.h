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
  int   (*getFanSpeed)();   // the REQUESTED speed (what HA and the UI show)
  // The speed the fan has actually reached (the ramp, FAN_SLEW_PCT_PER_SEC).
  // Reported next to the request so "asked for 100 %, still at 0 %" - the state
  // the heat interlock exists for - is visible from outside.
  float (*getFanActual)();
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
  float (*getRorError)();       // RoR target minus the measured rate (see ROR_GUIDANCE_ON_BT)
  // The correction the guidance is applying RIGHT NOW, in percent (signed). It
  // is the number the rate limit acts on, and since 0.15.0 it can no longer be
  // read off the heater duty: the duty is the feed-forward + the (bipolar) PID
  // + this, clipped, so the guidance is only one part of it - and in a cold-probe
  // transient the other two pin the sum at 0 % while this climbs.
  float (*getRorCorrection)();
  bool  (*getRorActive)();      // true when correction is being applied
  // PID diagnostics, so the loop can be trimmed against a machine that is
  // running (see include/pid.h): the three contributions as last applied
  // (percent), the raw integral state (C*s), and the filtered bean temperature
  // the PID actually read - the control input, not the value the safety layer
  // and the UI see.
  float (*getPidP)();
  float (*getPidI)();
  float (*getPidD)();
  float (*getPidIntegral)();
  float (*getFilteredBT)();
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
  // Stops the roast AND starts the fan-only cool-down in one call, under one
  // state lock: the fan must not pass through "off" between the two, which is
  // what two separate requests leave room for. Returns false (and changes
  // nothing) when the values are out of range.
  bool (*stopRoastAndCool)(int speed, unsigned long durationSeconds);
  void (*pauseRoast)();
  void (*resumeRoast)();
};

void web_server_init(WebServerCallbacks callbacks);

// True once a push OTA upload has been written and verified: main.cpp restarts
// on it, from loop(), so the HTTP response gets out first.
bool web_ota_reboot_pending();

// Called by main.cpp: false while the filesystem partition is being rewritten
// by a filesystem OTA transfer (ArduinoOTA's onStart() or the push route).
// web_server.cpp provides a weak default that already refuses while a
// filesystem push is running (ota_push_fs_unmounted()), and main.cpp overrides
// it with the same answer plus its ArduinoOTA flag, so the server can be linked
// without main.
bool web_fs_available();
