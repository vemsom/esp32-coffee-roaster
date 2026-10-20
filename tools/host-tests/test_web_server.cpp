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
#include <Update.h>

#include <string>
#include <vector>

#include "config.h"
#include "web_server.h"

// web_server.cpp and roast_profile.cpp both reach the filesystem through this
// one instance (defined here, the way the other host tests define theirs).
LittleFSClass LittleFS;

// ota_push.cpp (linked into this binary, because the real handler behind
// /api/update calls it) reaches Update through the stub. Declaring the
// instance here keeps the stub's own storage out of the header.
UpdateClass Update;
SerialStub Serial;

static int failures = 0;
static int checks = 0;

// What the stop-and-cool endpoint handed the control layer. The endpoint's job
// is the contract: validate, then call once with seconds. What the call DOES is
// tested in test_control against the real main.cpp, so here the callback only
// records and answers.
static int g_stopCoolCalls = 0;
static int g_stopCoolSpeed = -1;
static unsigned long g_stopCoolSeconds = 0;
static bool g_stopCoolAccept = true;

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

  // The endpoints are registered by web_server_init(). The callbacks are not
  // optional: an empty function pointer is a null call, not a default value,
  // and the status handler reads twenty-four of them. Every getter it touches
  // therefore needs a real function or the /api/status dispatch below jumps
  // through null. The values are deliberately dull - this test is about
  // whether the fields are present and well-formed, not about what they say
  // (test_control drives the real values end to end).
  WebServerCallbacks callbacks = {};
  callbacks.getBT = []() { return 23.0f; };
  callbacks.getET = []() { return 21.0f; };
  callbacks.getRorBt = []() { return 0.0f; };
  callbacks.getRorEt = []() { return 0.0f; };
  callbacks.getHeaterDuty = []() { return 0.0f; };
  callbacks.getFanSpeed = []() { return 0; };
  callbacks.getFanActual = []() { return 0.0f; };
  callbacks.getRoastActive = []() { return false; };
  callbacks.getDutyCeiling = []() { return 0.0f; };
  callbacks.getElapsedSeconds = []() { return 0UL; };
  callbacks.getRoastPaused = []() { return false; };
  callbacks.getManualActive = []() { return false; };
  callbacks.getManualTargetTemp = []() { return 0.0f; };
  callbacks.getManualRemainingSeconds = []() { return 0UL; };
  callbacks.getManualAutoCool = []() { return false; };
  callbacks.getCoolActive = []() { return false; };
  callbacks.getCoolSpeed = []() { return 0; };
  callbacks.getCoolRemainingSeconds = []() { return 0UL; };
  callbacks.getSafetyFault = []() { return false; };
  callbacks.getSafetyReason = []() { return "none"; };
  callbacks.getFanFault = []() { return false; };
  callbacks.getWifiConnected = []() { return true; };
  callbacks.getRorGuidance = []() { return -1; };
  callbacks.getRorTarget = []() { return 0.0f; };
  callbacks.getRorError = []() { return 0.0f; };
  callbacks.getRorActive = []() { return false; };
  callbacks.getPidP = []() { return 1.5f; };
  callbacks.getPidI = []() { return 2.5f; };
  callbacks.getPidD = []() { return 0.0f; };
  callbacks.getPidIntegral = []() { return 16.7f; };
  callbacks.getFilteredBT = []() { return 23.5f; };
  callbacks.stopRoastAndCool = [](int speed, unsigned long seconds) {
    g_stopCoolCalls++;
    g_stopCoolSpeed = speed;
    g_stopCoolSeconds = seconds;
    return g_stopCoolAccept;
  };
  web_server_init(callbacks);
  check(asyncWebServers().size() == 1 && asyncWebServers()[0]->begun(),
        "web_server_init starts the server");
  check(asyncWebServers().size() == 1,
        "the file-static server is registered once, so every route is unique");

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

  // ---------------------------------------- stop and cool (one endpoint) ------
  // POST /api/roast/stop-and-cool is the button, and it must not be reachable
  // through, or collide with, the plain Stop route. The device-side effect (the
  // fan never passing through 0 %) is main.cpp's job and is asserted in
  // test_control; here it is the contract: the values, their limits, one call.
  {
    const AsyncWebServer::Route *stopCool = findRoute("/api/roast/stop-and-cool", HTTP_POST);
    check(stopCool != nullptr, "POST /api/roast/stop-and-cool is registered");
    check(asyncWebServers()[0]->routeFor("/api/roast/stop-and-cool", HTTP_POST) == stopCool,
          "the new route is reached, not the plain Stop route beside it");
    check(findRoute("/api/roast/stop", HTTP_POST) != nullptr,
          "and the plain Stop route is still registered");

    if (stopCool) {
      g_stopCoolCalls = 0;
      AsyncWebServerRequest ok = dispatchBody(*stopCool, "{\"coolSpeed\":60,\"coolMinutes\":2}");
      check(isStatus(ok, 200), "stop-and-cool with valid values answers 200");
      check(g_stopCoolCalls == 1 && g_stopCoolSpeed == 60 && g_stopCoolSeconds == 120,
            "it calls the control layer once, with the speed and the time in seconds");

      const int callsBefore = g_stopCoolCalls;
      struct Bad { const char *json; const char *what; };
      const Bad bad[] = {
          {"{\"coolSpeed\":0,\"coolMinutes\":2}", "0 % is not a fan speed"},
          {"{\"coolSpeed\":101,\"coolMinutes\":2}", "101 % is over the top"},
          {"{\"coolSpeed\":60,\"coolMinutes\":0.2}", "below the shortest cool-down"},
          {"{\"coolSpeed\":60,\"coolMinutes\":121}", "past the longest cool-down"},
          {"{\"coolSpeed\":60}", "minutes missing entirely"},
          {"not json", "a body that is not JSON"},
      };
      for (const Bad &b : bad) {
        AsyncWebServerRequest refused = dispatchBody(*stopCool, b.json);
        check(isStatus(refused, 400), std::string("stop-and-cool refuses ") + b.what);
      }
      check(g_stopCoolCalls == callsBefore,
            "a refused request never reaches the control layer (nothing is stopped)");

      // The control layer can still refuse (an out-of-range value that got
      // past the browser, or a state it will not stop in): that is a 409, and
      // it has to be visible rather than reported as a success.
      g_stopCoolAccept = false;
      AsyncWebServerRequest denied = dispatchBody(*stopCool, "{\"coolSpeed\":60,\"coolMinutes\":2}");
      check(isStatus(denied, 409), "a refusal from the control layer answers 409");
      g_stopCoolAccept = true;
    }
  }

  // ------------------------------- profile cool-down fields (per profile) -----
  // The profile's own cool-down: written by POST /api/profiles, read back by
  // GET /api/profile, and only in the file when it is actually set - a profile
  // that never had these fields must stay byte-for-byte what it was.
  {
    AsyncWebServerRequest post = dispatchBody(
        *postProfiles,
        "{\"name\":\"cooled\",\"startTemp\":170,\"coolFan\":80,\"coolMinutes\":4.5,"
        "\"steps\":[{\"ramp\":30,\"hold\":120,\"temp\":190,\"fan\":70}]}");
    check(isStatus(post, 200), "POST create with a cool-down answers 200");

    AsyncWebServerRequest get = dispatchQuery(*getProfile, "name", String("cooled"));
    check(isStatus(get, 200), "GET of the cooled profile answers 200");
    check(bodyHas(get, "\"coolFan\":80"), "the cool-down fan speed comes back");
    check(bodyHas(get, "\"coolMinutes\":4.5"), "the cool-down minutes come back");

    // The file itself carries them, at profile level - that is the format the
    // docs and a hand-edited profile follow.
    auto cooledFile = littlefs_stub::files().find(profilePath("cooled"));
    check(cooledFile != littlefs_stub::files().end() &&
              cooledFile->second.find("\"coolFan\":80") != std::string::npos &&
              cooledFile->second.find("\"coolMinutes\":4.5") != std::string::npos,
          "the stored profile file carries coolFan and coolMinutes");

    // Out of range: clamped to the limits the cooling panel uses, not stored as
    // written, so a hand-edited number cannot mean something the UI could not.
    AsyncWebServerRequest clamped = dispatchBody(
        *postProfiles,
        "{\"name\":\"clamped\",\"startTemp\":170,\"coolFan\":999,\"coolMinutes\":999,"
        "\"steps\":[{\"ramp\":30,\"hold\":120,\"temp\":190,\"fan\":70}]}");
    check(isStatus(clamped, 200), "POST create with out-of-range cool values answers 200");
    AsyncWebServerRequest clampedGet = dispatchQuery(*getProfile, "name", String("clamped"));
    check(bodyHas(clampedGet, "\"coolFan\":100"),
          "an over-large fan speed is clamped to the maximum");
    check(bodyHas(clampedGet, "\"coolMinutes\":120"),
          "an over-long cool-down is clamped to the maximum");

    // A profile that asks for no cool-down: reported as 0/0 over the API, and
    // absent from the file.
    AsyncWebServerRequest plain = dispatchQuery(*getProfile, "name", String("morning"));
    check(isStatus(plain, 200), "GET of the profile without a cool-down answers 200");
    check(bodyHas(plain, "\"coolFan\":0") && bodyHas(plain, "\"coolMinutes\":0"),
          "a profile without a cool-down reports 0/0");
    check(fileEquals(profilePath("morning"),
                     "{\"startTemp\":170,\"steps\":[{\"ramp\":30,\"hold\":120,"
                     "\"temp\":190,\"fan\":70}]}"),
          "and its file stays byte-for-byte what it was before the fields existed");
  }

  // ------------------------------------- the bean weight (duty ceiling) ------
  // The charge is a profile field like the cool-down pair: written by
  // POST /api/profiles, read back by GET /api/profile, clamped to
  // BEAN_WEIGHT_MIN_G..BEAN_WEIGHT_MAX_G, and absent from the file when the
  // profile does not state one - a profile that never had it must not change.
  {
    AsyncWebServerRequest post = dispatchBody(
        *postProfiles,
        "{\"name\":\"weighed\",\"startTemp\":170,\"beanWeight\":20,"
        "\"steps\":[{\"ramp\":30,\"hold\":120,\"temp\":190,\"fan\":70}]}");
    check(isStatus(post, 200), "POST create with a bean weight answers 200");

    AsyncWebServerRequest get = dispatchQuery(*getProfile, "name", String("weighed"));
    check(isStatus(get, 200), "GET of the weighed profile answers 200");
    check(bodyHas(get, "\"beanWeight\":20"), "the bean weight comes back in grams");

    auto weighedFile = littlefs_stub::files().find(profilePath("weighed"));
    check(weighedFile != littlefs_stub::files().end() &&
              weighedFile->second.find("\"beanWeight\":20") != std::string::npos,
          "the stored profile file carries beanWeight");

    // Out of range: clamped to what the editor could produce, so a hand-edited
    // profile cannot carry a value the rest of the firmware would refuse.
    struct Weight { int given; int stored; const char *what; };
    const Weight weights[] = {
        {1, BEAN_WEIGHT_MIN_G, "a weight below the minimum is clamped up"},
        {9999, BEAN_WEIGHT_MAX_G, "a weight above the maximum is clamped down"},
    };
    for (const Weight &w : weights) {
      const std::string json =
          std::string("{\"name\":\"clamped-weight\",\"startTemp\":170,\"beanWeight\":") +
          std::to_string(w.given) + ",\"steps\":[]}";
      AsyncWebServerRequest r = dispatchBody(*postProfiles, json);
      check(isStatus(r, 200), std::string("POST create with ") + w.what + " answers 200");
      AsyncWebServerRequest back =
          dispatchQuery(*getProfile, "name", String("clamped-weight"));
      const std::string want = "\"beanWeight\":" + std::to_string(w.stored);
      check(bodyHas(back, want.c_str()), w.what);
    }

    // No stated weight: 0 over the API (= no ceiling), and no field in the
    // file - the same rule the cool-down pair follows.
    AsyncWebServerRequest plain = dispatchQuery(*getProfile, "name", String("morning"));
    check(bodyHas(plain, "\"beanWeight\":0"),
          "a profile without a bean weight reports 0 (no ceiling)");
    check(littlefs_stub::files()[profilePath("morning")].find("beanWeight") ==
              std::string::npos,
          "and no beanWeight is written into its file");
  }

  // ------------------------------------------- build identity in /api/status --
  // The reason this exists: after an OTA push the question is "did the image I
  // sent actually land?", and a version number that did not change cannot
  // answer it. The status handler must therefore carry the release, the git sha
  // of the build and the build time - with values a push can be checked
  // against.
  //
  // Note: web_server_init() must NOT be called a second time here. The server
  // in web_server.cpp is file-static, so a second init registers the same
  // object in the stub registry again and every route appears twice - and the
  // callbacks would be replaced with the empty set below. The handler is
  // reached through the route this test already has.
  {
    const AsyncWebServer::Route *status = findRoute("/api/status", HTTP_GET);
    check(status != nullptr, "GET /api/status is registered");
    if (status) {
      check(status->onRequest != nullptr,
            "the /api/status route has a handler (a route without one is a null call)");
      AsyncWebServerRequest r;
      status->onRequest(&r);
      check(isStatus(r, 200), "GET /api/status answers 200 with the callbacks this test set");
      const std::string body = r.responseBody().c_str();

      // FW_VERSION first: it is what Home Assistant shows as sw_version, so the
      // two must never drift apart.
      check(body.find(std::string("\"fw\":\"") + FW_VERSION + "\"") != std::string::npos,
            "status reports the release as fw, and it is FW_VERSION");
      check(body.find("\"build\":\"") != std::string::npos,
            "status reports the build id");
      check(body.find("\"built\":\"") != std::string::npos,
            "status reports when the build was made");

      // The build id has to be usable as a comparison, so it may not be empty
      // and may not contain a quote or a backslash (it goes into JSON and into
      // a shell comparison by hand).
      const size_t key = body.find("\"build\":\"");
      const size_t start = key + strlen("\"build\":\"");
      const size_t end = body.find('"', start);
      const std::string buildId = body.substr(start, end - start);
      check(!buildId.empty(), "the build id is not empty");
      check(buildId.find('\\') == std::string::npos && buildId.find('"') == std::string::npos,
            "the build id has no character that breaks JSON or a comparison");
      check(body.find(std::string("\"lang\":\"") + FW_LANG_CODE + "\"") != std::string::npos,
            "status still reports the build language (unchanged by this)");

      // Which channels the safety latch watches. A reduced-protection image
      // (SAFETY_ET_ENABLED 0) has to say so: `et` is null in both cases - a
      // dead module and a switched-off channel look the same over the API - so
      // null alone can never mean "not watched". This test runs in both build
      // modes and the expectation follows the flag it was compiled with.
      const bool wantEtDisabled = (SAFETY_ET_ENABLED == 0);
      check(body.find(std::string("\"etDisabled\":") + (wantEtDisabled ? "true" : "false")) !=
                std::string::npos,
            "status reports etDisabled matching the build's SAFETY_ET_ENABLED");

      // Which channel the RoR guidance steers on, reported for the same reason:
      // rorBt and rorEt are both published whatever the source is, so the
      // numbers alone cannot say which one a rorTarget step is compared
      // against. Follows the build mode too (ROR_GUIDANCE_ON_BT), so this test
      // runs in both passes and expects the source its own flag implies.
      check(body.find(std::string("\"rorSource\":\"") +
                      (ROR_GUIDANCE_ON_BT ? "bt" : "et") + "\"") != std::string::npos,
            "status reports which channel the RoR guidance steers on");

      // The PID diagnostics and the fan's reached speed. These exist so the
      // loop can be trimmed against a machine that is RUNNING - the earlier
      // regime was to flash a change and roast again to find out what the
      // gains did - so they have to be in the payload, with the values the
      // callbacks report. The literal values here are deliberately distinct
      // from each other and from the other fields, so a Field that reports the
      // wrong getter cannot pass by accident.
      check(body.find("\"pidP\":1.5") != std::string::npos,
            "status reports the PID's P contribution");
      check(body.find("\"pidI\":2.5") != std::string::npos,
            "status reports the PID's I contribution");
      check(body.find("\"pidD\":0") != std::string::npos,
            "status reports the PID's D contribution (0 in the shipped tuning)");
      check(body.find("\"pidIntegral\":16.7") != std::string::npos,
            "status reports the raw integral state in C*s");
      check(body.find("\"btFiltered\":23.5") != std::string::npos,
            "status reports the filtered bean temperature the PID read");
      check(body.find("\"fanActual\":0") != std::string::npos,
            "status reports the fan speed the fan has actually reached");
    }
  }

  return finish();
}
