// Host-side test of the DISABLED push-OTA endpoint: the same sources, built
// with an empty OTA_TOKEN (see the -DOTA_TOKEN=\"\" line in run.sh).
//
// An endpoint that accepts an unauthenticated firmware upload is worse than no
// endpoint, so an empty token in include/secrets.h switches the whole thing
// off: neither /api/update nor /api/update/fs takes a byte from anyone, and the
// answer is 503 "switched off" rather than a decoy 404. That is the one place
// where the route is allowed to admit it exists - it cannot be made to do
// anything, so there is nothing to hide behind a 404.
//
// This is its own binary because the token is a compile-time constant: the main
// push test is built with a token precisely so it can exercise the accepted
// path.
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <vector>

#include "config.h"
#include "ota_push.h"
#include "web_server.h"

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

static const AsyncWebServer::Route *findRoute(const char *uri,
                                              WebRequestMethodComposite method) {
  for (AsyncWebServer *server : asyncWebServers())
    for (const AsyncWebServer::Route &r : server->routes())
      if (r.uri == uri && r.method == method) return &r;
  return nullptr;
}

static AsyncWebServerRequest makeRequest() {
  AsyncWebServerRequest request;
  request.client()->setRemoteIP(String("192.0.2.10"));
  request.addHeader("X-OTA-Token", String(""));
  // Well-formed as far as the header goes: the endpoint must refuse on the
  // missing configuration, not on the checksum.
  request.addHeader("X-OTA-SHA256",
                    String("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  return request;
}

static void dispatch(const AsyncWebServer::Route &route,
                     const std::vector<uint8_t> &body, size_t chunk) {
  AsyncWebServerRequest request = makeRequest();
  for (size_t off = 0; off < body.size(); off += chunk) {
    const size_t len = (off + chunk > body.size()) ? body.size() - off : chunk;
    route.onBody(&request, const_cast<uint8_t *>(body.data() + off), len, off, body.size());
  }
  check(request.sent() && request.responseCode() == 503,
        "a POST to a disabled endpoint answers 503 (switched off, not hidden)");
  check(std::strstr(request.responseBody().c_str(), "disabled") != nullptr,
        "and the answer says the endpoint is disabled");
}

int main() {
  check(OTA_PUSH_ENABLED == 0, "this build has no OTA_TOKEN, so the endpoint is off");
  ota_push_init(OtaPushCallbacks{});
  check(!ota_push_enabled(), "ota_push_enabled() reports the endpoint as off");

  WebServerCallbacks callbacks = {};
  web_server_init(callbacks);

  const AsyncWebServer::Route *update = findRoute("/api/update", HTTP_POST);
  const AsyncWebServer::Route *fsUpdate = findRoute("/api/update/fs", HTTP_POST);
  const AsyncWebServer::Route *probe = findRoute("/api/update", HTTP_GET);
  const AsyncWebServer::Route *fsProbe = findRoute("/api/update/fs", HTTP_GET);
  check(update != nullptr, "POST /api/update is registered even when disabled");
  check(fsUpdate != nullptr, "POST /api/update/fs is registered even when disabled");
  check(probe != nullptr, "GET /api/update is registered (probe route)");
  check(fsProbe != nullptr, "GET /api/update/fs is registered (probe route)");
  if (!update || !fsUpdate || !probe || !fsProbe) {
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
  }

  std::vector<uint8_t> image(64 * 1024, 0x42);
  std::vector<uint8_t> fsImage(200 * 1024, 0x11);

  {
    // The chunks after the refusal arrive too, exactly as on the device: the
    // answer must stay 503 and nothing may be opened or written.
    Update.reset();
    const size_t opsBefore = littlefs_stub::opCount();
    dispatch(*update, image, 4096);
    check(Update.beginCalls == 0, "the app route opens no partition");
    check(Update.writeCalls == 0, "and writes no byte");
    check(littlefs_stub::opCount() == opsBefore, "and touches no file");
    check(LittleFS.mounted(), "and does not unmount the filesystem");
  }
  {
    Update.reset();
    const size_t opsBefore = littlefs_stub::opCount();
    dispatch(*fsUpdate, fsImage, 4096);
    check(Update.beginCalls == 0, "the filesystem route opens no partition either");
    check(Update.writeCalls == 0, "and writes no byte");
    check(littlefs_stub::opCount() == opsBefore, "and touches no file");
    check(LittleFS.mounted(), "and leaves the filesystem mounted");
    check(!ota_push_fs_unmounted(), "and never reports it as unmounted");
  }
  {
    // A GET probe from anyone, even the configured client, learns nothing.
    AsyncWebServerRequest request;
    request.client()->setRemoteIP(String("192.0.2.10"));
    probe->onRequest(&request);
    check(request.sent() && request.responseCode() == 404,
          "GET /api/update answers 404 while the endpoint is off");
  }
  {
    AsyncWebServerRequest request;
    request.client()->setRemoteIP(String("192.0.2.10"));
    fsProbe->onRequest(&request);
    check(request.sent() && request.responseCode() == 404,
          "GET /api/update/fs answers 404 while the endpoint is off");
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
