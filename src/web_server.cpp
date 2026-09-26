#include "web_server.h"
#include "config.h"
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
  doc["bt"] = cb.getBT();
  doc["et"] = cb.getET();
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

  RoastProfile p;
  p.setStartTemp(doc["startTemp"] | 20.0f);
  for (JsonObject s : doc["steps"].as<JsonArray>()) {
    p.addStep(s["ramp"] | 0, s["hold"] | 0, s["temp"] | 0.0f, s["fan"] | 0.0f);
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

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.begin();
}
