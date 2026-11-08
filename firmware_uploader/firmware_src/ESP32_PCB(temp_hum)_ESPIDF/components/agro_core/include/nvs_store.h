/*
  nvs_store.h - обвивка над ESP-IDF NVS (nvs_open, nvs_get_xxx, nvs_set_xxx), namespace "cfg".

  Типовете и ключовете са СЪВМЕСТИМИ със старата Arduino версия (Preferences пише със същите
  nvs_set_* извиквания), така че конфигурация, записана от Arduino config_esp32 firmware-а,
  се чете от ESP-IDF прошивките и обратно:
    putString -> nvs_set_str,  putUChar -> nvs_set_u8,  putULong -> nvs_set_u32,  putBytes -> blob.
*/
#ifndef NVS_STORE_H
#define NVS_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "nvs.h"

#define NVS_NAMESPACE "cfg"

class NvsStore {
 public:
  NvsStore() : handle_(0), opened_(false) {}
  ~NvsStore() { close(); }

  bool open(const char *ns, bool readOnly);
  void close();

  // низ: връща false (и празен низ), ако ключът липсва или низът не се побира в cap
  bool getString(const char *key, char *out, size_t cap);
  uint8_t getU8(const char *key, uint8_t def);
  uint32_t getU32(const char *key, uint32_t def);
  // връща дължината на blob-а; 0 ако липсва или не се побира в cap
  size_t getBlob(const char *key, void *buf, size_t cap);

  bool setString(const char *key, const char *value);
  bool setU8(const char *key, uint8_t value);
  bool setU32(const char *key, uint32_t value);
  bool setBlob(const char *key, const void *data, size_t len);
  bool commit();

 private:
  nvs_handle_t handle_;
  bool opened_;
};

#endif
