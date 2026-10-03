#include "ota_push.h"
#include "config.h"
#include "state_lock.h"
#include <Update.h>

// ---------------------------------------------------------------------------
// Push-OTA. See include/ota_push.h for why this exists and what the auth
// rule is; this file is the state machine.
//
// The body arrives in chunks (ESPAsyncWebServer hands the whole request to one
// body handler), and Update.write() is called once per chunk. Nothing here
// touches NVS, LittleFS or the profiles: Update writes to the inactive OTA
// slot, and a transfer that is refused or fails simply leaves that slot dirty
// - the running image, the filesystem and everything in it are untouched.
// ---------------------------------------------------------------------------

static OtaPushCallbacks cb;
static bool enabled = false;
static bool inProgress = false;
static bool authorized = false;
static size_t expectedTotal = 0;
static size_t written = 0;

void ota_push_init(OtaPushCallbacks callbacks) {
  cb = callbacks;

  // An empty token means no token was configured, and an endpoint that
  // accepts an unauthenticated firmware upload is worse than no endpoint.
  // Same shape as mqtt_init(): refuse to enable, say why on the serial log.
  if (!OTA_PUSH_ENABLED) {
    enabled = false;
    Serial.println("[OTA] OTA_TOKEN not set in include/secrets.h - push OTA disabled");
    return;
  }

  enabled = true;
}

bool ota_push_enabled() { return enabled; }

bool ota_push_authorized(const char *token) {
  if (!enabled || token == nullptr) return false;
  return strcmp(token, OTA_TOKEN) == 0;
}

// Everything the endpoint must know before it reads a single byte.
OtaPushDecision ota_push_begin(const char *token, size_t total) {
  if (!enabled) return OtaPushDisabled;
  if (strcmp(token == nullptr ? "" : token, OTA_TOKEN) != 0) return OtaPushUnauthorized;

  // Refused while the machine is doing something. Flashing takes the device
  // down for a few seconds and the run would be lost mid-roast; worse, this
  // is exactly when the element is conducting.
  if (cb.isRunActive && cb.isRunActive()) return OtaPushBusy;

  if (total == 0) return OtaPushBadRequest;
  if (total > OTA_PUSH_MAX_BYTES) return OtaPushTooLarge;

  if (!Update.begin(total)) return OtaPushUpdateBeginFailed;

  authorized = true;
  inProgress = true;
  expectedTotal = total;
  written = 0;
  return OtaPushOk;
}

size_t ota_push_write(uint8_t *data, size_t len) {
  if (!inProgress || !authorized) return 0;
  if (written + len > expectedTotal) return 0;
  size_t n = Update.write(data, len);
  written += n;
  return n;
}

// Called exactly once, when the last chunk has been handed over (or when the
// request died early). A finished image only becomes the running one after a
// restart, which the endpoint triggers - main.cpp does that via cb.restart,
// because the endpoint itself must not return before the response is sent.
bool ota_push_finished(bool complete) {
  bool ok = false;
  if (inProgress && authorized && complete && written == expectedTotal) {
    ok = Update.end(true);
  } else if (inProgress) {
    Update.abort();
  }
  inProgress = false;
  authorized = false;
  return ok;
}

bool ota_push_in_progress() { return inProgress; }
size_t ota_push_bytes_written() { return written; }
