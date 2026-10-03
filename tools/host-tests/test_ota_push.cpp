// Host-side test of push OTA: the REAL POST /api/update handler in
// src/web_server.cpp, the real state machine in src/ota_push.cpp, dispatched
// through the stubbed ESPAsyncWebServer onto the stubbed Update library - and a
// REAL SHA-256, so the checksum being exercised is one that actually verifies
// images.
//
// Why this exists: with the ArduinoOTA path the DEVICE had to connect back to
// the uploader, which a VLAN-split network blocks, so the upload path moved to
// the direction that works (uploader -> device, see include/ota_push.h). That
// makes a firmware replacement reachable over HTTP on the port that is already
// there, and the endpoint then has to defend itself on its own merits:
//
//   * only the allowed client (OTA_ALLOWED_CLIENT_IP) may replace the
//     firmware. A valid token is not enough - IoT devices reach each other
//     inside the IoT VLAN, so the network does not isolate this route.
//     Everything else gets 404, the same answer an unknown path gives, so a
//     probe cannot even map it.
//   * a bad token or a malformed checksum header also gets 404, and no byte of
//     the image is ever read (the check is in the index == 0 branch, before
//     Update.begin()).
//   * a run active OR the element asking for power gets 409. Not "roast not
//     running": the element is the thing that must never be live during a
//     flash.
//   * the image is hashed ON THE DEVICE and compared with the announced
//     checksum. A mismatch discards the written data before any reboot, so a
//     wrong or half-written image never becomes the boot target - that is the
//     rollback, and this test proves it through Update.abort() rather than
//     Update.end().
//   * a complete, verified transfer writes exactly Content-Length bytes, ends
//     with Update.end(), touches no file (NVS and the profiles survive) and
//     only then asks for the restart.
//
// Build: the test_ota_push line in run.sh. OTA_TOKEN and OTA_ALLOWED_CLIENT_IP
// are throwaway values there, never the real ones.
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <mbedtls/sha256.h>

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

// The values this test device is built with (from -D in run.sh). Never real.
static const char *TEST_TOKEN = OTA_TOKEN;
static const char *ALLOWED_CLIENT = OTA_ALLOWED_CLIENT_IP;
static const char *FOREIGN_CLIENT = "192.168.2.x";

// ---- the checksum, computed with the same SHA-256 the device runs ----------

static std::string sha256hex(const std::vector<uint8_t> &data) {
  mbedtls_sha256_context ctx;
  uint8_t digest[32];
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, data.data(), data.size());
  mbedtls_sha256_finish(&ctx, digest);
  char out[65];
  for (size_t i = 0; i < sizeof(digest); i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
  return std::string(out, 64);
}

// A known vector first, so a broken hasher cannot make this test agree with
// itself and pass.
static void checkHashImplementation() {
  std::vector<uint8_t> abc = {'a', 'b', 'c'};
  check(sha256hex(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "the SHA-256 in this binary matches the known vector for \"abc\"");
  check(sha256hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "and for the empty input");
}

// ---- driving the registered handler ---------------------------------------

static const AsyncWebServer::Route *findRoute(const char *uri,
                                              WebRequestMethodComposite method) {
  for (AsyncWebServer *server : asyncWebServers())
    for (const AsyncWebServer::Route &r : server->routes())
      if (r.uri == uri && r.method == method) return &r;
  return nullptr;
}

struct PushOptions {
  const char *client = ALLOWED_CLIENT;
  const char *token = TEST_TOKEN;
  bool sendSha = true;
  std::string sha;  // empty means "the hash of the image being sent"
};

// Build a request the way ESPAsyncWebServer would: headers on the request, the
// image as the body, index/total per chunk.
static AsyncWebServerRequest makeRequest(const PushOptions &opts,
                                        const std::vector<uint8_t> &image) {
  AsyncWebServerRequest request;
  request.client()->setRemoteIP(String(opts.client));
  if (opts.token) request.addHeader("X-OTA-Token", String(opts.token));
  if (opts.sendSha) {
    const std::string sha = opts.sha.empty() ? sha256hex(image) : opts.sha;
    request.addHeader("X-OTA-SHA256", String(sha.c_str()));
  }
  return request;
}

// A push is one POST body. A refusal answers on the first chunk - but the
// library keeps handing over the rest of the body anyway, so dispatch() sends
// every chunk the way the device does. A test that stops at the first chunk
// would not see what the second one does.
static AsyncWebServerRequest dispatch(const AsyncWebServer::Route &route,
                                      const std::vector<uint8_t> &image,
                                      const PushOptions &opts = PushOptions(),
                                      size_t chunk = 0) {
  AsyncWebServerRequest request = makeRequest(opts, image);
  const size_t size = chunk ? chunk : image.size();
  for (size_t off = 0; off < image.size(); off += size) {
    const size_t len = (off + size > image.size()) ? image.size() - off : size;
    route.onBody(&request, const_cast<uint8_t *>(image.data() + off), len, off, image.size());
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

// ---- the predicates the endpoint asks -------------------------------------

static bool runActive = false;
static bool heaterAsking = false;
static bool isRunActive() { return runActive; }
static bool isHeaterAsking() { return heaterAsking; }

int main() {
  printf("this build: token %d chars, allowed client %s\n",
         (int)strlen(TEST_TOKEN), ALLOWED_CLIENT);
  checkHashImplementation();

  // A profile, so "a transfer does not touch the filesystem" means something.
  const std::string profilePath = std::string(PROFILES_DIR) + "/espresso.json";
  const char *profileJson = "{\"startTemp\":180,\"steps\":[]}";
  littlefs_stub::files()[profilePath] = profileJson;

  ota_push_init(OtaPushCallbacks{isRunActive, isHeaterAsking, nullptr});

  WebServerCallbacks callbacks = {};
  callbacks.isRunActive = isRunActive;
  web_server_init(callbacks);

  const AsyncWebServer::Route *update = findRoute("/api/update", HTTP_POST);
  const AsyncWebServer::Route *probe = findRoute("/api/update", HTTP_GET);
  check(update != nullptr, "POST /api/update is registered");
  check(probe != nullptr, "GET /api/update is registered (probe route)");
  if (!update || !probe) return finish();

  std::vector<uint8_t> image(64 * 1024, 0x42);
  image[0] = 0xE9;  // a plausible firmware header byte

  // ---------------------------------- 1. only the allowed client -----------
  {
    Update.reset();
    PushOptions foreign;
    foreign.client = FOREIGN_CLIENT;
    const size_t opsBefore = littlefs_stub::opCount();
    AsyncWebServerRequest r = dispatch(*update, image, foreign);
    check(isStatus(r, 404), "a request from another IoT address answers 404");
    check(bodyHas(r, "Not Found"), "and looks exactly like an unknown path");
    check(Update.beginCalls == 0, "Update.begin() is never reached for a foreign client");
    check(Update.writeCalls == 0, "no byte is written for a foreign client");
    check(littlefs_stub::opCount() == opsBefore, "a refused upload touches no file");
  }
  {
    AsyncWebServerRequest request;
    request.client()->setRemoteIP(String(FOREIGN_CLIENT));
    probe->onRequest(&request);
    check(isStatus(request, 404), "GET /api/update from a stranger answers 404");
  }
  {
    // The address check is on the endpoint, not on the network: this is the
    // IoT-internal case @network-tech flagged.
    check(!ota_push_probe_trusted(FOREIGN_CLIENT, TEST_TOKEN, sha256hex(image).c_str()),
          "a valid token does not buy access from a non-allowed address");
  }

  // ---------------------------------- 2. bad credentials stay hidden ------
  {
    Update.reset();
    PushOptions bad;
    bad.token = "not-the-token";
    AsyncWebServerRequest r = dispatch(*update, image, bad, 4096);
    check(isStatus(r, 404), "a wrong token answers 404, not 401 - the route stays hidden");
    check(Update.beginCalls == 0, "Update.begin() is never reached with a wrong token");
    check(Update.writeCalls == 0, "no byte is written with a wrong token");
    // The regression that only the real device showed: chunk two arrives after
    // the refusal, and must not try to write into a transfer that never began.
    check(r.responseCode() != 500, "the chunks after a refusal do not turn it into a 500");
    check(!ota_push_in_progress(), "the state machine is idle after a refused body");
  }
  {
    Update.reset();
    PushOptions none;
    none.token = nullptr;
    AsyncWebServerRequest r = dispatch(*update, image, none, 4096);
    check(isStatus(r, 404), "a missing token answers 404");
    check(Update.writeCalls == 0, "no byte is written without a token");
    check(r.responseCode() != 500, "and the rest of the body does not change that");
  }
  {
    Update.reset();
    PushOptions badSha;
    badSha.sha = "not-a-sha256";
    AsyncWebServerRequest r = dispatch(*update, image, badSha, 4096);
    check(isStatus(r, 404), "a malformed checksum header answers 404 before the body is read");
    check(Update.beginCalls == 0, "and never reaches Update.begin()");
    check(r.responseCode() != 500, "and the rest of the body does not change that");
  }
  {
    Update.reset();
    PushOptions foreign;
    foreign.client = FOREIGN_CLIENT;
    AsyncWebServerRequest r = dispatch(*update, image, foreign, 4096);
    check(isStatus(r, 404), "a foreign client sending many chunks still ends at 404");
    check(Update.writeCalls == 0, "and still writes nothing");
  }
  {
    Update.reset();
    PushOptions noSha;
    noSha.sendSha = false;
    AsyncWebServerRequest request = makeRequest(noSha, image);
    update->onBody(&request, image.data(), image.size(), 0, image.size());
    check(isStatus(request, 404), "a missing checksum header answers 404");
    check(Update.beginCalls == 0, "and never reaches Update.begin()");
  }
  {
    // The oversized and no-length cases must also hold up when the rest of the
    // body arrives.
    Update.reset();
    PushOptions opts;
    AsyncWebServerRequest request = makeRequest(opts, image);
    std::vector<uint8_t> chunk(4096, 0);
    for (size_t off = 0; off < image.size(); off += chunk.size()) {
      const size_t len = (off + chunk.size() > image.size()) ? image.size() - off : chunk.size();
      update->onBody(&request, chunk.data(), len, off, OTA_PUSH_MAX_BYTES + 1);
    }
    check(isStatus(request, 413), "an oversized body stays at 413 through all its chunks");
    check(Update.writeCalls == 0, "and never writes a byte");
  }
  {
    // An upper-case hash looks well-formed but is the wrong shape; the format
    // is pinned so the comparison cannot be fooled by case.
    std::string upper = sha256hex(image);
    for (char &c : upper) c = (char)toupper((unsigned char)c);
    check(!ota_push_probe_trusted(ALLOWED_CLIENT, TEST_TOKEN, upper.c_str()),
          "an upper-case checksum header is refused (the format is pinned)");
  }

  // ---------------------------------- 3. the window -----------------------
  {
    runActive = true;
    Update.reset();
    AsyncWebServerRequest r = dispatch(*update, image, PushOptions(), 4096);
    check(isStatus(r, 409), "a transfer during a run is refused with 409");
    check(bodyHas(r, "active"), "the refusal names the reason");
    check(Update.beginCalls == 0, "Update.begin() is never reached while a run is active");
    check(Update.bytes.empty(), "nothing was written while a run was active");
    check(r.responseCode() != 500, "the rest of the body does not turn 409 into a 500");
    runActive = false;
  }
  {
    // The element conducting with no run active - exactly the case a "is a
    // roast running?" check would wave through.
    heaterAsking = true;
    Update.reset();
    AsyncWebServerRequest r = dispatch(*update, image, PushOptions(), 4096);
    check(isStatus(r, 409), "a transfer while the element is asking for power is refused with 409");
    check(Update.beginCalls == 0, "Update.begin() is never reached while the element is live");
    check(Update.bytes.empty(), "nothing was written while the element was live");
    check(r.responseCode() != 500, "and the rest of the body does not turn that into a 500");
    heaterAsking = false;
  }
  {
    AsyncWebServerRequest request = makeRequest(PushOptions(), image);
    probe->onRequest(&request);
    check(isStatus(request, 405), "GET from the trusted uploader is told to POST instead");
  }

  // ---------------------------------- sizes -------------------------------
  {
    Update.reset();
    AsyncWebServerRequest request = makeRequest(PushOptions(), image);
    std::vector<uint8_t> one(1, 0);
    update->onBody(&request, one.data(), 1, 0, 0);
    check(isStatus(request, 400), "a body with no Content-Length is refused with 400");
    check(Update.beginCalls == 0, "Update.begin() is never reached without a length");
  }
  {
    Update.reset();
    AsyncWebServerRequest request = makeRequest(PushOptions(), image);
    std::vector<uint8_t> one(1, 0);
    update->onBody(&request, one.data(), 1, 0, OTA_PUSH_MAX_BYTES + 1);
    check(isStatus(request, 413), "an image larger than the OTA slot is refused with 413");
    check(Update.beginCalls == 0, "Update.begin() is never reached for an oversized image");
  }

  // ---------------------------------- 4. checksum + rollback -------------
  {
    // The announced checksum is a valid SHA-256 that simply is not this
    // image's: the transfer is fully written and then thrown away.
    Update.reset();
    PushOptions wrongSha;
    wrongSha.sha = "0000000000000000000000000000000000000000000000000000000000000000";
    AsyncWebServerRequest r = dispatch(*update, image, wrongSha, 4096);
    check(isStatus(r, 403),
          "an image that does not match the announced checksum is refused with 403");
    check(Update.endCalls == 0, "the image is never committed (Update.end() not called)");
    check(Update.abortCalls == 1, "the written data is discarded with Update.abort()");
    check(!web_ota_reboot_pending(), "no restart is asked for, so the running image stays");
    check(ota_push_last_transfer_rejected(), "the state machine records the rejected transfer");
    check(littlefs_stub::files()[profilePath] == profileJson,
          "the stored profile is byte-identical after a rejected transfer");
  }
  {
    // A transfer that dies half way: the full image's hash is announced, but
    // only half the bytes arrive. Nothing is committed and the written data is
    // thrown away - the rollback path for a dropped connection.
    Update.reset();
    PushOptions half;
    half.sha = sha256hex(image);
    AsyncWebServerRequest request = makeRequest(half, image);
    const size_t halfLen = image.size() / 2;
    update->onBody(&request, image.data(), halfLen, 0, image.size());
    check(!request.sent(), "a half-sent image produces no answer of its own yet");
    check(Update.endCalls == 0, "and nothing is committed");
    check(!ota_push_finished(false), "a dropped connection cannot complete the transfer");
    check(Update.abortCalls == 1, "the half-written image is discarded");
    check(!web_ota_reboot_pending(), "and the device keeps booting the current image");
  }

  // ---------------------------------- a complete transfer -----------------
  {
    Update.reset();
    const size_t opsBefore = littlefs_stub::opCount();
    AsyncWebServerRequest r = dispatch(*update, image, PushOptions(), 4096);
    check(isStatus(r, 200),
          "a complete transfer with the right token, address and checksum answers 200");
    check(Update.beginCalls == 1, "Update.begin() was called once");
    check(Update.endCalls == 1 && Update.abortCalls == 0, "the transfer ended, it was not aborted");
    check(Update.bytes.size() == image.size(), "exactly Content-Length bytes reached the OTA slot");
    check(Update.bytes == image, "the bytes are the image, in order");
    check(Update.isFinished(), "Update reports the image finished");
    check(std::string(ota_push_sha256_hex()) == sha256hex(image),
          "the device's own hash of what it received matches the announced one");
    check(littlefs_stub::opCount() == opsBefore,
          "a full transfer touches no file - NVS and the profiles are untouched");
    check(littlefs_stub::files()[profilePath] == profileJson,
          "the stored profile is byte-identical after a transfer");
    check(web_ota_reboot_pending(), "a finished transfer asks for a restart");
  }

  // ---------------------------------- Update refuses ----------------------
  {
    Update.reset();
    Update.failBegin = true;
    AsyncWebServerRequest r = dispatch(*update, image);
    check(isStatus(r, 500), "Update.begin() failing answers 500");
  }
  {
    Update.reset();
    Update.failEnd = true;
    AsyncWebServerRequest r = dispatch(*update, image);
    check(isStatus(r, 500), "Update.end() failing answers 500");
  }

  // The identity check is exact - a near-miss must not pass.
  {
    std::string almost(TEST_TOKEN);
    almost.pop_back();
    check(!ota_push_probe_trusted(ALLOWED_CLIENT, almost.c_str(), sha256hex(image).c_str()),
          "a token one character short is refused");
    check(!ota_push_probe_trusted(ALLOWED_CLIENT, "", sha256hex(image).c_str()),
          "an empty token is refused");
    check(!ota_push_probe_trusted(nullptr, TEST_TOKEN, sha256hex(image).c_str()),
          "a missing client address is refused");
    check(ota_push_probe_trusted(ALLOWED_CLIENT, TEST_TOKEN, sha256hex(image).c_str()),
          "the exact client, token and checksum are accepted");
  }

  return finish();
}
