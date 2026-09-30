// Host-side test of the profile API in src/web_server.cpp: the REAL handlers,
// dispatched through a stubbed ESPAsyncWebServer onto an in-memory LittleFS
// that logs every operation it is asked to perform.
//
// It exists because a profile name went straight into a file path -
// "/profiles/" + name + ".json" - with nothing checking the name. "GET
// /api/profile?name=../config" then reads /profiles/../config.json, which the
// filesystem resolves OUTSIDE /profiles, and DELETE removes exactly that
// file. The honeytokens below are real files in the store: without the name
// check they are reachable, which turns the answer from 400 into 200 and adds
// an operation to the log - so a regression fails on three counts, not only
// on the status code.
//
// Build: the test_web_server line in run.sh. Linked with the real
// src/roast_profile.cpp behind web_server.cpp, so "GET a normal profile" goes
// through the actual file load - and the log proves the ACCEPTED path did
// reach the filesystem, which is what makes "the rejected one did not" a
// meaningful claim.
//
// Covered:
//   * name=../config and name=../../etc/passwd over GET and DELETE: 400,
//     no filesystem operation, honeytoken untouched
//   * the same names over POST /api/profiles (create): 400, nothing written
//   * a normal name: GET returns the profile, POST creates one, the listing
//     shows it, DELETE removes it
//   * the charset boundary: 32 characters accepted, 33 refused, no name at
//     all still answers "missing name"
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include <string>
#include <vector>

#include "config.h"
#include "web_server.h"

// web_server.cpp and roast_profile.cpp both reach the filesystem through this
// one instance (defined here, the way the other host tests define theirs).
LittleFSClass LittleFS;

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

// ---- driving the registered handlers ---------------------------------------

static const AsyncWebServer::Route *findRoute(const char *uri,
                                              WebRequestMethodComposite method) {
  for (AsyncWebServer *server : asyncWebServers())
    for (const AsyncWebServer::Route &r : server->routes())
      if (r.uri == uri && r.method == method) return &r;
  return nullptr;
}

// GET / DELETE: the name arrives as a query parameter, which is what
// ESPAsyncWebServer hands the handler after decoding.
static AsyncWebServerRequest dispatchQuery(const AsyncWebServer::Route &route,
                                           const char *param, const String &value) {
  AsyncWebServerRequest request;
  if (param) request.addParam(param, value);
  route.onRequest(&request);
  return request;
}

// POST: the profile JSON is the request body, in one chunk.
static AsyncWebServerRequest dispatchBody(const AsyncWebServer::Route &route,
                                          const String &json) {
  AsyncWebServerRequest request;
  std::vector<uint8_t> data(json.c_str(), json.c_str() + json.length());
  route.onBody(&request, data.data(), data.size(), 0, data.size());
  return request;
}

// ---- assertions -------------------------------------------------------------

static bool isStatus(const AsyncWebServerRequest &r, int code) {
  return r.sent() && r.responseCode() == code;
}
static bool bodyHas(const AsyncWebServerRequest &r, const char *needle) {
  return std::strstr(r.responseBody().c_str(), needle) != nullptr;
}
static bool fileEquals(const std::string &path, const char *content) {
  auto it = littlefs_stub::files().find(path);
  return it != littlefs_stub::files().end() && it->second == content;
}
static std::string profilePath(const char *name) {
  return std::string(PROFILES_DIR) + "/" + name + ".json";
}
// The path a traversal name really ends up at, once ".." is resolved - the
// address a missing check would read from or delete.
static std::string resolvesTo(const char *name) {
  return littlefs_stub::normalise(profilePath(name));
}

static int finish() {
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

int main() {
  // ---- seed: one profile inside the directory, honeytokens outside it ------
  const std::string configTarget = resolvesTo("../config");
  const std::string passwdTarget = resolvesTo("../../etc/passwd");
  check(configTarget.rfind(PROFILES_DIR, 0) != 0 && passwdTarget.rfind(PROFILES_DIR, 0) != 0,
        "the honeytokens resolve to files outside the profile directory");
  littlefs_stub::files()[configTarget] = "{\"startTemp\":999,\"steps\":[]}";
  littlefs_stub::files()[passwdTarget] = "root:x:0:0";
  littlefs_stub::files()[profilePath("espresso")] =
      "{\"startTemp\":180,\"steps\":[{\"ramp\":60,\"hold\":300,\"temp\":200,\"fan\":80}]}";
  littlefs_stub::dirs().push_back(PROFILES_DIR);
  const char *honeyRootConfig = "{\"startTemp\":999,\"steps\":[]}";
  const char *honeyPasswd = "root:x:0:0";

  // The endpoints are registered by web_server_init(); no callbacks are
  // needed, because nothing under test reads status.
  WebServerCallbacks callbacks = {};
  web_server_init(callbacks);
  check(asyncWebServers().size() == 1 && asyncWebServers()[0]->begun(),
        "web_server_init starts the server");

  const AsyncWebServer::Route *getProfile = findRoute("/api/profile", HTTP_GET);
  const AsyncWebServer::Route *deleteProfile = findRoute("/api/profile", HTTP_DELETE);
  const AsyncWebServer::Route *postProfiles = findRoute("/api/profiles", HTTP_POST);
  const AsyncWebServer::Route *listProfiles = findRoute("/api/profiles", HTTP_GET);
  check(getProfile && deleteProfile && postProfiles && listProfiles,
        "the profile endpoints are registered");
  if (!getProfile || !deleteProfile || !postProfiles || !listProfiles) return finish();

  // ------------------------------------ path traversal must be refused -------
  struct BadName {
    const char *name;      // the name as the browser sends it
    const std::string *target;  // the file it would resolve to
    const char *content;   // what that file contains
  };
  const BadName bad[] = {
      {"../config", &configTarget, honeyRootConfig},
      {"../../etc/passwd", &passwdTarget, honeyPasswd},
  };

  for (const BadName &b : bad) {
    const size_t opsBefore = littlefs_stub::opCount();

    AsyncWebServerRequest get = dispatchQuery(*getProfile, "name", String(b.name));
    check(isStatus(get, 400), "GET name=" + std::string(b.name) + " answers 400");
    check(bodyHas(get, "invalid name"),
          "GET name=" + std::string(b.name) + " says why it was refused");
    check(littlefs_stub::opCount() == opsBefore,
          "GET name=" + std::string(b.name) + " never touches the filesystem");
    check(fileEquals(*b.target, b.content),
          "GET name=" + std::string(b.name) + " leaves the file outside /profiles unmodified");

    AsyncWebServerRequest del = dispatchQuery(*deleteProfile, "name", String(b.name));
    check(isStatus(del, 400), "DELETE name=" + std::string(b.name) + " answers 400");
    check(bodyHas(del, "invalid name"),
          "DELETE name=" + std::string(b.name) + " says why it was refused");
    check(littlefs_stub::opCount() == opsBefore,
          "DELETE name=" + std::string(b.name) + " never touches the filesystem");
    check(fileEquals(*b.target, b.content),
          "DELETE name=" + std::string(b.name) + " leaves the file outside /profiles in place");
  }

  // The same names over create: writing outside the directory is the same bug.
  for (const BadName &b : bad) {
    const size_t opsBefore = littlefs_stub::opCount();
    const std::string json =
        std::string("{\"name\":\"") + b.name + "\",\"startTemp\":100,\"steps\":[]}";
    AsyncWebServerRequest post = dispatchBody(*postProfiles, json);
    check(isStatus(post, 400),
          "POST create name=" + std::string(b.name) + " answers 400");
    check(littlefs_stub::opCount() == opsBefore,
          "POST create name=" + std::string(b.name) + " writes nothing");
    check(fileEquals(*b.target, b.content),
          "POST create name=" + std::string(b.name) + " does not overwrite the file outside /profiles");
  }

  // ---------------------------------------- a normal name has to work --------
  {
    const size_t opsBefore = littlefs_stub::opCount();
    AsyncWebServerRequest get = dispatchQuery(*getProfile, "name", String("espresso"));
    check(isStatus(get, 200), "GET name=espresso answers 200");
    check(bodyHas(get, "\"name\":\"espresso\""), "the profile body carries the requested name");
    check(bodyHas(get, "\"ramp\":60"), "the profile body carries the stored steps");
    check(littlefs_stub::opCount() > opsBefore,
          "the accepted name really did reach the filesystem");
  }

  // Create, list, delete: the other three handlers take the same names.
  {
    AsyncWebServerRequest post = dispatchBody(
        *postProfiles,
        "{\"name\":\"morning\",\"startTemp\":170,\"steps\":[{\"ramp\":30,\"hold\":120,"
        "\"temp\":190,\"fan\":70}]}");
    check(isStatus(post, 200), "POST create name=morning answers 200");
    check(littlefs_stub::files().count(profilePath("morning")) == 1,
          "the new profile landed in the profile directory");

    AsyncWebServerRequest list = dispatchQuery(*listProfiles, nullptr, String());
    check(isStatus(list, 200), "GET /api/profiles answers 200");
    check(bodyHas(list, "morning"), "the listing shows the newly created profile");
    check(bodyHas(list, "espresso"), "the listing shows the seeded profile");

    AsyncWebServerRequest del = dispatchQuery(*deleteProfile, "name", String("espresso"));
    check(isStatus(del, 200), "DELETE name=espresso answers 200");
    check(littlefs_stub::files().count(profilePath("espresso")) == 0,
          "the deleted profile is gone from the profile directory");

    AsyncWebServerRequest missing = dispatchQuery(*deleteProfile, "name", String("no-such"));
    check(isStatus(missing, 404), "DELETE of an unknown profile still answers 404");
  }

  // ------------------------------------------- the charset boundary -----------
  {
    const std::string name32(32, 'a');
    const std::string name33(33, 'a');

    const size_t opsBefore = littlefs_stub::opCount();
    AsyncWebServerRequest at32 = dispatchQuery(*getProfile, "name", String(name32));
    check(isStatus(at32, 404), "a 32-character name passes the check (no such profile -> 404)");
    check(littlefs_stub::opCount() > opsBefore,
          "the 32-character name reached the filesystem");

    AsyncWebServerRequest at33 = dispatchQuery(*getProfile, "name", String(name33));
    check(isStatus(at33, 400), "a 33-character name is refused with 400");

    AsyncWebServerRequest mixed = dispatchQuery(*getProfile, "name", String("abc_DEF-123"));
    check(isStatus(mixed, 404), "letters, digits, dash and underscore are accepted");

    AsyncWebServerRequest dotted = dispatchQuery(*getProfile, "name", String("a.b"));
    check(isStatus(dotted, 400), "a name with a dot is refused with 400");

    AsyncWebServerRequest spaced = dispatchQuery(*getProfile, "name", String("bad name"));
    check(isStatus(spaced, 400), "a name with a space is refused with 400");

    AsyncWebServerRequest empty = dispatchQuery(*getProfile, "name", String(""));
    check(isStatus(empty, 400), "an empty name is refused with 400");
  }

  // The missing-parameter answers predate the name check and must not move.
  AsyncWebServerRequest noNameGet = dispatchQuery(*getProfile, nullptr, String());
  check(isStatus(noNameGet, 400) && bodyHas(noNameGet, "missing name"),
        "GET without a name keeps answering 'missing name'");
  AsyncWebServerRequest noNameDelete = dispatchQuery(*deleteProfile, nullptr, String());
  check(isStatus(noNameDelete, 400) && bodyHas(noNameDelete, "missing name"),
        "DELETE without a name keeps answering 'missing name'");

  return finish();
}
