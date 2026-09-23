#include "web_server.h"
#include "config.h"
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "roast_profile.h"

static AsyncWebServer server(80);
static WebServerCallbacks cb;

static void handleStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
    doc["bt"] = cb.getBT();
      doc["et"] = cb.getET();
        doc["heaterDuty"] = cb.getHeaterDuty();
          doc["fanSpeed"] = cb.getFanSpeed();
            doc["roastActive"] = cb.getRoastActive();
              doc["elapsedSeconds"] = cb.getElapsedSeconds();

                String out;
                  serializeJson(doc, out);
                    request->send(200, "application/json", out);
                    }

                    static void handleFanSet(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
                      JsonDocument doc;
                        if (deserializeJson(doc, data, len)) {
                            request->send(400, "application/json", "{\"error\":\"invalid json\"}");
                                return;
                                  }
                                    int speed = doc["speed"] | 0;
                                      cb.setFanSpeed(speed);
                                        request->send(200, "application/json", "{\"ok\":true}");
                                        }

                                        static void handleHeaterManualSet(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
                                          JsonDocument doc;
                                            if (deserializeJson(doc, data, len)) {
                                                request->send(400, "application/json", "{\"error\":\"invalid json\"}");
                                                    return;
                                                      }
                                                        float duty = doc["duty"] | 0.0;
                                                          cb.setManualHeaterDuty(duty);
                                                            request->send(200, "application/json", "{\"ok\":true}");
                                                            }

                                                            static void handleProfilesList(AsyncWebServerRequest *request) {
                                                              JsonDocument doc;
                                                                JsonArray arr = doc.to<JsonArray>();

                                                                  File dir = LittleFS.open(PROFILES_DIR);
                                                                    File file = dir.openNextFile();
                                                                      while (file) {
                                                                          arr.add(String(file.name()));
                                                                              file = dir.openNextFile();
                                                                                }

                                                                                  String out;
                                                                                    serializeJson(doc, out);
                                                                                      request->send(200, "application/json", out);
                                                                                      }

                                                                                      static void handleProfileCreate(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
                                                                                        JsonDocument doc;
                                                                                          if (deserializeJson(doc, data, len)) {
                                                                                              request->send(400, "application/json", "{\"error\":\"invalid json\"}");
                                                                                                  return;
                                                                                                    }
                                                                                                      String name = doc["name"] | "";
                                                                                                        if (name.isEmpty()) {
                                                                                                            request->send(400, "application/json", "{\"error\":\"missing name\"}");
                                                                                                                return;
                                                                                                                  }
                                                                                                                  
                                                                                                                    RoastProfile p;
                                                                                                                      for (JsonObject pt : doc["points"].as<JsonArray>()) {
                                                                                                                          p.addPoint(pt["t"].as<unsigned long>(), pt["temp"].as<float>());
                                                                                                                            }
                                                                                                                            
                                                                                                                              String path = String(PROFILES_DIR) + "/" + name + ".json";
                                                                                                                                if (!p.saveToFile(path)) {
                                                                                                                                    request->send(500, "application/json", "{\"error\":\"could not save\"}");
                                                                                                                                        return;
                                                                                                                                          }
                                                                                                                                            request->send(200, "application/json", "{\"ok\":true}");
                                                                                                                                            }
                                                                                                                                            
                                                                                                                                            static void handleRoastStart(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
                                                                                                                                              JsonDocument doc;
                                                                                                                                                if (deserializeJson(doc, data, len)) {
                                                                                                                                                    request->send(400, "application/json", "{\"error\":\"invalid json\"}");
                                                                                                                                                        return;
                                                                                                                                                          }
                                                                                                                                                            String profileName = doc["profile"] | "";
                                                                                                                                                              if (profileName.isEmpty() || !cb.startRoast(profileName)) {
                                                                                                                                                                  request->send(400, "application/json", "{\"error\":\"could not start roast\"}");
                                                                                                                                                                      return;
                                                                                                                                                                        }
                                                                                                                                                                          request->send(200, "application/json", "{\"ok\":true}");
                                                                                                                                                                          }
                                                                                                                                                                          
                                                                                                                                                                          static void handleRoastStop(AsyncWebServerRequest *request) {
                                                                                                                                                                            cb.stopRoast();
                                                                                                                                                                              request->send(200, "application/json", "{\"ok\":true}");
                                                                                                                                                                              }
                                                                                                                                                                              
                                                                                                                                                                              void web_server_init(WebServerCallbacks callbacks) {
                                                                                                                                                                                cb = callbacks;
                                                                                                                                                                                
                                                                                                                                                                                  server.on("/api/status", HTTP_GET, handleStatus);
                                                                                                                                                                                    server.on("/api/fan", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, handleFanSet);
                                                                                                                                                                                      server.on("/api/heater/manual", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, handleHeaterManualSet);
                                                                                                                                                                                        server.on("/api/profiles", HTTP_GET, handleProfilesList);
                                                                                                                                                                                          server.on("/api/profiles", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, handleProfileCreate);
                                                                                                                                                                                            server.on("/api/roast/start", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, handleRoastStart);
                                                                                                                                                                                              server.on("/api/roast/stop", HTTP_POST, handleRoastStop);
                                                                                                                                                                                              
                                                                                                                                                                                                server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
                                                                                                                                                                                                
                                                                                                                                                                                                  server.begin();
                                                                                                                                                                                                  }
                                                                                                                                                                                                  
