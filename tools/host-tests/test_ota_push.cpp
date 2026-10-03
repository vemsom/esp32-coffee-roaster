// Host-side test of push OTA: the REAL POST /api/update handler in
// src/web_server.cpp, the real state machine in src/ota_push.cpp, dispatched
// through the stubbed ESPAsyncWebServer onto the stubbed Update library.
//
// Why this exists: with the ArduinoOTA path the DEVICE had to connect back to
// the uploader, which a VLAN-split network blocks, so the upload path moved to
// the direction that works (uploader -> device, see include/ota_push.h). That
// makes a firmware replacement reachable over HTTP, which means the rules
// around it are now load-bearing:
//
//   * a wrong or missing token must be refused BEFORE Update.begin() - the
//     token is sent as a header on the first chunk, and the check happens in
//     the index == 0 branch, so no byte of an unauthorised image is ever
//     written
//   * a run in progress (roast / manual / cooling) must be refused for the
//     same reason serviceOta() refuses then
//   * an empty OTA_TOKEN on the device must disable the endpoint completely
//   * a complete transfer writes exactly Content-Length bytes and ends with
//     Update.end(), and the reboot is only requested after the response is
//     sent
//   * nothing here can touch NVS: the stub has none, and the only filesystem
//     in this binary is the profile LittleFS. A "transfer did not touch the
//     profiles" check is therefore possible on the real handler.
//
// Build: the test_ota_push line in run.sh -FW_LANG_EN=1 and OTA_TOKEN set to a
// throwaway value, so the endpoint is enabled exactly as it is on a device
// with a token in include/secrets.h. The token below is a literal in the test,
// never the real one.
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>

#include <string>
#include <vector>

#include "config.h"
#include "ota_push.h"
#include "web_server.h"

// The one filesystem in this binary (web_server.cpp and roast_profile.cpp
// reach it through these definitions, as in the other host tests).
LittleFSClass LittleFS;
UpdateClass Update;
SerialStub Serial;

static int failures = 0;
static int checks = 0;

static void check(bool cond, const char *what) {
  checks++;
  if (cond) {
    printf("ok   %s\n", what);
  } else {
    printf("FAIL %s\n", what);
    failures++;
  }
}
static void check(bool cond, const std::string &what) { check(cond, what.c_str()); }

// The token the test device is built with: it comes from -DOTA_TOKEN=... in
// run.sh and must match what the test sends. Never a real secret.
static const char *TEST_TOKEN = OTA_TOKEN;

// ---- driving the registered handler ---------------------------------------

static const AsyncWebServer::Route *findRoute(const char *uri,
                                              WebRequestMethodComposite method) {
  for (AsyncWebServer *server : asyncWebServers())
    for (const AsyncWebServer::Route &r : server->routes())
      if (r.uri == uri && r.method == method) return &r;
  return nullptr;
}

// A push is one POST body. dispatch() sends it the way ESPAsyncWebServer does:
// the token header is on the request, the image is the body, chunk by chunk
// with index/total. Token == nullptr sends no header at all.
static AsyncWebServerRequest dispatch(const AsyncWebServer::Route &route,
                                     const char *token,
                                     const std::vector<uint8_t> &image,
                                     size_t chunk = 0) {
  AsyncWebServerRequest request;
  if (token) request.addHeader("X-OTA-Token", String(token));
  const size_t size = chunk ? chunk : image.size();
  for (size_t off = 0; off < image.size(); off += size) {
    const size_t len = (off + size > image.size()) ? image.size() - off : size;
    route.onBody(&request, const_cast<uint8_t *>(image.data() + off), len, off, image.size());
    if (request.sent()) break;  // a refusal answers on the first chunk
  }
  return request;
}

static bool isStatus(const AsyncWebServerRequest &r, int code) {
  return r.sent() && r.responseCode() == code;
}
static bool bodyHas(const AsyncWebServerRequest &r, const char *needle) {
  return std::strstr(r.responseBody().c_str(), needle) != nullptr;
}

static int finish() {
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

// ---- the run-active predicate the endpoint asks ---------------------------

static bool runActive = false;
static bool isRunActive() { return runActive; }

int main() {
  printf("push OTA token in this build: %d characters\n", (int)strlen(TEST_TOKEN));

  // A profile, so "a transfer does not touch the filesystem" means something.
  const std::string profilePath = std::string(PROFILES_DIR) + "/espresso.json";
  const char *profileJson = "{\"startTemp\":180,\"steps\":[]}";
  littlefs_stub::files()[profilePath] = profileJson;

  ota_push_init(OtaPushCallbacks{isRunActive});

  WebServerCallbacks callbacks = {};
  callbacks.isRunActive = isRunActive;
  web_server_init(callbacks);

  const AsyncWebServer::Route *update = findRoute("/api/update", HTTP_POST);
  check(update != nullptr, "POST /api/update is registered");
  if (!update) return finish();

  std::vector<uint8_t> image(64 * 1024, 0x42);
  image[0] = 0xE9;  // a plausible firmware header byte, not that it matters here

  // ---------------------------------------- wrong token ----------------------
  {
    Update.reset();
    const size_t opsBefore = littlefs_stub::opCount();
    AsyncWebServerRequest r = dispatch(*update, "not-the-token", image);
    check(isStatus(r, 401), "a wrong token is refused with 401");
    check(bodyHas(r, "unauthorized"), "the refusal says why");
    check(Update.beginCalls == 0, "Update.begin() is never reached with a wrong token");
    check(Update.writeCalls == 0, "no byte is written with a wrong token");
    check(littlefs_stub::opCount() == opsBefore, "a refused upload touches no file");

    Update.reset();
    AsyncWebServerRequest none = dispatch(*update, nullptr, image);
    check(isStatus(none, 401), "a missing token is refused with 401");
    check(Update.writeCalls == 0, "no byte is written without a token");
  }

  // ---------------------------------------- a run in progress ---------------
  {
    runActive = true;
    Update.reset();
    AsyncWebServerRequest r = dispatch(*update, TEST_TOKEN, image);
    check(isStatus(r, 409), "a transfer during a roast/manual/cool run is refused with 409");
    check(bodyHas(r, "active"), "the refusal names the reason");
    check(Update.beginCalls == 0, "Update.begin() is never reached while a run is active");
    check(Update.bytes.empty(), "nothing was written while a run was active");
    runActive = false;
  }

  // ---------------------------------------- no Content-Length ---------------
  // An empty body never reaches a body handler on the device (the library
  // needs a Content-Length for one), so this case is driven straight at the
  // handler: total == 0 must be refused, not treated as a zero-byte image.
  {
    Update.reset();
    AsyncWebServerRequest request;
    request.addHeader("X-OTA-Token", String(TEST_TOKEN));
    std::vector<uint8_t> one(1, 0);
    update->onBody(&request, one.data(), 1, 0, 0);
    check(isStatus(request, 400), "a body with no Content-Length is refused with 400");
    check(Update.beginCalls == 0, "Update.begin() is never reached without a length");
  }

  // ---------------------------------------- the image is too large ----------
  {
    Update.reset();
    AsyncWebServerRequest request;
    request.addHeader("X-OTA-Token", String(TEST_TOKEN));
    std::vector<uint8_t> one(1, 0);
    // Call the handler directly with a total beyond the slot: the body itself
    // is never sent, which is the point of checking the length up front.
    update->onBody(&request, one.data(), 1, 0, OTA_PUSH_MAX_BYTES + 1);
    check(isStatus(request, 413), "an image larger than the OTA slot is refused with 413");
    check(Update.beginCalls == 0, "Update.begin() is never reached for an oversized image");
  }

  // ---------------------------------------- a complete transfer -------------
  {
    Update.reset();
    const size_t opsBefore = littlefs_stub::opCount();
    AsyncWebServerRequest r = dispatch(*update, TEST_TOKEN, image, 4096);
    check(isStatus(r, 200), "a complete transfer with the right token answers 200");
    check(Update.beginCalls == 1, "Update.begin() was called once");
    check(Update.endCalls == 1 && Update.abortCalls == 0, "the transfer ended, it was not aborted");
    check(Update.bytes.size() == image.size(),
          "exactly Content-Length bytes reached the OTA slot");
    check(Update.bytes == image, "the bytes are the image, in order");
    check(Update.isFinished(), "Update reports the image finished");
    check(littlefs_stub::opCount() == opsBefore,
          "a full transfer touches no file - the profiles are untouched");
    check(littlefs_stub::files()[profilePath] == profileJson,
          "the stored profile is byte-identical after a transfer");
    check(web_ota_reboot_pending(), "a finished transfer asks for a restart");
  }

  // ---------------------------------------- Update refuses / fails ----------
  {
    Update.reset();
    Update.failBegin = true;
    AsyncWebServerRequest r = dispatch(*update, TEST_TOKEN, image);
    check(isStatus(r, 500), "Update.begin() failing answers 500");
  }
  {
    Update.reset();
    Update.failEnd = true;
    AsyncWebServerRequest r = dispatch(*update, TEST_TOKEN, image);
    check(isStatus(r, 500), "Update.end() failing answers 500");
  }

  // ---------------------------------------- the transfer aborts cleanly -----
  {
    Update.reset();
    check(ota_push_begin(TEST_TOKEN, 1000) == OtaPushOk, "a transfer can start");
    check(ota_push_in_progress(), "the state machine reports it in progress");
    ota_push_write(image.data(), 100);
    check(ota_push_bytes_written() == 100, "written bytes are counted");
    check(!ota_push_finished(false), "an incomplete transfer does not report success");
    check(Update.abortCalls == 1, "an incomplete transfer aborts instead of ending");
    check(!ota_push_in_progress(), "the state machine is idle again after an abort");
  }

  // The token comparison is exact - a prefix must not pass.
  {
    std::string almost(TEST_TOKEN);
    almost.pop_back();
    check(!ota_push_authorized(almost.c_str()), "a token that is one character short is refused");
    check(ota_push_authorized(TEST_TOKEN), "the exact token is accepted");
    check(!ota_push_authorized(""), "an empty token is refused");
    check(!ota_push_authorized(nullptr), "a null token is refused");
  }

  return finish();
}
