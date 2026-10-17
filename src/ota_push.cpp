#include "ota_push.h"
#include "config.h"
#include "state_lock.h"
#include <LittleFS.h>
#include <Update.h>
#include <mbedtls/sha256.h>

// ---------------------------------------------------------------------------
// Push-OTA. See include/ota_push.h for why this exists and what the rules
// are; this file is the state machine.
//
// The body arrives in chunks (ESPAsyncWebServer hands the whole request to one
// body handler), and Update.write() is called once per chunk. An app transfer
// touches nothing else: Update writes to the inactive OTA slot, and a transfer
// that is refused, fails or does not verify leaves that slot dirty while the
// running image (and everything in it) stays exactly as it was.
//
// A filesystem transfer is the exception, and it is the reason the target is
// tracked here: U_SPIFFS writes straight to the partition LittleFS lives on, so
// the filesystem has to be closed before the first byte and mounted again
// afterwards. A mounted filesystem holds its own metadata and buffers in RAM
// and would write its view of the same blocks over/behind the incoming image;
// the symptom is not a clean refusal but a transfer that starts, runs for a
// while and then dies. Same rule ArduinoOTA.onStart() follows.
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
static OtaPushTarget transferTarget = OtaPushTargetApp;
// True between LittleFS.end() and the remount. Kept here, not only in
// main.cpp, so the profile endpoints can refuse during a push without the
// server having to know how the transfer was started.
static bool fsUnmounted = false;
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
                               const char *sha256, size_t total,
                               OtaPushTarget target) {
  // A disabled endpoint accepts nothing from anyone, so saying so is not a
  // leak: there is no image it could be made to take. 503, not 404 - "the
  // service is off", which is what an empty OTA_TOKEN means.
  if (!enabled) return OtaPushDisabled;

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
  // One transfer at a time: a second body arriving while the first is still
  // being written would interleave its chunks into the same Update and, on the
  // filesystem route, unmount a partition that is already being rewritten. 409
  // is the honest answer - "busy, try again", the same code the window below
  // uses.
  if (inProgress) return OtaPushBusy;

  // The window: no run active AND the element not asking for power. Not "roast
  // not running" - a manual run, a cool-down and a heater that is still
  // conducting all count, and the element is the thing that must never be live
  // during a flash.
  if ((cb.isRunActive && cb.isRunActive()) || (cb.isHeaterAsking && cb.isHeaterAsking())) {
    return OtaPushBusy;
  }

  if (total == 0) return OtaPushBadRequest;

  // The size window belongs to the target: an app image may not exceed the OTA
  // slot, a filesystem image may not exceed the filesystem partition - and a
  // filesystem image smaller than a filesystem is not one, so it is refused
  // before it can leave the partition unmountable.
  if (target == OtaPushTargetFs) {
    if (total < OTA_PUSH_FS_MIN_BYTES) return OtaPushTooSmall;
    if (total > OTA_PUSH_FS_MAX_BYTES) return OtaPushTooLarge;
  } else if (total > OTA_PUSH_MAX_BYTES) {
    return OtaPushTooLarge;
  }

  // The command is what decides which partition Update opens - U_FLASH for the
  // inactive app slot, U_SPIFFS for the filesystem partition. It comes from the
  // route, never from the request.
  const int command = (target == OtaPushTargetFs) ? U_SPIFFS : U_FLASH;
  if (!Update.begin(total, command)) return OtaPushUpdateBeginFailed;

  // Update now owns the target partition. The same invariants that
  // ArduinoOTA.onStart() enforces must hold here: the element must be latched
  // off and any active run aborted before the first byte of the image lands.
  // A windowed heater left conducting would stay on for the whole transfer,
  // because no control cycle runs while the body is being written.
  if (cb.latchHeaterOff) cb.latchHeaterOff();
  if (cb.abortRunForSafety) cb.abortRunForSafety();

  // Filesystem transfer: close the filesystem before a single byte of the new
  // image is written. A mounted LittleFS keeps its own view of the blocks that
  // are about to be rewritten, and the transfer dies part-way when it does.
  // Unlike the app slot, this partition is the one /profiles lives on, so the
  // read path has to know: ota_push_fs_unmounted() turns the profile endpoints
  // into 503s until the transfer is over.
  if (target == OtaPushTargetFs) {
    LittleFS.end();
    fsUnmounted = true;
    Serial.println("[OTA] LittleFS unmounted for the filesystem transfer");
  }

  transferTarget = target;
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

// Put the filesystem back after a transfer. Leaving it unmounted turns every
// profile call into a 503 until the next boot, and a transfer can end without
// a reboot (a rejected body still reaches finished()). After a verified image
// the partition holds a complete filesystem and mounts as it is; after a
// failed one it is half written, and formatting is the only way back to
// something mountable - the same thing ArduinoOTA.onError() does. If the mount
// fails the flag stays set, so the endpoints keep saying 503 instead of
// serving whatever is there, and the restart that follows mounts it at boot.
static void fsRestore(bool failed) {
  if (!fsUnmounted) return;
  const bool mounted = LittleFS.begin(failed);
  if (mounted) fsUnmounted = false;
  Serial.println(mounted
                     ? "[OTA] LittleFS mounted again after the filesystem transfer"
                     : "[OTA] LittleFS could not be mounted again after the filesystem transfer");
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

  // Only a filesystem transfer ever unmounted anything, so this is a no-op on
  // the app path - which is exactly what "the app route behaves as it did"
  // means here.
  fsRestore(!verified);

  inProgress = false;
  authorized = false;
  return verified;
}

bool ota_push_in_progress() { return inProgress; }
OtaPushTarget ota_push_target() { return transferTarget; }
size_t ota_push_bytes_written() { return written; }
const char *ota_push_sha256_hex() { return deviceHash; }
bool ota_push_last_transfer_rejected() { return lastTransferRejected; }
bool ota_push_fs_unmounted() { return fsUnmounted; }
