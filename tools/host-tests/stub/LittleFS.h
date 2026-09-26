#pragma once
// Host stub for LittleFS: main.cpp only mounts the filesystem and makes the
// profile directory, so nothing here needs to store anything.
class LittleFSClass {
 public:
  bool begin(bool format = false) { (void)format; return true; }
  bool exists(const char *) { return true; }
  bool mkdir(const char *) { return true; }
};
extern LittleFSClass LittleFS;
