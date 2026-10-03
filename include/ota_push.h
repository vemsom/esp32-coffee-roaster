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
// The endpoint lives in the server that is already there (port 80), because
// that path is the one proven to work - a new listening port would be an
// unproven one. It then has to defend itself on its own merits, since a
// permanently listening HTTP route is not the same thing as a dial-back
// window that only exists during an upload:
//
//   1. The request must come from OTA_ALLOWED_CLIENT_IP (secrets.h, default
//      192.168.1.x - the machine that runs the upload). A valid token is not
//      enough on its own: IoT devices reach each other inside the IoT VLAN,
//      so the network does not isolate this endpoint.
//   2. A wrong token, or a request arriving at all outside the window, answers
//      404 rather than 401 - a client that probes must not be able to tell
//      that the route exists. 403 is reserved for "you are not the allowed
//      client", which is only ever sent to an address that is already trusted
//      by nothing else happening here.
//   3. The image is hashed ON THE DEVICE (SHA-256) and compared with the
//      X-OTA-SHA256 header before the first byte of the image is accepted, and
//      again at the end. The token travels in clear text over Wi-Fi - it is a
//      gate, the checksum is the protection.
//   4. Refused unless BOTH "no run active" AND "heater not asking for power".
//      Not "roast not running" - the element is the thing that must never be
//      conducting during a flash.
//   5. A transfer that does not verify, or that dies half-written, never
//      becomes the boot image: Update.abort() throws the written data away and
//      the current image keeps booting. See ota_push.cpp.
//
// An empty token disables the endpoint completely - same principle as MQTT
// (no user -> disabled) and ArduinoOTA (no password -> not started). There is
// no fallback and no default: an unauthenticated POST must never be able to
// replace the firmware.
// ---------------------------------------------------------------------------

#include "config.h"

#ifndef OTA_TOKEN
#define OTA_TOKEN ""
#endif

// The only client allowed to replace the firmware. Overridable from
// secrets.h, and by -D in a host test.
#ifndef OTA_ALLOWED_CLIENT_IP
#define OTA_ALLOWED_CLIENT_IP "192.168.1.x"
#endif

#define OTA_PUSH_ENABLED (sizeof(OTA_TOKEN) > 1)

// The ESP32 OTA slot is 1.25 MB; anything larger cannot be a firmware image
// for this board and is refused before a byte is written.
#define OTA_PUSH_MAX_BYTES (1400000u)

// SHA-256 as hex: 64 characters, plus the terminator.
#define OTA_SHA256_HEX_LEN 64

#include <Arduino.h>

enum OtaPushDecision {
  OtaPushOk = 0,
  OtaPushDisabled,           // no OTA_TOKEN configured
  OtaPushHidden,             // not the allowed client, or not the right window: answer 404
  OtaPushUnauthorized,       // no/bad X-OTA-SHA256: answer 403
  OtaPushBusy,               // a run is active or the element is asking for power
  OtaPushBadRequest,         // no usable Content-Length
  OtaPushTooLarge,           // larger than the OTA slot
  OtaPushUpdateBeginFailed,  // Update.begin() refused (slot, size)
  OtaPushHashMismatch,       // only known at the end of the transfer
};

struct OtaPushCallbacks {
  bool (*isRunActive)();      // roast / manual / cooling
  bool (*isHeaterAsking)();   // the element is asking for power right now
  uint32_t (*checksumMs)();   // how long hashing may take, before the body is read
  void (*latchHeaterOff)();   // emergency-off the element before bytes land
  void (*abortRunForSafety)(); // stop any run before the transfer starts
};

void ota_push_init(OtaPushCallbacks callbacks);

// Is the endpoint usable at all (a token is configured)? False makes the
// route invisible, not just unauthorised.
bool ota_push_enabled();

// May this caller be told the truth (a real error code) instead of 404? True
// only for the allowed client with the right token and a well-formed checksum.
// Shared by the POST handler and the GET probe so both hide the route from
// exactly the same callers.
bool ota_push_probe_trusted(const char *allowedClient, const char *token, const char *sha256);

// Decision for an incoming request. allowedClient is the remote IP as a
// string; token and sha256 are the two headers. Everything is checked here,
// before a single byte of the body is read.
OtaPushDecision ota_push_begin(const char *allowedClient, const char *token,
                               const char *sha256, size_t total);

size_t ota_push_write(uint8_t *data, size_t len);

// Complete == the last chunk arrived. Returns false if the image does not
// verify, in which case the written data has been discarded.
bool ota_push_finished(bool complete);

bool ota_push_in_progress();
size_t ota_push_bytes_written();
const char *ota_push_sha256_hex();  // hex of what was actually received

// True when the last transfer was thrown away because it did not verify.
bool ota_push_last_transfer_rejected();
