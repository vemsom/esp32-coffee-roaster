#include "web_server.h"
#include "config.h"
#include "strings.h"
#include "ota_push.h"
#include "state_lock.h"
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "roast_profile.h"

static AsyncWebServer server(80);
static WebServerCallbacks cb;

static void sendOk(AsyncWebServerRequest *request) {
  request->send(200, "application/json", "{\"ok\":true}");
}

static void sendError(AsyncWebServerRequest *request, int code, const char *msg) {
  JsonDocument doc;
  doc["error"] = msg;
  String out;
  serializeJson(doc, out);
  request->send(code, "application/json", out);
}

// ---------------------------------------------------------------- status ---

static void handleStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  // Assembled under the state lock for the same reason as the MQTT payload:
  // the getters lock individually, but only the block keeps one snapshot from
  // straddling a change (mode from before a stop, elapsed time from after
  // it). Held for microseconds, never across the send.
  {
    StateLockGuard guard;
    doc["bt"] = cb.getBT();
    doc["et"] = cb.getET();
    // Is the ET channel in use at all? false is the normal build. true means
    // this image was compiled BT-only (SAFETY_ET_ENABLED 0 in include/config.h):
    // the ET converter is not read, `et` stays null, and its trips and the
    // BT/ET cross-check are switched off. Reported so a reduced-protection
    // image can never be taken for a full one from the outside - null on `et`
    // alone says "no reading", not "not watched".
    doc["etDisabled"] = (SAFETY_ET_ENABLED == 0);
    // Rate of rise, C/min, one decimal. Negative is a real value (cooling or
    // past the turning point), so it is passed straight through.
    doc["rorBt"] = cb.getRorBt();
    doc["rorEt"] = cb.getRorEt();
    doc["heaterDuty"] = cb.getHeaterDuty();
    doc["fanSpeed"] = cb.getFanSpeed();
    // The speed the fan has actually reached, next to the requested one. They
    // differ exactly while the ramp (FAN_SLEW_PCT_PER_SEC) is running - and that
    // is also when the interlock is holding the element off, so this is the
    // field that explains a fanFault next to a fanSpeed of 100.
    doc["fanActual"] = cb.getFanActual();
    doc["roastActive"] = cb.getRoastActive();
    doc["elapsedSeconds"] = cb.getElapsedSeconds();
    doc["roastPaused"] = cb.getRoastPaused();
    doc["manualActive"] = cb.getManualActive();
    doc["manualTargetTemp"] = cb.getManualTargetTemp();
    doc["manualRemainingSeconds"] = cb.getManualRemainingSeconds();
    doc["manualAutoCool"] = cb.getManualAutoCool();
    doc["coolActive"] = cb.getCoolActive();
    doc["coolSpeed"] = cb.getCoolSpeed();
    doc["coolRemainingSeconds"] = cb.getCoolRemainingSeconds();
    doc["safetyFault"] = cb.getSafetyFault();
    doc["safetyReason"] = cb.getSafetyReason();
    doc["fanFault"] = cb.getFanFault();
    // Device-side WiFi state. The browser normally cannot see this (if the
    // roaster's WiFi is down the page cannot be fetched at all - that case is
    // the fetch failing, which the UI shows separately), so it is a secondary
    // signal, useful for cached pages and for anything else reading the API.
    doc["wifiConnected"] = cb.getWifiConnected();
    // Rate-of-rise guidance diagnostics. roRorGuidance is the active step
    // index, -1 when no step is RoR-driven. These are always present so the
    // web UI can show "off" as the default state.
    doc["roRorGuidance"] = cb.getRorGuidance();
    doc["roRorTarget"] = cb.getRorTarget();
    doc["roRorError"] = cb.getRorError();
    doc["roRorActive"] = cb.getRorActive();
    // PID diagnostics, so the loop can be trimmed while the machine is running
    // instead of between roasts (see include/pid.h and the PID section in
    // docs/firmware-notes.md). pidP/pidI/pidD are the three contributions as
    // they were last applied, in percent; the sum of the three (before the
    // output clamp) is what the element was asked for in profile mode. These
    // are always present and read 0 while no run is active, so a client can
    // tell "no run" from "a run with a silent term".
    doc["pidP"] = cb.getPidP();
    doc["pidI"] = cb.getPidI();
    doc["pidD"] = cb.getPidD();
    // The integral state itself, in C*s. This is the number that used to be
    // clamped against the 0-100 output range (which capped the I term at
    // KI * 100 = 5 % and left the loop ~20 C under its setpoint). Watch it while
    // trimming: if it sits pinned at outMax / KI the loop is asking for more
    // than the element can give, and the offset is physical, not tuning.
    doc["pidIntegral"] = cb.getPidIntegral();
    // The control input: the bean temperature low-passed at
    // PID_INPUT_FILTER_TAU_S, which is what the PID above read. The raw value
    // stays in `bt` - the safety layer, the RoR fit and HA all keep seeing that
    // one, and only the controller is behind the filter.
    doc["btFiltered"] = cb.getFilteredBT();
    // Which channel that guidance steers on, "bt" or "et". Compile-time
    // (ROR_GUIDANCE_ON_BT in include/config.h, which follows SAFETY_ET_ENABLED)
    // and reported because neither rorBt nor rorEt says which of the two the
    // error above was built from - and a rorTarget step that never moves is
    // exactly the silent failure this field makes visible.
    doc["rorSource"] = ROR_GUIDANCE_SOURCE_NAME;
    // Build language (FW_LANG_EN in include/config.h), reported so the page
    // can follow the firmware. The browser's own language must not decide:
    // the UI and the Home Assistant names have to agree.
    doc["lang"] = FW_LANG_CODE;
    // Build identity. fw is the release (same as HA's sw_version), build is
    // the git sha of the build and built is when it was compiled - the three
    // together are what makes a push provable: compare "build" with
    //   git rev-parse --short HEAD
    // on the machine that sent it. Without them an OTA push that landed and
    // one that silently failed look identical from the outside.
    doc["fw"] = FW_VERSION;
    doc["build"] = FW_BUILD_SHA;
    doc["built"] = FW_BUILD_TIME;
  }

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

// ------------------------------------------------------------------- fan ---

static void handleFanSet(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                         size_t index, size_t total) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    sendError(request, 400, "invalid json");
    return;
  }
  cb.setFanSpeed(doc["speed"] | 0);
  sendOk(request);
}

// ---------------------------------------------------------------- manual ---

static void handleManualStart(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                              size_t index, size_t total) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    sendError(request, 400, "invalid json");
    return;
  }
  float temp = doc["temp"] | 0.0f;
  float minutes = doc["minutes"] | 0.0f;
  unsigned long seconds = (unsigned long)(minutes * 60.0f + 0.5f);
  if (temp <= 0 || seconds == 0) {
    sendError(request, 400, "temp and minutes must be greater than zero");
    return;
  }
  bool autoCool = doc["autoCool"] | false;
  int coolSpeed = doc["coolSpeed"] | 0;
  float coolMinutes = doc["coolMinutes"] | 0.0f;
  unsigned long coolSeconds = (unsigned long)(coolMinutes * 60.0f + 0.5f);

  // Fan interlock, refused up front with its own message: starting a run that
  // is not allowed to heat would only look like a broken UI. The same rule is
  // enforced again in main.cpp, where it cuts the element if the fan drops
  // below the threshold mid-run.
  if (cb.getFanSpeed() < FAN_MIN_FOR_HEATER_PCT) {
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot start: fan must run at least %d %% first",
             FAN_MIN_FOR_HEATER_PCT);
    sendError(request, 409, msg);
    return;
  }
  if (!cb.startManual(temp, seconds, autoCool, coolSpeed, coolSeconds)) {
    sendError(request, 409, "cannot start: safety alarm active");
    return;
  }
  sendOk(request);
}

static void handleManualStop(AsyncWebServerRequest *request) {
  cb.stopManual();
  sendOk(request);
}

// ------------------------------------------------------------------ cool ---

static void handleCoolStart(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                            size_t index, size_t total) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    sendError(request, 400, "invalid json");
    return;
  }
  int speed = doc["speed"] | 0;
  float minutes = doc["minutes"] | 0.0f;
  unsigned long seconds = (unsigned long)(minutes * 60.0f + 0.5f);
  if (speed <= 0 || seconds == 0) {
    sendError(request, 400, "speed and minutes must be greater than zero");
    return;
  }
  cb.startCool(speed, seconds);
  sendOk(request);
}

static void handleCoolStop(AsyncWebServerRequest *request) {
  cb.stopCool();
  sendOk(request);
}

// -------------------------------------------------------------- profiles ---

// A profile name doubles as its file name under PROFILES_DIR, so an
// unchecked name is a path: "name=../config" would be appended as
// /profiles/../config.json, which the filesystem resolves OUTSIDE the
// directory - readable over GET, deletable over DELETE, writable over POST.
// Accepted is only ^[A-Za-z0-9_-]{1,32}$ - letters, digits, underscore and
// hyphen, 1 to 32 characters - checked before the path is built, in every
// handler that takes a name, never after.
static bool validProfileName(const String &name) {
  size_t len = name.length();
  if (len < 1 || len > 32) return false;
  const char *s = name.c_str();
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

// True while the profile partition is readable. A filesystem OTA transfer
// rewrites that partition, so during one the profile endpoints must refuse
// rather than read a half-written image as if it were a filesystem.
//
// Weak on purpose: web_server.cpp owns the check, but the transfer that
// unmounts the filesystem can be started from two places - ArduinoOTA's
// onStart() (main.cpp) and the push route (ota_push.cpp). The push side knows
// its own state, so the default already refuses while a filesystem push is
// running; main.cpp overrides it with the same answer AND its ArduinoOTA flag.
// Linking the server without main (the host tests for the profile endpoints)
// thus still gets a truthful answer.
__attribute__((weak)) bool web_fs_available() { return !ota_push_fs_unmounted(); }

static bool fsReady(AsyncWebServerRequest *request) {
  if (web_fs_available()) return true;
  sendError(request, 503, "filesystem update in progress");
  return false;
}

static void handleProfilesList(AsyncWebServerRequest *request) {
  if (!fsReady(request)) return;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  File dir = LittleFS.open(PROFILES_DIR);
  File file = dir.openNextFile();
  while (file) {
    String name = String(file.name());
    name.replace("/profiles/", "");
    name.replace(".json", "");
    arr.add(name);
    file = dir.openNextFile();
  }

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

static void handleProfileGet(AsyncWebServerRequest *request) {
  if (!fsReady(request)) return;
  if (!request->hasParam("name")) {
    sendError(request, 400, "missing name");
    return;
  }
  String name = request->getParam("name")->value();
  if (!validProfileName(name)) {
    sendError(request, 400, "invalid name");
    return;
  }
  String path = String(PROFILES_DIR) + "/" + name + ".json";

  RoastProfile p;
  if (!p.loadFromFile(path)) {
    sendError(request, 404, "profile not found");
    return;
  }

  JsonDocument doc;
  doc["name"] = name;
  doc["startTemp"] = p.startTemp();
  JsonArray steps = doc["steps"].to<JsonArray>();
  for (const auto &s : p.steps()) {
    JsonObject o = steps.add<JsonObject>();
    o["ramp"] = s.rampSeconds;
    o["hold"] = s.holdSeconds;
    o["temp"] = s.temp;
    o["fan"] = s.fan;
    if (s.rorTarget > 0.0f) {
      o["rorTarget"] = s.rorTarget;
      o["rorStart"] = s.rorStart;
      o["rorEnd"] = s.rorEnd;
    }
  }

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

static void handleProfileCreate(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                                size_t index, size_t total) {
  if (!fsReady(request)) return;
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    sendError(request, 400, "invalid json");
    return;
  }
  String name = doc["name"] | "";
  if (name.isEmpty()) {
    sendError(request, 400, "missing name");
    return;
  }
  if (!validProfileName(name)) {
    sendError(request, 400, "invalid name");
    return;
  }

  RoastProfile p;
  p.setStartTemp(doc["startTemp"] | 20.0f);
  for (JsonObject s : doc["steps"].as<JsonArray>()) {
    float rorTarget = s["rorTarget"] | 0.0f;
    if (rorTarget > 0.0f) {
      p.addStep(s["ramp"] | 0, s["hold"] | 0, s["temp"] | 0.0f, s["fan"] | 0.0f,
                rorTarget, s["rorStart"] | 0.0f, s["rorEnd"] | 0.0f);
    } else {
      p.addStep(s["ramp"] | 0, s["hold"] | 0, s["temp"] | 0.0f, s["fan"] | 0.0f);
    }
  }

  String path = String(PROFILES_DIR) + "/" + name + ".json";
  if (!p.saveToFile(path)) {
    sendError(request, 500, "could not save");
    return;
  }
  sendOk(request);
}

static void handleProfileDelete(AsyncWebServerRequest *request) {
  if (!fsReady(request)) return;
  if (!request->hasParam("name")) {
    sendError(request, 400, "missing name");
    return;
  }
  String name = request->getParam("name")->value();
  if (!validProfileName(name)) {
    sendError(request, 400, "invalid name");
    return;
  }
  String path = String(PROFILES_DIR) + "/" + name + ".json";
  if (!LittleFS.exists(path)) {
    sendError(request, 404, "profile not found");
    return;
  }
  LittleFS.remove(path);
  sendOk(request);
}

// ----------------------------------------------------------------- roast ---

static void handleRoastStart(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                             size_t index, size_t total) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    sendError(request, 400, "invalid json");
    return;
  }
  String profileName = doc["profile"] | "";
  if (profileName.isEmpty() || !cb.startRoast(profileName)) {
    sendError(request, 400, "could not start roast");
    return;
  }
  sendOk(request);
}

static void handleRoastStop(AsyncWebServerRequest *request) {
  cb.stopRoast();
  sendOk(request);
}

static void handleRoastPause(AsyncWebServerRequest *request) {
  cb.pauseRoast();
  sendOk(request);
}

static void handleRoastResume(AsyncWebServerRequest *request) {
  cb.resumeRoast();
  sendOk(request);
}

// ------------------------------------------------------------------ push OTA ---

// POST /api/update (the app image) and POST /api/update/fs (the filesystem
// image), with the image as the body, X-OTA-Token and X-OTA-SHA256 as headers.
// Every check happens before a single byte of the image is read (see
// include/ota_push.h and src/ota_push.cpp), the body is only accepted while the
// machine is idle AND the element is off, and a finished transfer restarts into
// the new image - but only after the response has been sent, otherwise the
// client would see a broken connection on an upload that actually succeeded.
//
// The route decides the partition: this handler passes the target on, and the
// state machine asks Update for U_FLASH or U_SPIFFS accordingly. There is no
// way for a client to choose the wrong one short of posting to the wrong path.
static volatile bool pushOtaReboot = false;

static void sendOtaError(AsyncWebServerRequest *request, OtaPushDecision decision) {
  int code = 400;
  const char *msg = "bad request";
  switch (decision) {
    case OtaPushDisabled:
      // No OTA_TOKEN in this build: the endpoint takes nobody's image. 503 says
      // "switched off", which is the truth, and there is nothing here to guess
      // at because no request can ever be accepted.
      code = 503;
      msg = "push OTA is disabled on this device (no token configured)";
      break;
    case OtaPushHidden:
      // Not the allowed client, or a bad token/checksum: indistinguishable
      // from a path that does not exist. 401 would confirm that this route is
      // there and worth guessing at.
      request->send(404, "text/plain", "Not Found");
      return;
    case OtaPushUnauthorized:
      code = 403;
      msg = "the image does not match the checksum that was announced";
      break;
    case OtaPushBusy:
      code = 409;
      msg = "refused: a run is active or the heater is still conducting";
      break;
    case OtaPushTooSmall:
      code = 400;
      msg = "image too small to be a filesystem image";
      break;
    case OtaPushTooLarge:
      code = 413;
      msg = "image too large for the target partition";
      break;
    case OtaPushUpdateBeginFailed:
      code = 500;
      msg = "Update.begin() failed";
      break;
    case OtaPushHashMismatch:
      code = 403;
      msg = "the image does not match the checksum that was announced";
      break;
    default:
      code = 400;
      msg = "missing or unusable Content-Length";
      break;
  }
  sendError(request, code, msg);
}

// True when a request may be answered with a real error instead of a 404
// decoy: the allowed client, with the right token and a well-formed checksum.
// A probe from anywhere else learns nothing.
static bool otaProbeIsTrusted(AsyncWebServerRequest *request) {
  char remote[24];
  snprintf(remote, sizeof(remote), "%s", request->client()->remoteIP().toString().c_str());
  const AsyncWebHeader *token = request->getHeader("X-OTA-Token");
  const AsyncWebHeader *sha = request->getHeader("X-OTA-SHA256");
  return ota_push_probe_trusted(remote, token ? token->value().c_str() : nullptr,
                                sha ? sha->value().c_str() : nullptr);
}

// GET on an update path: same rule as a POST that is refused. A client that
// pokes at the URI without being able to upload anything sees "Not Found".
static void handleOtaProbe(AsyncWebServerRequest *request) {
  if (otaProbeIsTrusted(request)) {
    sendError(request, 405, "POST the firmware image here with X-OTA-Token and X-OTA-SHA256");
    return;
  }
  request->send(404, "text/plain", "Not Found");
}

static void handleOtaFsProbe(AsyncWebServerRequest *request) {
  if (otaProbeIsTrusted(request)) {
    sendError(request, 405,
              "POST the filesystem image here with X-OTA-Token and X-OTA-SHA256");
    return;
  }
  request->send(404, "text/plain", "Not Found");
}

// Both update routes are one handler with a different target: the checks are
// identical by construction, so a rule added to one cannot be missing from the
// other. Only the partition Update is asked for differs.
static void handleOtaBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                         size_t index, size_t total, OtaPushTarget target) {
  // index == 0 is the first chunk of the body and the only place where the
  // decision is made; later chunks belong to a transfer already accepted.
  if (index == 0) {
    char remote[24];
    snprintf(remote, sizeof(remote), "%s", request->client()->remoteIP().toString().c_str());
    const AsyncWebHeader *token = request->getHeader("X-OTA-Token");
    const AsyncWebHeader *sha = request->getHeader("X-OTA-SHA256");
    OtaPushDecision decision = ota_push_begin(
        remote, token ? token->value().c_str() : nullptr,
        sha ? sha->value().c_str() : nullptr, total, target);
    if (decision != OtaPushOk) {
      // The library keeps calling this handler for the remaining chunks of the
      // same body even after the response is sent, so the refusal has to be
      // remembered for this request: without this, chunk two would fall through
      // to ota_push_write() with no transfer started, Update.write() would fail
      // and the client would see 500 "write failed" instead of the 404 that
      // deliberately hides the route. The flag is per-request, not global: a
      // rejected request must never be able to drop chunks from a legitimate
      // transfer that arrives at the same time.
      request->setAttribute("otaPushRefused", true);
      sendOtaError(request, decision);
      return;
    }
    // A transfer that is accepted can still be torn off mid-body. Register the
    // disconnect callback now, on the first chunk, so the state machine is reset
    // if the TCP connection disappears before the last chunk. Without this the
    // endpoint would stay stuck (inProgress=true) and every later push would be
    // rejected with "already running" until the device rebooted.
    request->onDisconnect([]() { ota_push_finished(false); });
  }
  if (request->getAttribute("otaPushRefused", false)) return;  // rest of a body already refused

  size_t accepted = ota_push_write(data, len);
  if (accepted != len) {
    sendError(request, 500, "write failed");
    ota_push_finished(false);
    return;
  }

  // The last chunk is the normal end of the transfer. A connection that dies
  // before this is handled by onDisconnect(), which aborts Update and clears
  // inProgress so the next request can start fresh.
  if (index + len >= total) {
    // ota_push_finished() hashes what arrived and compares it with the header.
    // A mismatch discards the written data here, before any reboot is asked
    // for, so the device keeps booting the image it is already running.
    bool ok = ota_push_finished(true);
    if (!ok) {
      sendOtaError(request, ota_push_last_transfer_rejected() ? OtaPushHashMismatch
                                                              : OtaPushUpdateBeginFailed);
      return;
    }
    request->send(200, "application/json",
                  "{\"ok\":true,\"note\":\"restarting into the new image\"}");
    pushOtaReboot = true;
  }
}

static void handleOtaUpdate(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                           size_t index, size_t total) {
  handleOtaBody(request, data, len, index, total, OtaPushTargetApp);
}

static void handleOtaFsUpdate(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                             size_t index, size_t total) {
  handleOtaBody(request, data, len, index, total, OtaPushTargetFs);
}

bool web_ota_reboot_pending() { return pushOtaReboot; }

// --------------------------------------------------------------- bootstrap ---

void web_server_init(WebServerCallbacks callbacks) {
  cb = callbacks;

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/fan", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr, handleFanSet);

  server.on("/api/manual/start", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr, handleManualStart);
  server.on("/api/manual/stop", HTTP_POST, handleManualStop);

  server.on("/api/cool/start", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr, handleCoolStart);
  server.on("/api/cool/stop", HTTP_POST, handleCoolStop);

  server.on("/api/profiles", HTTP_GET, handleProfilesList);
  server.on("/api/profiles", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr, handleProfileCreate);
  server.on("/api/profile", HTTP_GET, handleProfileGet);
  server.on("/api/profile", HTTP_DELETE, handleProfileDelete);

  server.on("/api/roast/start", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr, handleRoastStart);
  server.on("/api/roast/stop", HTTP_POST, handleRoastStop);
  server.on("/api/roast/pause", HTTP_POST, handleRoastPause);
  server.on("/api/roast/resume", HTTP_POST, handleRoastResume);

  // Push OTA. Deliberately not under /api/status's neighbourhood: this one
  // replaces the running firmware, so it is its own path with its own token.
  // /api/update writes the app slot; /api/update/fs writes the filesystem
  // partition (and unmounts LittleFS for the duration). Separate routes, same
  // checks: which partition is written follows from the path, never from the
  // body, so an image cannot land in the wrong one.
  //
  // THE FILESYSTEM ROUTE IS REGISTERED FIRST, and that is not cosmetic.
  // server.on(uri, ...) does not compare paths for equality: it registers a
  // BackwardCompatible matcher whose rule is
  //     _uri == path || path.startsWith(_uri + "/")
  // and the first handler that matches wins (ESPAsyncWebServer, WebServer.cpp:
  // canHandle / _addHandler order). So the path "/api/update/fs" matches the
  // URI "/api/update" as well - if the app route were first, a filesystem image
  // posted to /api/update/fs would be handed to the app handler, written with
  // U_FLASH into the APP SLOT and, on a matching checksum, committed as the
  // boot image. The device would then boot a filesystem as firmware.
  // The host test asserts the resulting order (routeFor() in the stub models
  // both rules).
  server.on("/api/update/fs", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr,
            handleOtaFsUpdate);
  server.on("/api/update/fs", HTTP_GET, handleOtaFsProbe);
  server.on("/api/update", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr, handleOtaUpdate);
  server.on("/api/update", HTTP_GET, handleOtaProbe);

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.begin();
}
