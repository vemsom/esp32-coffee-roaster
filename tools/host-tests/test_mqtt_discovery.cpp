// Host-side test of the MQTT layer: drives mqtt_client.cpp with a recording
// PubSubClient stub and asserts on the real published payloads, so the Home
// Assistant discovery configs can be checked without a broker or an ESP32.
//
// MQTT is report-only by design, so this test also asserts the absence of
// control: no subscriptions, no message callback, no command_topic on any
// entity, and no topic outside coffee_roaster/ and homeassistant/.
//
// If include/secrets.h exists it is used. Without it, run.sh passes
// -DMQTT_HOST/-DMQTT_USER/-DMQTT_PASSWORD for BOTH translation units (macros do
// not cross translation units, so defining them here would only affect this
// file and mqtt_client.cpp would still see the "TBD" placeholder).

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mqtt_client.h"
#include "config.h"
#include "strings.h"   // STR_NAME_* / STR_DEVICE_NAME - the names under test

// ---- globals required by the stubs ----
std::vector<CapturedPublish> g_published;
std::vector<std::string> g_subscriptions;
std::string g_connectUser;
std::string g_connectPass;
std::string g_connectClientId;
std::string g_connectWillTopic;
PubSubCallback g_callback = nullptr;
uint16_t g_bufferSize = 256;
bool g_connected = false;

static unsigned long fakeMillis = 0;
unsigned long millis() { return fakeMillis; }
void delay(unsigned long) {}
void delayMicroseconds(unsigned int) {}
void pinMode(int, int) {}
void digitalWrite(int, int) {}
int digitalRead(int) { return 0; }
SerialStub Serial;
WiFiClass WiFi;

// ---- fake state for the status getters ----
static float gb() { return 187.5f; }
static float ge() { return 231.25f; }
static float gh() { return 42.0f; }
static int gf() { return 65; }
static const char *gm() { return "profile"; }
static const char *gp() { return "test-profile"; }
static unsigned long gms() { return 61; }
static bool gsf() { return false; }
static const char *gsr() { return "none"; }
static bool gra() { return true; }
static bool gff() { return false; }   // fan interlock: not tripped in this test

// ---- test helpers ----
static int checks = 0;
static int failures = 0;

static void check(bool cond, const char *what) {
  checks++;
  if (cond) {
    printf("ok   %s\n", what);
  } else {
    printf("FAIL %s\n", what);
    failures++;
  }
}

static const CapturedPublish *findPublish(const std::string &topic) {
  for (const auto &p : g_published) {
    if (p.topic == topic) return &p;
  }
  return nullptr;
}

static bool startsWith(const std::string &s, const std::string &prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

// Home Assistant derives entity_id from the discovery `name`: slugify() folds
// the Latin-1 letters this firmware uses down to ASCII (a-umlaut and
// a-ring -> 'a', o-umlaut -> 'o'), turns spaces into underscores and
// lowercases. Reproduce that here so a friendly-name rename that would mint a
// NEW entity_id fails the test - the name is the only language-dependent part
// of a discovery config, so it is the only thing a language switch may move.
static std::string haSlug(const std::string &name) {
  std::string out;
  for (size_t i = 0; i < name.size(); i++) {
    unsigned char c = (unsigned char)name[i];
    if (c == 0xC3 && i + 1 < name.size()) {
      unsigned char d = (unsigned char)name[i + 1];
      if (d == 0xA4 || d == 0xA5) { out += 'a'; i++; continue; }  // ä, å
      if (d == 0xB6) { out += 'o'; i++; continue; }               // ö
    }
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
    if (c == ' ') c = '_';
    out += (char)c;
  }
  return out;
}

// argv[1], if given, is a path the unique_id set is written to. run.sh runs
// this binary once per build language and diffs the two dumps: identical
// unique_ids across languages is exactly the promise include/strings.h makes.
int main(int argc, char **argv) {
  MqttCallbacks cb = {gb, ge, gh, gf, gm, gp, gms, gsf, gsr, gra, gff};

  mqtt_init(cb);

  if (strlen(MQTT_HOST) == 0 || strcmp(MQTT_HOST, "TBD") == 0 || strlen(MQTT_USER) == 0) {
    printf("no broker configured (MQTT_HOST/TBD or MQTT_USER empty) - nothing to test\n");
    return 0;
  }

  fakeMillis = 60000;
  mqtt_update();

  check(mqtt_connected(), "connects on the first mqtt_update");
  check(!g_connectUser.empty(), "connect carries credentials (broker rejects anonymous)");
  check(g_connectWillTopic == std::string(MQTT_BASE_TOPIC) + "/availability",
        "last will is set on the availability topic");

  const std::string base(MQTT_BASE_TOPIC);
  const std::string status = base + "/status";
  const std::string avail = base + "/availability";

  const CapturedPublish *online = findPublish(avail);
  check(online != nullptr && online->payload == "online" && online->retained,
        "availability published as retained 'online'");

  // ------------------------------------------- report-only guarantees -------
  check(g_subscriptions.empty(), "subscribes to nothing at all");
  check(g_callback == nullptr, "no message callback is registered");

  // ---------------------------------------------------- discovery payload ---
  std::set<std::string> uniqueIds;
  std::set<std::string> components;
  int discoveryCount = 0;
  int sensorCount = 0;
  int binarySensorCount = 0;
  bool commandTopicsFound = false;
  bool safetySeen = false;
  bool fanFaultSeen = false;
  bool profileSensorSeen = false;
  bool topicsInsideNamespace = true;
  int namesChecked = 0;

  // object-suffix -> { friendly name of THIS build, entity_id slug HA derives
  // from it }. Keyed on the part of the discovery topic after <device>_, i.e.
  // the object_id that must not change. The name is taken from the macro in
  // include/strings.h, so this fails if the language switch does not reach the
  // payload; the slug is pinned per language, so it fails if the name a fresh
  // Home Assistant would mint is not the one intended. What proves the switch
  // is SAFE is the other half: object_id and unique_id are byte-identical in
  // both languages, which run.sh checks by running this binary once per
  // language and diffing the unique_id dumps.
#if FW_LANG_EN
  const std::map<std::string, std::pair<std::string, std::string>> expectedNames = {
      {"bt", {STR_NAME_BT, "bean_temperature"}},
      {"et", {STR_NAME_ET, "environment_temperature"}},
      {"heater", {STR_NAME_HEATER, "heater"}},
      {"fan", {STR_NAME_FAN, "fan"}},
      {"mode", {STR_NAME_MODE, "mode"}},
      {"elapsed", {STR_NAME_ELAPSED, "roast_time"}},
      {"safety", {STR_NAME_SAFETY, "safety_alarm"}},
      {"fan_fault", {STR_NAME_FAN_FAULT, "fan_interlock"}},
      {"profile", {STR_NAME_PROFILE, "profile"}},
  };
#else
  const std::map<std::string, std::pair<std::string, std::string>> expectedNames = {
      {"bt", {STR_NAME_BT, "bontemperatur"}},
      {"et", {STR_NAME_ET, "miljotemperatur"}},
      {"heater", {STR_NAME_HEATER, "varmelement"}},
      {"fan", {STR_NAME_FAN, "flakt"}},
      {"mode", {STR_NAME_MODE, "lage"}},
      {"elapsed", {STR_NAME_ELAPSED, "rosttid"}},
      {"safety", {STR_NAME_SAFETY, "sakerhetslarm"}},
      {"fan_fault", {STR_NAME_FAN_FAULT, "flaktsparr"}},
      {"profile", {STR_NAME_PROFILE, "profil"}},
  };
#endif
  printf("build language: %s\n", FW_LANG_CODE);

  const std::string discoveryPrefix = std::string(MQTT_DISCOVERY_PREFIX) + "/";
  for (const auto &p : g_published) {
    if (!startsWith(p.topic, base + "/") && !startsWith(p.topic, discoveryPrefix)) {
      topicsInsideNamespace = false;
      printf("     unexpected topic: %s\n", p.topic.c_str());
    }
    if (!startsWith(p.topic, discoveryPrefix)) continue;
    discoveryCount++;

    // homeassistant/<component>/<device>_<object>/config
    std::string rest = p.topic.substr(discoveryPrefix.size());
    size_t slash1 = rest.find('/');
    size_t slash2 = rest.find('/', slash1 + 1);
    check(slash1 != std::string::npos && slash2 != std::string::npos,
          "discovery topic has component/object/config shape");
    if (slash1 == std::string::npos || slash2 == std::string::npos) continue;
    std::string component = rest.substr(0, slash1);
    std::string objectId = rest.substr(slash1 + 1, slash2 - slash1 - 1);
    std::string leaf = rest.substr(slash2 + 1);
    components.insert(component);
    if (component == "sensor") sensorCount++;
    check(leaf == "config", "discovery topic ends in /config");
    check(startsWith(objectId, std::string(MQTT_DEVICE_ID) + "_"),
          "object id starts with the device id");

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, p.payload);
    if (err) {
      printf("FAIL discovery payload is not JSON: %s\n", p.payload.c_str());
      checks++;
      failures++;
      continue;
    }

    check(doc["name"].is<const char *>(), "has name");
    check(doc["unique_id"].is<const char *>(), "has unique_id");
    check(doc["availability_topic"].is<const char *>(), "has availability_topic");
    check(doc["device"]["identifiers"][0].is<const char *>(), "has device identifiers");
    // The device block name follows the build language too (STR_DEVICE_NAME).
    {
      std::string wantDevice = std::string("device block name is ") + STR_DEVICE_NAME;
      bool ok = doc["device"]["name"].is<const char *>() &&
                std::string(doc["device"]["name"].as<const char *>()) == STR_DEVICE_NAME;
      check(ok, wantDevice.c_str());
    }
    if (doc["unique_id"].is<const char *>()) {
      bool fresh = uniqueIds.insert(doc["unique_id"].as<const char *>()).second;
      check(fresh, "unique_id is unique across entities");
    }
    check(p.retained, "discovery config is retained");

    // ---- friendly name + entity_id stability ----
    std::string suffix =
        objectId.substr(std::string(MQTT_DEVICE_ID).size() + 1);  // drop "<device>_"
    auto expected = expectedNames.find(suffix);
    if (expected == expectedNames.end()) {
      printf("FAIL unexpected entity object id: %s\n", objectId.c_str());
      checks++;
      failures++;
    } else {
      namesChecked++;
      if (doc["name"].is<const char *>()) {
        std::string friendly = doc["name"].as<const char *>();
        std::string wantName = "name of " + suffix + " is " + expected->second.first;
        check(friendly == expected->second.first, wantName.c_str());
        std::string wantSlug = "slug of " + suffix + " in this build is " + expected->second.second;
        check(haSlug(friendly) == expected->second.second, wantSlug.c_str());
      }
      // unique_id and object_id are what HA keys the entity on - they may not
      // move with the rename.
      if (doc["unique_id"].is<const char *>()) {
        check(std::string(doc["unique_id"].as<const char *>()) == objectId,
              "unique_id equals the object id (unchanged by a rename)");
      }
    }

    if (doc["command_topic"].is<const char *>()) {
      commandTopicsFound = true;
      printf("     command_topic on %s\n", p.topic.c_str());
    }

    check(doc["state_topic"].is<const char *>(), "entity has state_topic");
    if (doc["state_topic"].is<const char *>()) {
      check(std::string(doc["state_topic"].as<const char *>()) == status,
            "state_topic points at coffee_roaster/status");
    }

    if (component == "binary_sensor") {
      binarySensorCount++;
      if (objectId == std::string(MQTT_DEVICE_ID) + "_safety") {
        safetySeen = true;
        check(std::string(doc["device_class"].as<const char *>()) == "problem",
              "safety entity uses device_class 'problem'");
      } else if (objectId == std::string(MQTT_DEVICE_ID) + "_fan_fault") {
        fanFaultSeen = true;
        check(std::string(doc["device_class"].as<const char *>()) == "problem",
              "fan interlock entity uses device_class 'problem'");
      }
    }
    if (objectId == std::string(MQTT_DEVICE_ID) + "_profile") {
      profileSensorSeen = true;
    }
  }

  check(topicsInsideNamespace, "every topic lives under coffee_roaster/ or homeassistant/");
  check(!commandTopicsFound, "no entity has a command_topic");
  check(discoveryCount == 9, "9 discovery configs published");
  check(sensorCount == 7, "7 read-only sensors (bt, et, heater, fan, mode, elapsed, profile)");
  check(binarySensorCount == 2, "2 binary_sensors (safety + fan interlock)");
  check(components.size() == 2, "only sensor and binary_sensor components");
  check(components.count("number") == 0 && components.count("select") == 0 &&
            components.count("button") == 0 && components.count("switch") == 0,
        "no controllable entity types published");
  check(safetySeen, "safety alarm entity published");
  check(fanFaultSeen, "fan interlock entity published");
  check(profileSensorSeen, "profile reported as a sensor");
  check(namesChecked == 9, "all 9 entities carry an expected friendly name");

  // ---------------------------------------------------------- status JSON ---
  const CapturedPublish *statusPub = findPublish(status);
  check(statusPub != nullptr, "status topic published");
  if (statusPub) {
    JsonDocument doc;
    check(!deserializeJson(doc, statusPub->payload), "status payload is valid JSON");
    check(doc["bt"].as<float>() == 187.5f, "status carries bt");
    check(doc["et"].as<float>() == 231.25f, "status carries et");
    check(doc["heater"].as<float>() == 42.0f, "status carries heater");
    check(doc["fan"].as<int>() == 65, "status carries fan");
    check(std::string(doc["mode"].as<const char *>()) == "profile", "status carries mode");
    check(std::string(doc["profile"].as<const char *>()) == "test-profile",
          "status carries the selected profile");
    check(doc["elapsed"].as<unsigned long>() == 61, "status carries elapsed seconds");
    check(doc["safetyFault"].is<bool>() && !doc["safetyFault"].as<bool>(),
          "status carries safetyFault");
    check(doc["fanFault"].is<bool>() && !doc["fanFault"].as<bool>(),
          "status carries fanFault");
    // The stub WiFi (tools/host-tests/stub/WiFi.h) owns the address; build the
    // expectation from it so the fixture stays a fixture and not a literal.
    IPAddress localIp = WiFi.localIP();
    char expectIp[16];
    snprintf(expectIp, sizeof(expectIp), "%u.%u.%u.%u", (unsigned)localIp[0],
             (unsigned)localIp[1], (unsigned)localIp[2], (unsigned)localIp[3]);
    check(std::string(doc["ip"].as<const char *>()) == expectIp, "status carries ip");
    check(!statusPub->retained, "status is not retained");
  }

  // ------------------------------------------------------------ size limits ---
  size_t biggest = 0;
  bool sizesOk = true;
  for (const auto &p : g_published) {
    biggest = std::max(biggest, p.payload.size());
    if (p.payload.size() >= MQTT_MAX_PAYLOAD_BYTES) sizesOk = false;
    if (p.topic.size() + p.payload.size() + 8 > MQTT_CLIENT_BUFFER_BYTES) sizesOk = false;
  }
  check(sizesOk, "every payload fits the PubSubClient buffer");
  printf("     biggest payload: %zu bytes (limit %d)\n", biggest, MQTT_MAX_PAYLOAD_BYTES);

  // -------------------------------------------- status keeps flowing --------
  int before = (int)g_published.size();
  mqtt_publish_status();
  check((int)g_published.size() == before + 1, "status can be republished on demand");

  fakeMillis += MQTT_PUBLISH_INTERVAL_MS + 1;
  int beforeTick = (int)g_published.size();
  mqtt_update();
  check((int)g_published.size() == beforeTick + 1, "status is republished on the interval");

  // ---------------------------------------------------- unique_id dump ---
  // Sorted, one per line, so the two language builds diff byte for byte.
  if (argc > 1) {
    std::ofstream dump(argv[1]);
    check(dump.good(), "unique_id dump can be written");
    for (const auto &id : uniqueIds) dump << id << "\n";
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
