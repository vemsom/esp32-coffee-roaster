#include "ota_push.h"
#include "config.h"
#include "state_lock.h"
#include <Update.h>
#include <mbedtls/sha256.h>

// ---------------------------------------------------------------------------
// Push-OTA. See include/ota_push.h for why this exists and what the rules
// are; this file is the state machine.
//
// The body arrives in chunks (ESPAsyncWebServer hands the whole request to one
// body handler), and Update.write() is called once per chunk. Nothing here
// touches NVS, LittleFS or the profiles: Update writes to the inactive OTA
// slot, and a transfer that is refused, fails or does not verify leaves that
// slot dirty while the running image (and everything in it) stays exactly as
// it was.
//
// SHA-256 runs on EVERY byte as it arrives, on the device. That is what makes
// the checksum worth anything: a hash the uploader sends and only the uploader
// checks proves nothing, and the token that guards this route travels in clear
// text over Wi-Fi.
// ---------------------------------------------------------------------------

static OtaPushCallbacks cb;
static bool enabled = false;
static bool inProgress = false;
static bool authorized = false;
static size_t expectedTotal = 0;
static size_t written = 0;
static char deviceHash[OTA_SHA256_HEX_LEN + 1] = {0};
static char expectedHash[OTA_SHA256_HEX_LEN + 1] = {0};
static bool lastTransferRejected = false;
static mbedtls_sha256_context sha;

static void hashReset() { mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0); }
static void hashUpdate(const uint8_t *data, size_t len) { mbedtls_sha256_update(&sha, data, len); }

// Hex, lower case, so the header comparison is a plain string compare.
static void hashFinish(char *out) {
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  for (size_t i = 0; i < sizeof(digest); i++) {
    snprintf(out + i * 2, 3, "%02x", digest[i]);
  }
}

static bool isLowerHex(const char *s, size_t len) {
  if (s == nullptr || strlen(s) != len) return false;
  for (size_t i = 0; i < len; i++) {
    const char c = s[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!ok) return false;
  }
  return true;
}

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
  Serial.print("[OTA] push endpoint armed, client ");
  Serial.print(OTA_ALLOWED_CLIENT_IP);
  Serial.println(" only");
}

bool ota_push_enabled() { return enabled; }

// The identity test on its own, without any of the window and size checks:
// used by the GET probe, which only wants to know whether the caller may see
// the truth about this route.
bool ota_push_probe_trusted(const char *allowedClient, const char *token, const char *sha256) {
  if (!enabled) return false;
  if (allowedClient == nullptr || strcmp(allowedClient, OTA_ALLOWED_CLIENT_IP) != 0) return false;
  if (strcmp(token == nullptr ? "" : token, OTA_TOKEN) != 0) return false;
  return isLowerHex(sha256, OTA_SHA256_HEX_LEN);
}

OtaPushDecision ota_push_begin(const char *allowedClient, const char *token,
                               const char *sha256, size_t total) {
  // Order matters. Anything that is not the allowed client - or that arrives
  // while the endpoint is switched off - gets the same answer as a path that
  // does not exist, so a probe cannot map the firmware route at all.
  if (!enabled) return OtaPushHidden;

  if (allowedClient == nullptr || strcmp(allowedClient, OTA_ALLOWED_CLIENT_IP) != 0) {
    // Logged, not answered: the client sees a 404 like any unknown path.
    Serial.print("[OTA] push attempt from ");
    Serial.print(allowedClient ? allowedClient : "(none)");
    Serial.print(" refused - allowed client is ");
    Serial.println(OTA_ALLOWED_CLIENT_IP);
    return OtaPushHidden;
  }

  // Same principle for a bad token, and for a header that is not a SHA-256 at
  // all: no clue about what this route wants.
  if (strcmp(token == nullptr ? "" : token, OTA_TOKEN) != 0 ||
      !isLowerHex(sha256, OTA_SHA256_HEX_LEN)) {
    Serial.println("[OTA] push attempt with an invalid token or checksum header");
    return OtaPushHidden;
  }

  // Past this point the caller is the trusted uploader, so it gets a real
  // answer instead of a decoy.
  //
  // The window: no run active AND the element not asking for power. Not "roast
  // not running" - a manual run, a cool-down and a heater that is still
  // conducting all count, and the element is the thing that must never be live
  // during a flash.
  if ((cb.isRunActive && cb.isRunActive()) || (cb.isHeaterAsking && cb.isHeaterAsking())) {
    return OtaPushBusy;
  }

  if (total == 0) return OtaPushBadRequest;
  if (total > OTA_PUSH_MAX_BYTES) return OtaPushTooLarge;

  if (!Update.begin(total)) return OtaPushUpdateBeginFailed;

  // Update now owns the inactive OTA slot. The same invariants that
  // ArduinoOTA.onStart() enforces must hold here: the element must be latched
  // off and any active run aborted before the first byte of the image lands.
  // A windowed heater left conducting would stay on for the whole transfer,
  // because no control cycle runs while the body is being written.
  if (cb.latchHeaterOff) cb.latchHeaterOff();
  if (cb.abortRunForSafety) cb.abortRunForSafety();

  authorized = true;
  inProgress = true;
  lastTransferRejected = false;
  expectedTotal = total;
  written = 0;
  snprintf(expectedHash, sizeof(expectedHash), "%s", sha256);
  deviceHash[0] = '\0';
  hashReset();
  return OtaPushOk;
}

size_t ota_push_write(uint8_t *data, size_t len) {
  if (!inProgress || !authorized) return 0;
  if (written + len > expectedTotal) return 0;
  hashUpdate(data, len);
  size_t n = Update.write(data, len);
  written += n;
  return n;
}

// Called exactly once, when the last chunk has been handed over (or when the
// request died early). A verified image only becomes the running one after a
// restart, so this function decides whether there is anything to restart into.
bool ota_push_finished(bool complete) {
  bool verified = false;

  if (inProgress && authorized) {
    hashFinish(deviceHash);
    const bool hashOk = (strcmp(deviceHash, expectedHash) == 0);

    if (complete && written == expectedTotal && hashOk) {
      verified = Update.end(true);
    } else {
      // Anything else - a short transfer, a connection that died, a checksum
      // that does not match - throws the written data away. The image only
      // becomes the boot target through Update.end(), so after an abort the
      // chip boots exactly what it booted before: a half-written or
      // unverified image cannot leave the device unusable.
      Serial.print("[OTA] push rejected (");
      Serial.print(written);
      Serial.print("/");
      Serial.print(expectedTotal);
      Serial.print(" bytes, hash ");
      Serial.print(hashOk ? "ok" : "MISMATCH");
      Serial.println(") - written data discarded, current image stays");
      Update.abort();
      lastTransferRejected = true;
    }
  }

  inProgress = false;
  authorized = false;
  return verified;
}

bool ota_push_in_progress() { return inProgress; }
size_t ota_push_bytes_written() { return written; }
const char *ota_push_sha256_hex() { return deviceHash; }
bool ota_push_last_transfer_rejected() { return lastTransferRejected; }
