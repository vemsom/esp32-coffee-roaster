#pragma once

// ---------------------------------------------------------------------------
// Push-OTA: the uploader connects to the roaster, not the other way round.
//
// Why: ArduinoOTA (the /ota path in main.cpp) needs the DEVICE to open a TCP
// connection back to whoever is uploading. In a VLAN-split home network new
// connections IoT -> LAN are blocked, so that handshake dies right after
// "Authenticating...OK". LAN -> IoT works fine, so the reliable direction is
// this one: the server POSTs the image to the roaster.
//
// Auth: the X-OTA-Token header must equal OTA_TOKEN from include/secrets.h.
// An empty token disables the endpoint completely - same principle as MQTT
// (no user -> disabled) and ArduinoOTA (no password -> not started). There is
// no fallback and no default: an unauthenticated POST must never be able to
// replace the firmware.
//
// Refused while a roast, a manual run or cooling is active, for the same
// reason serviceOta() refuses then: flashing while the element is conducting
// is exactly the situation the safety latch exists for. See ota_push.cpp.
// ---------------------------------------------------------------------------

#include "config.h"

#ifndef OTA_TOKEN
#define OTA_TOKEN ""
#endif

#define OTA_PUSH_ENABLED (sizeof(OTA_TOKEN) > 1)

// The ESP32 OTA slot is 1.25 MB; anything larger cannot be a firmware image
// for this board and is refused before a byte is written.
#define OTA_PUSH_MAX_BYTES (1400000u)

#include <Arduino.h>

enum OtaPushDecision {
  OtaPushOk = 0,
  OtaPushDisabled,           // no OTA_TOKEN configured
  OtaPushUnauthorized,       // wrong or missing X-OTA-Token
  OtaPushBusy,               // roast / manual / cooling running
  OtaPushBadRequest,         // no usable Content-Length
  OtaPushTooLarge,           // larger than the OTA slot
  OtaPushUpdateBeginFailed,  // Update.begin() refused (slot, size)
};

struct OtaPushCallbacks {
  bool (*isRunActive)();
};

void ota_push_init(OtaPushCallbacks callbacks);
bool ota_push_enabled();
bool ota_push_authorized(const char *token);
OtaPushDecision ota_push_begin(const char *token, size_t total);
size_t ota_push_write(uint8_t *data, size_t len);
bool ota_push_finished(bool complete);
bool ota_push_in_progress();
size_t ota_push_bytes_written();
