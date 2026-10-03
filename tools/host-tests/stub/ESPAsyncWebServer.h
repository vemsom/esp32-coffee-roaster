#pragma once
// Host stub for ESPAsyncWebServer: web_server.cpp registers its routes here
// exactly as it does on the device, and a test dispatches a synthetic request
// at them. The response is recorded on the request object, so a test can
// assert on status code and body with no network and no ESP32.
//
// The handler typedefs and the on() signatures mirror the real library, so
// the same src/web_server.cpp compiles against both without a #ifdef.
//
// The registered server in web_server.cpp is file-static and unreachable
// from outside that translation unit, so every instance puts itself in a
// registry: that is how a test finds the routes to dispatch at.
#include <Arduino.h>
#include <WiFi.h>  // IPAddress: request->client()->remoteIP() returns one
#include <functional>
#include <map>
#include <string>
#include <vector>

enum WebRequestMethod : uint32_t {
  HTTP_GET = 1u << 1,
  HTTP_POST = 1u << 2,
  HTTP_DELETE = 1u << 3,
  HTTP_PUT = 1u << 4,
  HTTP_HEAD = 1u << 5,
  HTTP_PATCH = 1u << 6,
  HTTP_OPTIONS = 1u << 7,
  HTTP_ANY = 0xFFFFu,
};

// The real library wraps the bit mask so methods can be OR-ed together; only
// equality is needed to dispatch, but the shape stays the same.
class WebRequestMethodComposite {
 public:
  constexpr WebRequestMethodComposite() = default;
  constexpr WebRequestMethodComposite(uint32_t mask) : _mask(mask) {}
  constexpr WebRequestMethodComposite(WebRequestMethod method) : _mask(method) {}
  constexpr bool operator==(const WebRequestMethodComposite &o) const { return _mask == o._mask; }
  constexpr bool operator!=(const WebRequestMethodComposite &o) const { return _mask != o._mask; }

 private:
  uint32_t _mask = 0;
};

class AsyncWebServerRequest;   // forward: the handler typedefs below need it

typedef std::function<void(AsyncWebServerRequest *request)> ArRequestHandlerFunction;
typedef std::function<void(AsyncWebServerRequest *request, const String &filename,
                           size_t index, uint8_t *data, size_t len, bool final)>
    ArUploadHandlerFunction;
typedef std::function<void(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                           size_t index, size_t total)>
    ArBodyHandlerFunction;

class AsyncWebParameter {
 public:
  AsyncWebParameter() = default;
  AsyncWebParameter(const String &name, const String &value) : _name(name), _value(value) {}
  const String &name() const { return _name; }
  const String &value() const { return _value; }

 private:
  String _name;
  String _value;
};

class AsyncWebServerRequest;   // forward: the handler typedefs below need it

// Only the fields the firmware reads: a header is a parameter with a value and
// no name, exactly like the real library. A test builds one to act as the
// X-OTA-Token header of an incoming push.
class AsyncWebHeader {
 public:
  AsyncWebHeader() = default;
  AsyncWebHeader(const String &name, const String &value) : _name(name), _value(value) {}
  const String &name() const { return _name; }
  const String &value() const { return _value; }

 private:
  String _name;
  String _value;
};

class AsyncWebServerRequest {
 public:
  // ---- what the test puts in ----
  void addParam(const String &name, const String &value) {
    _params.push_back(AsyncWebParameter(name, value));
  }

  // The requesting client, as the real library exposes it: only remoteIP() is
  // read by the firmware (the push OTA endpoint checks where the request came
  // from). A stub address type keeps the same shape without pulling in IPAddress.
  class AsyncClientStub {
   public:
    // The real library returns an IPAddress, and the firmware turns it into a
    // string with IPAddress::toString(). Returning the same type here keeps the
    // call site on the device compiling unchanged - the stub must not be more
    // convenient than reality.
    IPAddress remoteIP() const { return _ip; }
    void setRemoteIP(const String &ip) { _ip = IPAddress(ip); }
    void setRemoteIP(const IPAddress &ip) { _ip = ip; }

   private:
    IPAddress _ip;
  };
  AsyncClientStub *client() { return &_client; }

  void addHeader(const String &name, const String &value) {
    _headers.push_back(AsyncWebHeader(name, value));
  }

  const AsyncWebHeader *getHeader(const String &name) const {
    for (const AsyncWebHeader &h : _headers)
      if (h.name() == name) return &h;
    return nullptr;
  }

  // Query parameters only - the stub has no POST form or file parameters,
  // and nothing under test reads those.
  bool hasParam(const char *name) const { return getParam(name) != nullptr; }
  const AsyncWebParameter *getParam(const char *name) const {
    for (const AsyncWebParameter &p : _params)
      if (std::strcmp(p.name().c_str(), name) == 0) return &p;
    return nullptr;
  }

  void send(int code, const String &contentType, const String &content) {
    _sent = true;
    _code = code;
    _contentType = contentType;
    _response = content;
  }

  // ESPAsyncWebServer calls this when the TCP connection is closed before the
  // body is complete. The firmware registers it so an interrupted OTA push can
  // abort Update and clear inProgress; the test calls disconnect() to exercise
  // the same path. The real handler type has no request argument.
  void onDisconnect(std::function<void()> fn) { _onDisconnect = fn; }
  void disconnect() { if (_onDisconnect) _onDisconnect(); }

  // Per-request attributes, mirroring the real library's setAttribute /
  // getAttribute. The push-OTA handler stores the refusal flag here so a
  // rejected request cannot poison a legitimate transfer that arrives in
  // parallel.
  void setAttribute(const char *name, bool value) { _attributes[std::string(name)] = value ? "1" : ""; }
  bool getAttribute(const char *name, bool defaultValue) const {
    auto it = _attributes.find(std::string(name));
    if (it == _attributes.end()) return defaultValue;
    return it->second == "1";
  }

  // ---- what the test reads back ----
  bool sent() const { return _sent; }
  int responseCode() const { return _code; }
  const String &responseBody() const { return _response; }
  const String &responseContentType() const { return _contentType; }

 private:
  std::vector<AsyncWebParameter> _params;
  std::vector<AsyncWebHeader> _headers;
  AsyncClientStub _client;
  bool _sent = false;
  int _code = 0;
  String _contentType;
  String _response;
  std::function<void()> _onDisconnect;
  std::map<std::string, std::string> _attributes;
};

// Every live server, so a test can find the routes web_server.cpp registered
// through its file-static `server` object.
inline std::vector<class AsyncWebServer *> &asyncWebServers() {
  static std::vector<AsyncWebServer *> servers;
  return servers;
}

class AsyncStaticWebHandler {
 public:
  AsyncStaticWebHandler &setDefaultFile(const char *) { return *this; }
};

class AsyncWebServer {
 public:
  struct Route {
    std::string uri;
    WebRequestMethodComposite method;
    ArRequestHandlerFunction onRequest;
    ArBodyHandlerFunction onBody;
  };

  explicit AsyncWebServer(uint16_t port) : _port(port) { asyncWebServers().push_back(this); }

  // One overload with defaulted upload/body handlers, like the real library:
  // that covers both server.on(path, method, handler) and the five-argument
  // body form web_server.cpp uses for its POST endpoints.
  AsyncWebServer &on(const char *uri, WebRequestMethodComposite method,
                     ArRequestHandlerFunction onRequest,
                     ArUploadHandlerFunction onUpload = nullptr,
                     ArBodyHandlerFunction onBody = nullptr) {
    (void)onUpload;  // no upload path in the stub - the firmware has no endpoint that uses one
    Route r;
    r.uri = uri;
    r.method = method;
    r.onRequest = onRequest;
    r.onBody = onBody;
    _routes.push_back(r);
    return *this;
  }

  // Templated on the filesystem so this header does not have to know
  // LittleFS; web_server.cpp calls it with the stub's LittleFSClass.
  template <typename FS>
  AsyncStaticWebHandler &serveStatic(const char *uri, FS &, const char *path,
                                     const char *cacheControl = nullptr) {
    (void)uri;
    (void)path;
    (void)cacheControl;
    return _static;
  }

  void begin() { _begun = true; }
  bool begun() const { return _begun; }
  const std::vector<Route> &routes() const { return _routes; }

 private:
  uint16_t _port;
  bool _begun = false;
  std::vector<Route> _routes;
  AsyncStaticWebHandler _static;
};
