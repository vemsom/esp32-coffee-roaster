#include "mqtt_client.h"
#include "config.h"
#include "state_lock.h"
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
  // Fan interlock, reported separately from the safety latch on purpose: the
  // element is being held off because airflow is missing, which is a different
  // problem with a different fix. Self-clearing, so no latching semantics.
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_fan_fault", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Flaktsparr");
    doc["device_class"] = "problem";
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ 'ON' if value_json.fanFault else 'OFF' }}";
    doc["payload_on"] = "ON";
    doc["payload_off"] = "OFF";
    doc["icon"] = "mdi:fan-off";
    publishDiscoveryEntity("binary_sensor", "fan_fault", doc);
  }

  // Profile is reported as a plain sensor. MQTT is report-only: no number,
  // no select, no buttons, no command topics - see docs/firmware-notes.md.
  {
    JsonDocument doc;
    snprintf(uniqueId, sizeof(uniqueId), "%s_profile", MQTT_DEVICE_ID);
    addDeviceBlock(doc, uniqueId, "Profil");
    doc["state_topic"] = STATUS_TOPIC;
    doc["value_template"] = "{{ value_json.profile }}";
    doc["icon"] = "mdi:coffee-maker";
    publishDiscoveryEntity("sensor", "profile", doc);
  }
}

// ---------------------------------------------------------------- status ---

void mqtt_publish_status() {
  if (!enabled || !client.connected()) return;

  JsonDocument doc;
  // The whole snapshot is assembled under the state lock. The getters take it
  // themselves anyway (the lock is recursive), but taking it around the block
  // as well is what keeps one payload from straddling a change - reading the
  // profile name buffer while a concurrent profile start rewrites it was a
  // real race, caught by ThreadSanitizer in the host tests. Deliberately
  // released before the publish, which can block on the network.
  {
    StateLockGuard guard;
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
    doc["fanFault"] = cb.getFanFault();
    doc["uptime"] = millis() / 1000;
    doc["rssi"] = WiFi.RSSI();
    IPAddress address = WiFi.localIP();
    char ip[16];
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
             (unsigned)address[0], (unsigned)address[1],
             (unsigned)address[2], (unsigned)address[3]);
    doc["ip"] = ip;
  }

  if (!fillPayload(doc)) return;
  if (!client.publish(STATUS_TOPIC, payloadBuf, false)) {
    Serial.println("[MQTT] status publish failed");
  }
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

  // Report-only: no subscriptions at all, the roaster is never commanded
  // over MQTT (see docs/firmware-notes.md).

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
