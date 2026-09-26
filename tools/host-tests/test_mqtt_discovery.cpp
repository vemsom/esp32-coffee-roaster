// Host-side test of the MQTT layer: drives mqtt_client.cpp with a recording
// PubSubClient stub and asserts on the real published payloads, so the Home
// Assistant discovery configs and the command handling can be checked without a
// broker or an ESP32.
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
#include <set>
#include <vector>

#include "mqtt_client.h"
#include "config.h"

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

// ---- fake state for the callbacks ----
static int fanSet = -1;
static float heaterSet = -1;
static std::string profileSet;
static std::string selectedProfile = "test-profile";  // mirrors main.cpp's selectedProfileName
static std::string startedProfile;
static bool started = false;
static bool stopped = false;

static float gb() { return 187.5f; }
static float ge() { return 231.25f; }
static float gh() { return 42.0f; }
static int gf() { return 65; }
static const char *gm() { return "profile"; }
static const char *gp() { return selectedProfile.c_str(); }
static unsigned long gms() { return 61; }
static bool gsf() { return false; }
static const char *gsr() { return "none"; }
static bool gra() { return true; }

static int glist(String *out, int maxOut) {
  const char *names[] = {"test-profile", "city", "espresso"};
  int n = (int)(sizeof(names) / sizeof(names[0]));
  if (n > maxOut) n = maxOut;
  for (int i = 0; i < n; i++) out[i] = names[i];
  return n;
}

static void gsetFan(int p) { fanSet = p; }
static void gsetHeater(float p) { heaterSet = p; }
static void gsetProfile(const String &n) {
  profileSet = n.c_str();
  selectedProfile = profileSet;
}
static bool gstart(const String &n) { started = true; startedProfile = n.c_str(); return true; }
static void gstop() { stopped = true; }

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

static void inject(const char *topic, const char *payload) {
  if (!g_callback) return;
  g_callback(const_cast<char *>(topic),
             reinterpret_cast<uint8_t *>(const_cast<char *>(payload)),
             (unsigned int)strlen(payload));
}

int main() {
  MqttCallbacks cb = {gb, ge, gh, gf, gm, gp, gms, gsf, gsr, gra, glist,
                      gsetFan, gsetHeater, gsetProfile, gstart, gstop};

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

  // ---------------------------------------------------- discovery payload ---
  std::set<std::string> uniqueIds;
  std::set<std::string> components;
  int discoveryCount = 0;
  bool selectSeen = false;
  bool safetySeen = false;

  const std::string discoveryPrefix = std::string(MQTT_DISCOVERY_PREFIX) + "/";
  for (const auto &p : g_published) {
    if (p.topic.compare(0, discoveryPrefix.size(), discoveryPrefix) != 0) continue;
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
    check(leaf == "config", "discovery topic ends in /config");
    check(objectId.rfind(MQTT_DEVICE_ID "_", 0) == 0, "object id starts with the device id");

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
    check(doc["availability_topic"].is<const char*>(), "has availability_topic");
    check(doc["device"]["identifiers"][0].is<const char *>(), "has device identifiers");
    if (doc["unique_id"].is<const char *>()) {
      bool fresh = uniqueIds.insert(doc["unique_id"].as<const char *>()).second;
      check(fresh, "unique_id is unique across entities");
    }
    check(p.retained, "discovery config is retained");

    // entities driven by the status payload must point at it
    bool needsState = (component == "sensor" || component == "binary_sensor" ||
                       component == "number" || component == "select");
    if (needsState) {
      check(doc["state_topic"].is<const char *>(), "stateful entity has state_topic");
      if (doc["state_topic"].is<const char *>()) {
        check(std::string(doc["state_topic"].as<const char *>()) == status,
              "state_topic points at coffee_roaster/status");
      }
    }
    if (component == "number" || component == "select" || component == "button") {
      check(doc["command_topic"].is<const char *>(), "command entity has command_topic");
      if (doc["command_topic"].is<const char *>()) {
        check(std::string(doc["command_topic"].as<const char *>()).rfind(base + "/", 0) == 0,
              "command topic lives under the coffee_roaster base");
      }
    }

    if (component == "binary_sensor") {
      safetySeen = true;
      check(std::string(doc["device_class"].as<const char *>()) == "problem",
            "safety entity uses device_class 'problem'");
    }
    if (component == "select") {
      selectSeen = true;
      JsonArray opts = doc["options"].as<JsonArray>();
      check(opts.size() == 3, "select has the 3 profiles from LittleFS");
      if (opts.size() == 3) {
        check(std::string(opts[0].as<const char *>()) == "test-profile" &&
                  std::string(opts[1].as<const char *>()) == "city" &&
                  std::string(opts[2].as<const char *>()) == "espresso",
              "select options match the profile names");
      }
    }
  }

  check(discoveryCount == 12, "12 discovery configs published (6 sensors + safety + 2 numbers + select + 2 buttons)");
  check(components.count("sensor") == 1, "sensor component present");
  check(components.count("binary_sensor") == 1, "binary_sensor component present");
  check(components.count("number") == 1, "number component present");
  check(components.count("select") == 1, "select component present");
  check(components.count("button") == 1, "button component present");
  check(safetySeen, "safety alarm entity published");
  check(selectSeen, "profile select entity published");

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
    check(std::string(doc["ip"].as<const char *>()) == "192.168.1.173", "status carries ip");
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

  // ------------------------------------------------------------- commands ---
  std::vector<std::string> expectedSubs = {
      base + "/fan/set", base + "/heater/set", base + "/profile/set",
      base + "/roast/start", base + "/roast/stop"};
  check(g_subscriptions == expectedSubs, "subscribed to the five command topics");

  int publishesBefore = (int)g_published.size();

  inject((base + "/fan/set").c_str(), "75");
  check(fanSet == 75, "fan/set is forwarded to setFanSpeed");

  inject((base + "/heater/set").c_str(), "40.5");
  check(heaterSet == 40.5f, "heater/set is forwarded to setHeaterDuty");

  inject((base + "/profile/set").c_str(), "city");
  check(profileSet == "city", "profile/set selects a profile");

  started = false;
  inject((base + "/roast/start").c_str(), "espresso");
  check(started && startedProfile == "espresso", "roast/start with a name starts that profile");

  started = false;
  inject((base + "/roast/start").c_str(), "start");
  check(started && startedProfile == "city", "roast/start 'start' uses the selected profile");

  inject((base + "/roast/stop").c_str(), "x");
  check(stopped, "roast/stop stops");

  int publishesAfterCommands = (int)g_published.size();
  check(publishesAfterCommands > publishesBefore, "commands trigger an immediate status publish");

  // Unknown topic on the same base must be ignored (we only subscribe to the
  // five known ones, but the handler must not act on a stray message either).
  started = false;
  fanSet = -1;
  inject((base + "/roast/whatever").c_str(), "1");
  inject("tibber/whatever", "1");
  check(!started && fanSet == -1, "unknown topics are ignored");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
