#pragma once
#include <Arduino.h>

// MQTT bridge with Home Assistant MQTT discovery. Same pattern as the web
// server: the module never touches main.cpp internals, it goes through this
// callback struct.
struct MqttCallbacks {
  // Status getters
  float (*getBT)();
  float (*getET)();
  float (*getHeaterDuty)();
  int   (*getFanSpeed)();
  const char *(*getMode)();          // "idle" | "manual" | "profile" | "cool"
  const char *(*getProfileName)();   // currently selected profile, "" if none
  unsigned long (*getElapsedSeconds)();
  bool  (*getSafetyFault)();
  const char *(*getSafetyReason)();
  bool  (*getRoastActive)();
  // Fills up to maxOut names, returns how many. Used for the select entity.
  int   (*listProfiles)(String *out, int maxOut);

  // Commands
  void (*setFanSpeed)(int percent);
  void (*setHeaterDuty)(float percent);
  void (*setSelectedProfile)(const String &name);
  bool (*startRoast)(const String &profileName);
  void (*stopRoast)();
};

// No-op (MQTT stays disabled, but the firmware runs) when MQTT_HOST is "TBD".
void mqtt_init(MqttCallbacks callbacks);
void mqtt_update();          // call from loop()
void mqtt_publish_status();  // force an immediate status publish
bool mqtt_connected();
