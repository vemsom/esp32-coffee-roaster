#pragma once
#include <Arduino.h>

// MQTT bridge with Home Assistant MQTT discovery - REPORT-ONLY.
//
// The roaster publishes state and nothing else: no command topic is subscribed
// to, no entity carries a command_topic, and the device cannot be started,
// stopped or adjusted over MQTT. All control lives in the web UI (and in the
// safety layer). See docs/firmware-notes.md.
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
};

// No-op (MQTT stays disabled, but the firmware runs) when MQTT_HOST is "TBD".
void mqtt_init(MqttCallbacks callbacks);
void mqtt_update();          // call from loop()
void mqtt_publish_status();  // force an immediate status publish
bool mqtt_connected();
