#include "nvs_store.h"

#include <string.h>

bool NvsStore::open(const char *ns, bool readOnly) {
  close();
  esp_err_t e = nvs_open(ns, readOnly ? NVS_READONLY : NVS_READWRITE, &handle_);
  opened_ = (e == ESP_OK);
  return opened_;
}

void NvsStore::close() {
  if (opened_) nvs_close(handle_);
  opened_ = false;
}

bool NvsStore::getString(const char *key, char *out, size_t cap) {
  if (cap == 0) return false;
  out[0] = 0;
  if (!opened_) return false;
  size_t len = cap;
  if (nvs_get_str(handle_, key, out, &len) != ESP_OK) { out[0] = 0; return false; }
  return true;
}

uint8_t NvsStore::getU8(const char *key, uint8_t def) {
  uint8_t v = def;
  if (!opened_ || nvs_get_u8(handle_, key, &v) != ESP_OK) return def;
  return v;
}

uint32_t NvsStore::getU32(const char *key, uint32_t def) {
  uint32_t v = def;
  if (!opened_ || nvs_get_u32(handle_, key, &v) != ESP_OK) return def;
  return v;
}

size_t NvsStore::getBlob(const char *key, void *buf, size_t cap) {
  if (!opened_) return 0;
  size_t len = cap;
  if (nvs_get_blob(handle_, key, buf, &len) != ESP_OK) return 0;
  return len;
}

bool NvsStore::setString(const char *key, const char *value) {
  return opened_ && nvs_set_str(handle_, key, value) == ESP_OK;
}
bool NvsStore::setU8(const char *key, uint8_t value) {
  return opened_ && nvs_set_u8(handle_, key, value) == ESP_OK;
}
bool NvsStore::setU32(const char *key, uint32_t value) {
  return opened_ && nvs_set_u32(handle_, key, value) == ESP_OK;
}
bool NvsStore::setBlob(const char *key, const void *data, size_t len) {
  return opened_ && nvs_set_blob(handle_, key, data, len) == ESP_OK;
}
bool NvsStore::commit() {
  return opened_ && nvs_commit(handle_) == ESP_OK;
}
