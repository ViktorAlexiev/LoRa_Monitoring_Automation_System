// Общи помощници за драйверите за съвместимост (ESP-IDF код <-> стар Arduino код; Python е арбитър).
#ifndef COMPAT_COMMON_H
#define COMPAT_COMMON_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sim.h"
#include "sim_net.h"
#include "sim_periph.h"
#include "agro_radio.h"

extern "C" void app_main(void);

static inline std::string hexs(const std::vector<uint8_t> &v) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
  return s;
}
static inline std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

// каноничен ключ на теста за NVS
static inline void nvsCommon(const char *id, const std::string &keyHex, unsigned long freq, int sf, unsigned long bw) {
  sim::nvs.setStr("id", id);
  std::vector<uint8_t> k = unhex(keyHex);
  sim::nvs.setBlob("key", k.data(), k.size());
  sim::nvs.setU32("freq", (uint32_t)freq);
  sim::nvs.setU8("sf", (uint8_t)sf);
  sim::nvs.setU32("bw", (uint32_t)bw);
}

#endif
