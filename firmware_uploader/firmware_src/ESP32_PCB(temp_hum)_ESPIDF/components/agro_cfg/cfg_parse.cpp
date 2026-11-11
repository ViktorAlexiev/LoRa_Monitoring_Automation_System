#include "cfg_parse.h"
#include <stdint.h>
#include <inttypes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_pins.h"
#include "crypto_common.h"
#include "json_util.h"
#include "nvs_store.h"
#include "radio_timing.h"

static void nack(CfgReply *r, const char *why) {
  r->ack = false;
  snprintf(r->reason, sizeof(r->reason), "%s", why);
}

bool cfg_is_config_line(const char *line) {
  return strncmp(line, "CFG:", 4) == 0;
}

static bool parseUInt(const char *s, uint64_t maxV, uint64_t *out) {
  if (*s == 0) return false;
  uint64_t v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return false;
    v = v * 10 + (uint32_t)(*s - '0');
    if (v > maxV) return false;
  }
  *out = v;
  return true;
}

// Цяло число със знак ("-9", "22"): само цифри и водещ минус, в [lo, hi]
static bool parseSInt(const char *s, int32_t lo, int32_t hi, int32_t *out) {
  bool neg = false;
  if (*s == '-') { neg = true; s++; }
  if (*s == 0 || strlen(s) > 3) return false;
  int32_t v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return false;
    v = v * 10 + (*s - '0');
  }
  if (neg) v = -v;
  if (v < lo || v > hi) return false;
  *out = v;
  return true;
}

static int32_t hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool hexToBytes(const char *hex, uint8_t *out, size_t outLen) {
  if (strlen(hex) != outLen * 2) return false;
  for (size_t i = 0; i < outLen; i++) {
    int32_t hi = hexVal(hex[i * 2]);
    int32_t lo = hexVal(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// Низово поле: F_OK и копие в out, ако е присъствало като JSON низ. Стойност, по-дълга от cap - 1, е F_TOO_LONG
// (грешка, не се реже тихо); липсващо или не-низово поле е F_MISSING.
typedef JsonField FieldRes;
static const FieldRes F_MISSING = JSON_FIELD_MISSING, F_OK = JSON_FIELD_OK, F_TOO_LONG = JSON_FIELD_TOO_LONG;
static FieldRes field(const cJSON *obj, const char *key, char *out, size_t cap) { return jsonField(obj, key, out, cap); }

void cfg_apply_line(const char *line, CfgReply *reply) {
  reply->ack = true;
  reply->reason[0] = 0;
  if (!cfg_is_config_line(line)) { nack(reply, "не е CFG пакет"); return; }
  const char *json = line + 4;
  size_t n = strlen(json);
  // ЦЕЛИЯТ ред трябва да е ТОЧНО ЕДИН валиден JSON обект: отрязан/повреден по серийния порт ред не се приема частично
  JsonDoc doc(json, n);
  if (!doc.valid()) { nack(reply, "невалиден JSON (непълен или повреден ред)"); return; }
  const cJSON *root = doc.root();

  char moduleId[16];
  FieldRes fr = field(root, "id", moduleId, sizeof(moduleId));
  if (fr == F_MISSING) { nack(reply, "Липсва 'id' поле"); return; }
  if (fr == F_TOO_LONG || moduleId[0] == 0 || strlen(moduleId) > CFG_MODULE_ID_LEN) {
    nack(reply, "Невалидна дължина на module id"); return;
  }

  char freqStr[16], freqTxStr[16], sfStr[8], bwStr[12], keyStr[40], net[4];
  bool hasFreq = field(root, "freq", freqStr, sizeof(freqStr)) == F_OK;
  bool hasFreqTx = field(root, "freq_tx", freqTxStr, sizeof(freqTxStr)) == F_OK;
  bool hasSf = field(root, "sf", sfStr, sizeof(sfStr)) == F_OK;
  bool hasBw = field(root, "bw", bwStr, sizeof(bwStr)) == F_OK;
  bool hasKey = field(root, "key", keyStr, sizeof(keyStr)) == F_OK;
  char txpStr[8];
  bool hasTxp = field(root, "txp", txpStr, sizeof(txpStr)) == F_OK;
  field(root, "net", net, sizeof(net));
  // Опция "има батерия" (само ESP32 PCB): "1" = устройството измерва батерията и я праща; "0" или липсва = не.
  char batStr[4];
  bool hasBat = field(root, "bat", batStr, sizeof(batStr)) == F_OK;
  if (hasBat && strcmp(batStr, "0") != 0 && strcmp(batStr, "1") != 0) { nack(reply, "невалидно bat (0 или 1)"); return; }

  uint64_t freq = 0, freqTx = 0, sf = 0, bw = 0;
  uint8_t keyBytes[CRYPTO_KEY_LEN];
  if (hasFreq && (!parseUInt(freqStr, UINT64_C(0xFFFFFFFF), &freq) || freq == 0)) { nack(reply, "невалидна честота (freq)"); return; }
  if (hasFreqTx && (!parseUInt(freqTxStr, UINT64_C(0xFFFFFFFF), &freqTx) || freqTx == 0)) { nack(reply, "невалидна честота (freq_tx)"); return; }
  if (hasKey && !hexToBytes(keyStr, keyBytes, sizeof(keyBytes))) { nack(reply, "невалиден key (трябват 32 hex символа)"); return; }
  if (hasSf && (!parseUInt(sfStr, 99, &sf) || sf < RADIO_SF_MIN || sf > RADIO_SF_MAX)) { nack(reply, "невалиден sf (трябва 7..12)"); return; }
  if (hasBw && (!parseUInt(bwStr, UINT64_C(0xFFFFFFFF), &bw) || radioBwToIndex((uint32_t)bw) == 0xFF)) {
    nack(reply, "невалиден bw (трябва 62500, 125000 или 250000 Hz)"); return;
  }

  int32_t txp = 0;
  if (hasTxp && !parseSInt(txpStr, LORA_TX_POWER_MIN_DBM, LORA_TX_POWER_LIMIT_DBM, &txp)) {
    char why[CFG_REASON_LEN];
    snprintf(why, sizeof(why), "невалидна мощност txp (допустимо %" PRId32 "..%" PRId32 " dBm)", (int32_t)LORA_TX_POWER_MIN_DBM, (int32_t)LORA_TX_POWER_LIMIT_DBM);
    nack(reply, why);
    return;
  }

  // ---- Консуматори ----
  char cIds[CFG_MAX_CONSUMERS][CFG_CONSUMER_ID_LEN + 1];
  uint8_t cPins[CFG_MAX_CONSUMERS];
  int32_t numConsumers = 0;
  const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "consumers");
  bool hasConsumers = arr != nullptr;
  if (hasConsumers) {
    if (!cJSON_IsArray(arr)) { nack(reply, "Невалиден списък консуматори"); return; }
    const cJSON *el = nullptr;
    cJSON_ArrayForEach(el, arr) {
      if (numConsumers >= CFG_MAX_CONSUMERS) { nack(reply, "Повече от 10 консуматора"); return; }
      char cid[12], cpin[8];
      if (field(el, "id", cid, sizeof(cid)) != F_OK || field(el, "pin", cpin, sizeof(cpin)) != F_OK) {
        nack(reply, "Невалиден консуматор запис"); return;
      }
      if (cid[0] == 0 || strlen(cid) > CFG_CONSUMER_ID_LEN) { nack(reply, "Невалидна дължина на consumer id"); return; }
      uint64_t pin = 0;
      if (cpin[0] == 0 || strlen(cpin) > 2 || !parseUInt(cpin, 99, &pin)) { nack(reply, "Невалиден pin (GPIO номер)"); return; }
      if (!board_gpio_usable((int32_t)pin)) { nack(reply, "GPIO номерът не е използваем на тази платка"); return; }
      for (int32_t k = 0; k < numConsumers; k++) {
        if (strcmp(cIds[k], cid) == 0) { nack(reply, "Повтарящ се consumer id"); return; }
      }
      strcpy(cIds[numConsumers], cid);
      cPins[numConsumers] = (uint8_t)pin;
      numConsumers++;
    }
  }

  // ---- Wi-Fi / MQTT ----
  char wifiSsid[CFG_WIFI_SSID_LEN + 2], wifiPass[CFG_WIFI_PASSWORD_LEN + 2], mqttIp[CFG_MQTT_IP_LEN + 2];
  char mqttPort[8], mqttUser[CFG_MQTT_USER_LEN + 2], mqttPass[CFG_MQTT_PASSWORD_LEN + 2];
  FieldRes rSsid = field(root, "wifi_ssid", wifiSsid, sizeof(wifiSsid));
  FieldRes rPass = field(root, "wifi_password", wifiPass, sizeof(wifiPass));
  FieldRes rIp = field(root, "mqtt_ip", mqttIp, sizeof(mqttIp));
  FieldRes rPort = field(root, "mqtt_port", mqttPort, sizeof(mqttPort));
  FieldRes rUser = field(root, "mqtt_user", mqttUser, sizeof(mqttUser));
  FieldRes rMpass = field(root, "mqtt_password", mqttPass, sizeof(mqttPass));
  bool needNet = strcmp(net, "1") == 0;
  if (needNet) {
    if (rSsid != F_OK || wifiSsid[0] == 0) { nack(reply, "Липсва или е празно wifi_ssid"); return; }
    if (rIp != F_OK || mqttIp[0] == 0) { nack(reply, "Липсва или е празно mqtt_ip"); return; }
  }
  if (rSsid == F_TOO_LONG || strlen(wifiSsid) > CFG_WIFI_SSID_LEN) { nack(reply, "wifi_ssid твърде дълго"); return; }
  if (rPass == F_TOO_LONG || strlen(wifiPass) > CFG_WIFI_PASSWORD_LEN) { nack(reply, "wifi_password твърде дълго"); return; }
  if (rIp == F_TOO_LONG || strlen(mqttIp) > CFG_MQTT_IP_LEN) { nack(reply, "mqtt_ip твърде дълго"); return; }
  if (rUser == F_TOO_LONG || strlen(mqttUser) > CFG_MQTT_USER_LEN) { nack(reply, "mqtt_user твърде дълго"); return; }
  if (rMpass == F_TOO_LONG || strlen(mqttPass) > CFG_MQTT_PASSWORD_LEN) { nack(reply, "mqtt_password твърде дълго"); return; }
  uint64_t port = 1883;
  if (rPort == F_OK && mqttPort[0] != 0 && (!parseUInt(mqttPort, 65535, &port) || port == 0)) { nack(reply, "невалиден mqtt_port"); return; }
  if (rPort == F_TOO_LONG) { nack(reply, "невалиден mqtt_port"); return; }
  snprintf(mqttPort, sizeof(mqttPort), "%" PRIu32, (uint32_t)port);

  // ---- Запис в NVS (след като всичко е валидно) ----
  NvsStore s;
  if (!s.open(NVS_NAMESPACE, false)) { nack(reply, "NVS не може да се отвори"); return; }
  bool ok = s.setString("id", moduleId);
  if (hasFreq) ok &= s.setU32("freq", (uint32_t)freq);
  if (hasFreqTx) ok &= s.setU32("freqtx", (uint32_t)freqTx);
  if (hasSf) ok &= s.setU8("sf", (uint8_t)sf);
  if (hasBw) ok &= s.setU32("bw", (uint32_t)bw);
  if (hasTxp) ok &= s.setU8("txp", (uint8_t)(int8_t)txp);          // със знак (два комплемента)
  if (hasKey) ok &= s.setBlob("key", keyBytes, sizeof(keyBytes));
  if (hasBat) ok &= s.setU8("bat", batStr[0] == '1' ? 1 : 0);
  if (hasConsumers) {
    ok &= s.setU8("nc", (uint8_t)numConsumers);
    for (int32_t i = 0; i < numConsumers; i++) {
      char kId[8], kPin[8];
      snprintf(kId, sizeof(kId), "c%" PRIu32 "i", (uint32_t)i);
      snprintf(kPin, sizeof(kPin), "c%" PRIu32 "p", (uint32_t)i);
      ok &= s.setString(kId, cIds[i]);
      ok &= s.setU8(kPin, cPins[i]);
    }
  }
  if (needNet) {
    ok &= s.setString("wssid", wifiSsid);
    ok &= s.setString("wpass", wifiPass);
    ok &= s.setString("mqip", mqttIp);
    ok &= s.setString("mqport", mqttPort);
    ok &= s.setString("mquser", mqttUser);
    ok &= s.setString("mqpass", mqttPass);
  }
  ok &= s.commit();
  if (!ok) { nack(reply, "запис в NVS се провали"); return; }
}

// ======================================================================= CfgLineBuffer
bool CfgLineBuffer::feed(int32_t c) {
  if (c == '\r') return false;
  if (c == '\n') {
    bool had = !overflow_ && len_ > 0;
    if (overflow_) reportOverflow_ = true;
    buf_[len_] = 0;
    size_t l = len_;
    len_ = 0;
    overflow_ = false;
    if (!had) return false;
    // изрязване на крайни интервали (както line.trim())
    while (l > 0 && (buf_[l - 1] == ' ' || buf_[l - 1] == '\t')) buf_[--l] = 0;
    return l > 0;
  }
  if (len_ == 0 && (c == ' ' || c == '\t')) return false;   // водещи интервали се пропускат
  if (overflow_) return false;
  if (len_ >= CFG_LINE_MAX) { overflow_ = true; return false; }
  buf_[len_++] = (char)c;
  return false;
}

bool CfgLineBuffer::overflowed() {
  bool r = reportOverflow_;
  reportOverflow_ = false;
  return r;
}
