#pragma once
// Host stub for LittleFS: a small IN-MEMORY filesystem that also LOGS every
// operation it is asked to perform.
//
// main.cpp only mounts it and makes the profile directory, but the
// web-server test needs more than that: real files under /profiles, a
// directory listing for GET /api/profiles, paths that resolve ".." the way a
// real filesystem does - and above all the log, so a test can assert that a
// REJECTED request never touched the filesystem at all, instead of merely
// that it answered 400.
//
// Paths are normalised on every lookup ("/profiles/../config.json" is
// "/config.json"). That is what makes a traversal test honest: the honeytoken
// a bad name reaches for really is a different file, so without the name
// check the read and the delete would find it - and be counted here.
#include <Arduino.h>
#include <map>
#include <string>
#include <vector>

namespace littlefs_stub {

// Resolved like a real filesystem: "." drops out, ".." pops the previous
// segment (and never climbs above the root).
inline std::string normalise(const std::string &raw) {
  std::vector<std::string> segs;
  size_t i = 0;
  while (i <= raw.size()) {
    size_t j = raw.find('/', i);
    if (j == std::string::npos) j = raw.size();
    std::string seg = raw.substr(i, j - i);
    if (seg == "..") {
      if (!segs.empty()) segs.pop_back();
    } else if (!seg.empty() && seg != ".") {
      segs.push_back(seg);
    }
    i = j + 1;
  }
  std::string out;
  for (const std::string &s : segs) out += "/" + s;
  return out.empty() ? "/" : out;
}

// Function-local statics inside inline functions: one copy per test binary,
// shared by every translation unit that links against this header.
inline std::map<std::string, std::string> &files() {
  static std::map<std::string, std::string> f;
  return f;
}
inline std::vector<std::string> &dirs() {
  static std::vector<std::string> d;
  return d;
}
// One entry per operation, as "<op>:<path as handed to the stub>". A request
// that must not reach the filesystem may not add a single one.
inline std::vector<std::string> &opLog() {
  static std::vector<std::string> log;
  return log;
}
inline void note(const char *op, const std::string &rawPath) {
  opLog().push_back(std::string(op) + ":" + rawPath);
}
inline size_t opCount() { return opLog().size(); }

}  // namespace littlefs_stub

// A file handle as the firmware sees it: read (buffer + position), write
// (buffer, committed to the store on close) or a directory listing.
// read()/write() are the two entry points ArduinoJson's generic Reader and
// Writer drive, which is what deserializeJson(doc, file) and
// serializeJson(doc, file) go through in src/roast_profile.cpp.
class File {
 public:
  File() = default;

  explicit operator bool() const { return _kind != Kind::None; }
  bool operator!() const { return _kind == Kind::None; }

  // What ESPAsyncWebServer's file listing hands out: the full path, which
  // handleProfilesList strips the directory and the .json suffix from.
  const char *name() const { return _path.c_str(); }

  int read() {
    if (_pos >= _data.size()) return -1;
    return (unsigned char)_data[_pos++];
  }
  size_t readBytes(char *buffer, size_t length) {
    size_t n = 0;
    while (n < length && _pos < _data.size()) buffer[n++] = _data[_pos++];
    return n;
  }
  size_t write(uint8_t c) {
    _data += (char)c;
    return 1;
  }
  size_t write(const uint8_t *buf, size_t n) {
    _data.append(reinterpret_cast<const char *>(buf), n);
    return n;
  }

  void close() {
    if (_kind == Kind::Write) {
      littlefs_stub::note("write", _path);
      littlefs_stub::files()[littlefs_stub::normalise(_path)] = _data;
    }
    _kind = Kind::None;
  }

  File openNextFile() {
    if (_kind != Kind::Dir || _i >= _paths.size()) return File();
    const std::string &p = _paths[_i++];
    littlefs_stub::note("open", p);
    File f;
    f._kind = File::Kind::Read;
    f._path = p;
    f._data = littlefs_stub::files()[p];
    return f;
  }

 private:
  friend class LittleFSClass;   // open() fills the fields above
  enum class Kind { None, Read, Write, Dir };
  Kind _kind = Kind::None;
  std::string _path;
  std::string _data;
  size_t _pos = 0;
  std::vector<std::string> _paths;  // directory: the entries under it
  size_t _i = 0;
};

class LittleFSClass {
 public:
  bool begin(bool format = false) { (void)format; return true; }

  bool exists(const String &path) {
    littlefs_stub::note("exists", path.c_str());
    const std::string key = littlefs_stub::normalise(path.c_str());
    if (littlefs_stub::files().count(key)) return true;
    return isDirKey(key);
  }

  bool mkdir(const String &path) {
    littlefs_stub::note("mkdir", path.c_str());
    littlefs_stub::dirs().push_back(littlefs_stub::normalise(path.c_str()));
    return true;
  }

  bool remove(const String &path) {
    littlefs_stub::note("remove", path.c_str());
    return littlefs_stub::files().erase(littlefs_stub::normalise(path.c_str())) > 0;
  }

  // Declared out of line: the return type File must be complete here.
  File open(const String &path, const char *mode = "r");

 private:
  static bool isDirKey(const std::string &key) {
    for (const std::string &d : littlefs_stub::dirs())
      if (d == key) return true;
    return false;
  }
};

inline File LittleFSClass::open(const String &path, const char *mode) {
  littlefs_stub::note("open", path.c_str());
  const std::string key = littlefs_stub::normalise(path.c_str());
  const bool writing = mode && mode[0] == 'w';

  File f;
  auto it = littlefs_stub::files().find(key);
  if (writing) {
    f._kind = File::Kind::Write;
    f._path = key;
    return f;
  }
  if (it != littlefs_stub::files().end()) {
    f._kind = File::Kind::Read;
    f._path = key;
    f._data = it->second;
    return f;
  }

  // Not a file: a directory is anything that holds entries, or that mkdir
  // created. A path with neither is missing, and hands back an invalid
  // handle the way the real filesystem does.
  const std::string prefix = key == "/" ? "/" : key + "/";
  std::vector<std::string> entries;
  for (const auto &kv : littlefs_stub::files())
    if (kv.first.compare(0, prefix.size(), prefix) == 0) entries.push_back(kv.first);
  if (entries.empty() && !isDirKey(key)) return File();

  f._kind = File::Kind::Dir;
  f._path = key;
  f._paths = entries;
  return f;
}

extern LittleFSClass LittleFS;
