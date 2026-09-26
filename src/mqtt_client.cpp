#include "mqtt_client.h"
#include "config.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static const char *AVAILABILITY_TOPIC = MQTT_BASE_TOPIC "/availability";
static const char *STATUS_TOPIC = MQTT_BASE_TOPIC "/status";

// Fixed payload buffer instead of a heap String: the size is checked against
// MQTT_MAX_PAYLOAD_BYTES before publishing, so an oversized discovery config is
// reported rather than silently dropped by the client.
static char payloadBuf[MQTT_MAX_PAYLOAD_BYTES];

static WiFiClient mqttSocket;
static PubSubClient client(mqttSocket);
static MqttCallbacks cb;
static bool enabled = false;
static unsigned long lastPublish = 0;
static unsigned long lastConnectAttempt = 0;

static bool fillPayload(JsonDocument &doc) {
  size_t needed = measureJson(doc);
  if (needed >= sizeof(payloadBuf)) {
    Serial.println("[MQTT] payload too large for MQTT_MAX_PAYLOAD_BYTES - dropped");
    return false;
  }
  serializeJson(doc, payloadBuf, sizeof(payloadBuf));
  return true;
}

// ------------------------------------------------------------- discovery ---

static void addDeviceBlock(JsonDocument &doc, const char *uniqueId, const char *name) {
  doc["name"] = name;
  doc["unique_id"] = uniqueId;
  doc["availability_topic"] = AVAILABILITY_TOPIC;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";

  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"][0] = MQTT_DEVICE_ID;
  device["name"] = "Kafferostaren";
  device["manufacturer"] = "vemsom";
  device["model"] = "ESP32 coffee roaster";
  device["sw_version"] = FW_VERSION;
}

static void publishDiscoveryEntity(const char *component, const char *objectId, JsonDocument &doc) {
  String topic = String(MQTT_DISCOVERY_PREFIX) + "/" + component + "/" + MQTT_DEVICE_ID + "_" + objectId + "/config";
  if (!fillPayload(doc)) return;
  if (!client.publish(topic.c_str(), payloadBuf, true)) {
    Serial.print("[MQTT] discovery publish failed (buffer too small?): ");
    Serial.println(topic);
  }
}

static void publishDiscovery() {
  char uniqueId[64];

  // --- read-only telemetry ---
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_bt", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Bontemperatur");
    doc["device_class"] = "temperature";
    doc["unit_of_measurement"] = "\xC2\xB0" "C";
    doc["state_class"] = "measurement";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.bt }}";
    publishDiscoveryEntity("sensor", "bt", doc);
  }
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_et", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Miljotemperatur");
    doc["device_class"] = "temperature";
    doc["unit_of_measurement"] = "\xC2\xB0" "C";
    doc["state_class"] = "measurement";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.et }}";
    publishDiscoveryEntity("sensor", "et", doc);
  }
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_heater", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Varmelement");
    doc["unit_of_measurement"] = "%";
    doc["state_class"] = "measurement";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.heater }}";
    doc["icon"] = "mdi:radiator";
    publishDiscoveryEntity("sensor", "heater", doc);
  }
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_fan", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Flakt");
    doc["unit_of_measurement"] = "%";
    doc["state_class"] = "measurement";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.fan }}";
    doc["icon"] = "mdi:fan";
    publishDiscoveryEntity("sensor", "fan", doc);
  }
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_mode", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Lage");
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.mode }}";
    doc["icon"] = "mdi:state-machine";
    publishDiscoveryEntity("sensor", "mode", doc);
  }
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_elapsed", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Rosttid");
    doc["device_class"] = "duration";
    doc["unit_of_measurement"] = "s";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.elapsed }}";
    doc["icon"] = "mdi:timer-outline";
    publishDiscoveryEntity("sensor", "elapsed", doc);
  }
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_safety", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Sakerhetslarm");
    doc["device_class"] = "problem";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ 'ON' if value_json.safetyFault else 'OFF' }}";
    doc["payload_on"] = "ON";
    doc["payload_off"] = "OFF";
    doc["json_attributes_topic"] = STATUS_TOPIC;
    doc["json_attributes_template"] = "{\"reason\": \"{{ value_json.safetyReason }}\", \"mode\": \"{{ value_json.mode }}\"}";
    publishDiscoveryEntity("binary_sensor", "safety", doc);
  }

  // --- controls ---
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_fan_set", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Flakthastighet");
    doc["command_topic"] = MQTT_BASE_TOPIC "/fan/set";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.fan }}";
    doc["unit_of_measurement"] = "%";
    doc["min"] = 0;
    doc["max"] = 100;
    doc["step"] = 5;
    doc["mode"] = "slider";
    doc["icon"] = "mdi:fan";
    publishDiscoveryEntity("number", "fan_speed", doc);
  }
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_heater_set", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Varmelement duty");
    doc["command_topic"] = MQTT_BASE_TOPIC "/heater/set";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.heater }}";
    doc["unit_of_measurement"] = "%";
    doc["min"] = 0;
    doc["max"] = 100;
    doc["step"] = 5;
    doc["mode"] = "slider";
    doc["icon"] = "mdi:radiator";
    publishDiscoveryEntity("number", "heater_duty", doc);
  }

  // The select entity is only useful when there is at least one profile on
  // LittleFS, and HA rejects an empty options list.
  String names[MQTT_MAX_PROFILE_OPTIONS];
  int profileCount = cb.listProfiles ? cb.listProfiles(names, MQTT_MAX_PROFILE_OPTIONS) : 0;
  if (profileCount > 0) {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_profile", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Profil");
    JsonArray options = doc["options"].to<JsonArray>();
    for (int i = 0; i < profileCount; i++) options.add(names[i].c_str());
    doc["command_topic"] = MQTT_BASE_TOPIC "/profile/set";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.profile }}";
    doc["icon"] = "mdi:coffee-maker";
    publishDiscoveryEntity("select", "profile", doc);
  } else {
    Serial.println("[MQTT] no profiles on LittleFS - select entity skipped");
  }

  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_start", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Starta rostning");
    doc["command_topic"] = MQTT_BASE_TOPIC "/roast/start";
    doc["payload_press"] = "start";
    doc["icon"] = "mdi:play-circle-outline";
    publishDiscoveryEntity("button", "roast_start", doc);
  }
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_stop", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Stoppa rostning");
    doc["command_topic"] = MQTT_BASE_TOPIC "/roast/stop";
    doc["payload_press"] = "stop";
    doc["icon"] = "mdi:stop-circle-outline";
    publishDiscoveryEntity("button", "roast_stop", doc);
  }
}

// ---------------------------------------------------------------- status ---

void mqtt_publish_status() {
  if (!enabled || !client.connected()) return;

  JsonDocument doc;
  doc["bt"] = cb.getBT();
  doc["et"] = cb.getET();
  doc["heater"] = cb.getHeaterDuty();
  doc["fan"] = cb.getFanSpeed();
  doc["mode"] = cb.getMode();
  doc["profile"] = cb.getProfileName();
  doc["elapsed"] = cb.getElapsedSeconds();
  doc["roastActive"] = cb.getRoastActive();
  doc["safetyFault"] = cb.getSafetyFault();
  doc["safetyReason"] = cb.getSafetyReason();
  doc["uptime"] = millis() / 1000;
  doc["rssi"] = WiFi.RSSI();
  IPAddress address = WiFi.localIP();
  char ip[16];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
           (unsigned)address[0], (unsigned)address[1], (unsigned)address[2], (unsigned)address[3]);
  doc["ip"] = ip;

  if (!fillPayload(doc)) return;
  if (!client.publish(STATUS_TOPIC, payloadBuf, false)) {
    Serial.println("[MQTT] status publish failed");
  }
}

// -------------------------------------------------------------- commands ---

static void handleCommand(const String &topic, const String &payload) {
  if (topic.endsWith("/fan/set")) {
    cb.setFanSpeed(payload.toInt());
  } else if (topic.endsWith("/heater/set")) {
    cb.setHeaterDuty(payload.toFloat());
  } else if (topic.endsWith("/profile/set")) {
    if (payload.length()) cb.setSelectedProfile(payload);
  } else if (topic.endsWith("/roast/start")) {
    // Accepts either an explicit profile name or "start"/"press"/empty, in
    // which case the selected profile is used.
    String name = payload;
    if (name.length() == 0 || name.equalsIgnoreCase("start") || name.equalsIgnoreCase("press")) {
      name = cb.getProfileName();
    }
    if (name.length() == 0) {
      Serial.println("[MQTT] roast/start with no profile selected - ignored");
    } else if (!cb.startRoast(name)) {
      Serial.print("[MQTT] roast start refused (missing profile or active safety alarm): ");
      Serial.println(name);
    }
  } else if (topic.endsWith("/roast/stop")) {
    cb.stopRoast();
  }

  mqtt_publish_status();  // immediate feedback for the UI
}

static void onMessage(char *topic, byte *payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();
  Serial.print("[MQTT] <- ");
  Serial.print(topic);
  Serial.print(" : ");
  Serial.println(message);
  handleCommand(String(topic), message);
}

// ------------------------------------------------------------ connection ---

static bool connectBroker() {
  const char *user = (strlen(MQTT_USER) > 0) ? MQTT_USER : nullptr;
  const char *password = (strlen(MQTT_PASSWORD) > 0) ? MQTT_PASSWORD : nullptr;

  Serial.print("[MQTT] connecting to ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  if (!client.connect(MQTT_CLIENT_ID, user, password,
                      AVAILABILITY_TOPIC, 1, true, "offline")) {
    Serial.print("[MQTT] connect failed, state=");
    Serial.println(client.state());
    return false;
  }

  Serial.println("[MQTT] connected");
  client.publish(AVAILABILITY_TOPIC, "online", true);
  publishDiscovery();

  client.subscribe(MQTT_BASE_TOPIC "/fan/set");
  client.subscribe(MQTT_BASE_TOPIC "/heater/set");
  client.subscribe(MQTT_BASE_TOPIC "/profile/set");
  client.subscribe(MQTT_BASE_TOPIC "/roast/start");
  client.subscribe(MQTT_BASE_TOPIC "/roast/stop");

  mqtt_publish_status();
  return true;
}

void mqtt_init(MqttCallbacks callbacks) {
  cb = callbacks;

  if (strlen(MQTT_HOST) == 0 || strcmp(MQTT_HOST, "TBD") == 0) {
    enabled = false;
    Serial.println("[MQTT] MQTT_HOST not configured (see include/secrets.h) - MQTT disabled");
    return;
  }

  // The broker rejects anonymous connects (verified: CONNACK rc=5), so without
  // a configured user every reconnect would fail forever - refuse to enable it.
  if (strlen(MQTT_USER) == 0) {
    enabled = false;
    Serial.println("[MQTT] MQTT_USER not set in include/secrets.h - MQTT disabled (broker requires auth)");
    return;
  }

  enabled = true;
  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setBufferSize(MQTT_CLIENT_BUFFER_BYTES);  // default 256 is too small for discovery
  client.setKeepAlive(30);
  client.setCallback(onMessage);
  lastConnectAttempt = 0;
}

void mqtt_update() {
  if (!enabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();

  if (!client.connected()) {
    if (now - lastConnectAttempt < MQTT_RECONNECT_INTERVAL_MS) return;
    lastConnectAttempt = now;
    if (!connectBroker()) return;
    lastPublish = millis();  // connectBroker() already published a status
  }

  client.loop();

  if (now - lastPublish >= MQTT_PUBLISH_INTERVAL_MS) {
    lastPublish = now;
    mqtt_publish_status();
  }
}

bool mqtt_connected() { return enabled && client.connected(); }
