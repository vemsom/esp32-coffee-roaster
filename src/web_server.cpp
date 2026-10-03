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
    // Rate of rise, C/min, one decimal. Negative is a real value (cooling or
    // past the turning point), so it is passed straight through.
    doc["rorBt"] = cb.getRorBt();
    doc["rorEt"] = cb.getRorEt();
    doc["heaterDuty"] = cb.getHeaterDuty();
    doc["fanSpeed"] = cb.getFanSpeed();
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
    // Build language (FW_LANG_EN in include/config.h), reported so the page
    // can follow the firmware. The browser's own language must not decide:
    // the UI and the Home Assistant names have to agree.
    doc["lang"] = FW_LANG_CODE;
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

static void handleProfilesList(AsyncWebServerRequest *request) {
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

// POST /api/update with the image as the body and the token in X-OTA-Token.
// The header is checked before a single byte of the image is read, the body is
// only accepted while the machine is idle (see include/ota_push.h), and a
// finished transfer restarts into the new image - but only after the response
// has been sent, otherwise the client would see a broken connection on an
// upload that actually succeeded.
static volatile bool pushOtaReboot = false;

static void sendOtaError(AsyncWebServerRequest *request, OtaPushDecision decision) {
  int code = 400;
  const char *msg = "bad request";
  switch (decision) {
    case OtaPushDisabled:
      code = 503;
      msg = "push OTA disabled: OTA_TOKEN is not set on the device";
      break;
    case OtaPushUnauthorized:
      code = 401;
      msg = "unauthorized";
      break;
    case OtaPushBusy:
      code = 409;
      msg = "refused: a roast, manual run or cooling is active";
      break;
    case OtaPushTooLarge:
      code = 413;
      msg = "image too large for the OTA slot";
      break;
    case OtaPushUpdateBeginFailed:
      code = 500;
      msg = "Update.begin() failed";
      break;
    default:
      code = 400;
      msg = "missing or unusable Content-Length";
      break;
  }
  sendError(request, code, msg);
}

static void handleOtaUpdate(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                           size_t index, size_t total) {
  // index == 0 is the first chunk of the body and the only place where the
  // decision is made; later chunks belong to a transfer already accepted.
  if (index == 0) {
    const AsyncWebHeader *header = request->getHeader("X-OTA-Token");
    OtaPushDecision decision =
        ota_push_begin(header ? header->value().c_str() : nullptr, total);
    if (decision != OtaPushOk) {
      sendOtaError(request, decision);
      return;
    }
  }

  size_t accepted = ota_push_write(data, len);
  if (accepted != len) {
    sendError(request, 500, "write failed");
    ota_push_finished(false);
    return;
  }

  // The library sends final=true on the last chunk; index+len == total is the
  // same moment, and a body handler that sees neither (connection died) falls
  // through to the abort in ota_push_finished(false) on the next request.
  if (index + len >= total) {
    bool ok = ota_push_finished(true);
    if (!ok) {
      sendError(request, 500, "Update.end() failed");
      return;
    }
    request->send(200, "application/json",
                  "{\"ok\":true,\"note\":\"restarting into the new image\"}");
    pushOtaReboot = true;
  }
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
  server.on("/api/update", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr, handleOtaUpdate);

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.begin();
}
