/*
  config (ESP-IDF) - Stage-1 firmware на двуетапното качване: ЕДНО за всички устройства (sensor, executor,
  repeater, gateway, executor_wifi). Качва се ПЪРВО, преди реалния firmware. Слуша конзолата (серийния порт /
  USB) за ред от Python програмата (Firmware Uploader):

      CFG:{"id":"CS001","freq":"433000000", ... }          (формат и полета: виж cfg_parse.h)

  и записва в NVS със същите ключове и типове като Arduino config_esp32 (двете версии са взаимозаменяеми).
  Протокол към Python: "READY" на всеки 500 ms, докато чака; после "ACK" или "NACK:<причина>".
  Реалните приложения само ЧЕТАТ NVS при boot (не съдържат парсер/конфигурационна логика).
*/
#ifndef CONFIG_APP_H
#define CONFIG_APP_H

#include <stdint.h>

#define CONFIG_TIMEOUT_MS   UINT32_C(60000)
#define CONFIG_READY_MS     UINT32_C(500)

struct ConfigIo {
  int32_t (*getChar)(void);              // -1 (EOF) ако няма символ в момента
  void (*print)(const char *line);   // изпраща ред (с нов ред накрая)
};

enum ConfigResult { CONFIG_ACKED = 1, CONFIG_TIMED_OUT = 2 };

// Блокира (отстъпва процесора), докато получи валиден конфигурационен ред (ACK) или изтече прозорецът.
// NACK на невалиден ред НЕ приключва: потребителят може да прати поправен ред в същия прозорец.
ConfigResult config_run(const ConfigIo *io, uint32_t timeoutMs, uint32_t readyIntervalMs);

#endif
