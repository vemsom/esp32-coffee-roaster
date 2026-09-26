#pragma once
// Host-side stand-in for the ESP32 Preferences (NVS) API, so the persisted
// safety latch can be exercised without hardware. Only the calls that
// src/safety.cpp actually uses are implemented, and they behave like the real
// ones: a key missing from the store returns the caller's default, and a
// failed begin() makes every read/write a no-op.
//
// The store is deliberately global and keyed "namespace/key": it survives
// safety_init(), which is how a reboot is simulated in the tests.
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

inline std::map<std::string, int32_t> &preferencesStore() {
  static std::map<std::string, int32_t> store;
  return store;
}

// Test knob: simulate an ESP32 whose NVS will not open at all.
inline bool &preferencesFailBegin() {
  static bool fail = false;
  return fail;
}

class Preferences {
 public:
  bool begin(const char *name, bool readOnly = false, const char *partition_label = nullptr) {
    (void)readOnly;
    (void)partition_label;
    if (preferencesFailBegin()) {
      _open = false;
      return false;
    }
    _ns = name ? name : "";
    _open = true;
    return true;
  }

  void end() { _open = false; }
  bool isOpen() const { return _open; }

  size_t putBool(const char *key, bool value) { return putInt(key, value ? 1 : 0); }
  bool getBool(const char *key, bool defaultValue = false) const {
    const int32_t *v = find(key);
    return v ? (*v != 0) : defaultValue;
  }

  size_t putInt(const char *key, int32_t value) {
    if (!_open) return 0;
    preferencesStore()[_ns + "/" + key] = value;
    return sizeof(int32_t);
  }
  int32_t getInt(const char *key, int32_t defaultValue = 0) const {
    const int32_t *v = find(key);
    return v ? *v : defaultValue;
  }

  bool isKey(const char *key) const { return find(key) != nullptr; }

  bool remove(const char *key) {
    if (!_open) return false;
    return preferencesStore().erase(_ns + "/" + key) > 0;
  }

  bool clear() {
    if (!_open) return false;
    const std::string prefix = _ns + "/";
    for (auto it = preferencesStore().begin(); it != preferencesStore().end();) {
      it = (it->first.compare(0, prefix.size(), prefix) == 0)
               ? preferencesStore().erase(it)
               : std::next(it);
    }
    return true;
  }

 private:
  const int32_t *find(const char *key) const {
    if (!_open || !key) return nullptr;
    auto &store = preferencesStore();
    auto it = store.find(_ns + "/" + key);
    return it == store.end() ? nullptr : &it->second;
  }

  std::string _ns;
  bool _open = false;
};
