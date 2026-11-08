// Заместител на ESP32 Preferences.h (NVS) - в паметта. Тестът може да зареди/провери съдържанието
// директно (g_nvs) и брои записите (g_nvsWrites) - важно за износването на flash паметта.
#ifndef FAKE_PREFERENCES_H
#define FAKE_PREFERENCES_H

#include "Arduino.h"
#include <map>
#include <vector>

extern std::map<std::string, std::vector<uint8_t>> g_nvs;   // ключ "namespace/key"
extern int g_nvsWrites;                  // брой put* повиквания
extern int g_nvsReadOnlyWriteAttempts;   // опити за запис при begin(ns, true) - би бил провал на реалния чип

class Preferences {
public:
  bool begin(const char *name, bool readOnly = false) { ns_ = name; ro_ = readOnly; return true; }
  void end() {}

  String getString(const char *key, String def = String()) {
    auto it = g_nvs.find(k(key));
    if (it == g_nvs.end()) return def;
    return String(std::string(it->second.begin(), it->second.end()));
  }
  size_t putString(const char *key, String v) {
    if (!w()) return 0;
    g_nvs[k(key)] = std::vector<uint8_t>(v.c_str(), v.c_str() + v.length());
    return v.length();
  }
  uint8_t getUChar(const char *key, uint8_t def = 0) { return (uint8_t)getNum(key, def); }
  size_t putUChar(const char *key, uint8_t v) { return putNum(key, v); }
  uint32_t getULong(const char *key, uint32_t def = 0) { return (uint32_t)getNum(key, def); }
  size_t putULong(const char *key, uint32_t v) { return putNum(key, v); }
  size_t getBytes(const char *key, void *buf, size_t maxLen) {
    auto it = g_nvs.find(k(key));
    if (it == g_nvs.end()) return 0;
    size_t n = it->second.size() < maxLen ? it->second.size() : maxLen;
    memcpy(buf, it->second.data(), n);
    return n;
  }
  size_t putBytes(const char *key, const void *buf, size_t len) {
    if (!w()) return 0;
    g_nvs[k(key)] = std::vector<uint8_t>((const uint8_t *)buf, (const uint8_t *)buf + len);
    return len;
  }

private:
  std::string ns_;
  bool ro_ = false;
  std::string k(const char *key) const { return ns_ + "/" + key; }
  bool w() {
    if (ro_) { g_nvsReadOnlyWriteAttempts++; return false; }
    g_nvsWrites++;
    return true;
  }
  uint64_t getNum(const char *key, uint64_t def) {
    auto it = g_nvs.find(k(key));
    if (it == g_nvs.end() || it->second.size() != 8) return def;
    uint64_t v; memcpy(&v, it->second.data(), 8); return v;
  }
  size_t putNum(const char *key, uint64_t v) {
    if (!w()) return 0;
    std::vector<uint8_t> b(8); memcpy(b.data(), &v, 8);
    g_nvs[k(key)] = b;
    return 4;
  }
};

#endif
