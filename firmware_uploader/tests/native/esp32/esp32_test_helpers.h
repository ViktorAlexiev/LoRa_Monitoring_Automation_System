// Помощни функции за ESP32 тестовете: пълнене/четене на фалшивия NVS и сброс на всички заместители.
#ifndef ESP32_TEST_HELPERS_H
#define ESP32_TEST_HELPERS_H

#include "Arduino.h"
#include "Preferences.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "WiFi.h"
#include "PubSubClient.h"
#include "LoRa.h"
#include <string>
#include <vector>

// ---------- NVS ----------
inline void nvsClear() {
  g_nvs.clear();
  g_nvsWrites = 0;
  g_nvsReadOnlyWriteAttempts = 0;
}
inline void nvsSetStr(const char *key, const std::string &v) {
  Preferences p; p.begin("cfg", false); p.putString(key, String(v)); p.end();
}
inline void nvsSetU8(const char *key, uint8_t v) {
  Preferences p; p.begin("cfg", false); p.putUChar(key, v); p.end();
}
inline void nvsSetU32(const char *key, uint32_t v) {
  Preferences p; p.begin("cfg", false); p.putULong(key, v); p.end();
}
inline void nvsSetBytes(const char *key, const uint8_t *b, size_t n) {
  Preferences p; p.begin("cfg", false); p.putBytes(key, b, n); p.end();
}
inline bool nvsHas(const char *key) { return g_nvs.count(std::string("cfg/") + key) > 0; }
inline std::string nvsGetStr(const char *key) {
  auto it = g_nvs.find(std::string("cfg/") + key);
  return it == g_nvs.end() ? std::string("<нищо>") : std::string(it->second.begin(), it->second.end());
}
inline uint64_t nvsGetNum(const char *key) {
  auto it = g_nvs.find(std::string("cfg/") + key);
  if (it == g_nvs.end() || it->second.size() != 8) return (uint64_t)-1;
  uint64_t v; memcpy(&v, it->second.data(), 8); return v;
}
inline std::vector<uint8_t> nvsGetBytes(const char *key) {
  auto it = g_nvs.find(std::string("cfg/") + key);
  return it == g_nvs.end() ? std::vector<uint8_t>() : it->second;
}

// ---------- мрежов ключ за тестовете ----------
static const uint8_t TEST_KEY[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                     0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

// ---------- сброс на всички заместители ----------
inline void resetFakes() {
  nvsClear();
  g_serialOut.clear();
  g_serialIn.clear();
  g_fakeMillis = 0;
  g_fakeTimeUs = 1000000ULL;
  memset(g_pinState, 0, sizeof(g_pinState));
  memset(g_pinMode, 0, sizeof(g_pinMode));
  g_espRestarted = false;
  g_resetReason = ESP_RST_POWERON;
  g_timerWakeUs = 0;
  g_gpioWakeAfterUs = 0;
  g_gpioWakeEnabled = false;
  g_lightSleepCount = 0;
  g_lightSleepGpioWakes = 0;
  g_deepSleepRequested = false;
  g_deepSleepUs = 0;
  g_wakeGpio = -1;
  g_wakeGpioLevel = 0;
  g_wifiStatus = WL_CONNECTED;
  g_wifiBeginCalls = 0;
  g_mqtt.reset();
  g_lora.reset();
}

inline bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

#endif
