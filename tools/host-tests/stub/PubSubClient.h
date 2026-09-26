#pragma once
// Recording PubSubClient stub. Everything the firmware publishes or subscribes
// to lands in the globals below so the test can assert on the real payloads.
// A publish that would not fit in the configured buffer fails the way the real
// client does, which is what checks the firmware's buffer sizing.
#include <Arduino.h>
#include <WiFi.h>
#include <string>
#include <vector>

struct CapturedPublish {
  std::string topic;
  std::string payload;
  bool retained;
};

typedef void (*PubSubCallback)(char *topic, uint8_t *payload, unsigned int length);

// Defined by each test binary.
extern std::vector<CapturedPublish> g_published;
extern std::vector<std::string> g_subscriptions;
extern std::string g_connectUser;
extern std::string g_connectPass;
extern std::string g_connectClientId;
extern std::string g_connectWillTopic;
extern PubSubCallback g_callback;
extern uint16_t g_bufferSize;
extern bool g_connected;

class PubSubClient {
 public:
  explicit PubSubClient(WiFiClient &) {}

  void setServer(const char *, uint16_t) {}
  void setBufferSize(uint16_t size) { g_bufferSize = size; }
  void setKeepAlive(uint16_t) {}
  void setCallback(PubSubCallback cb) { g_callback = cb; }

  bool connect(const char *id, const char *user, const char *pass,
               const char *willTopic, uint8_t, bool, const char *) {
    g_connectUser = user ? user : "";
    g_connectPass = pass ? pass : "";
    g_connectClientId = id ? id : "";
    g_connectWillTopic = willTopic ? willTopic : "";
    if (g_connectUser.empty()) return false;  // this broker requires auth (rc=5)
    g_connected = true;
    return true;
  }

  bool publish(const char *topic, const char *payload, bool retained) {
    std::string t = topic ? topic : "";
    std::string p = payload ? payload : "";
    if (t.size() + p.size() + 8 > g_bufferSize) return false;
    g_published.push_back({t, p, retained});
    return true;
  }

  bool subscribe(const char *topic) {
    g_subscriptions.push_back(topic ? topic : "");
    return true;
  }

  bool connected() { return g_connected; }
  void loop() {}
  int state() { return 0; }
};
