/*
  consumers.h - консуматорите (релета/вентили), управлявани от executor и Wi-Fi executor.

  Състоянието на консуматора се чете от РЕАЛНИЯ пин (GPIO INPUT_OUTPUT), не от запомнена стойност.
  Пин, който не е използваем на платката (flash/PSRAM/USB ... - виж board_gpio_usable), не се пипа никога:
  командите към такъв консуматор се отхвърлят с NACK.
*/
#ifndef AGRO_CONSUMERS_H
#define AGRO_CONSUMERS_H

#include <stdint.h>
#include <stdbool.h>
#include "nvs_store.h"
#include "protocol.h"

struct ConsumerSet {
  uint8_t count;
  char id[MAX_CONSUMERS][CONSUMER_ID_LEN + 1];
  uint8_t pin[MAX_CONSUMERS];
  bool usable[MAX_CONSUMERS];
};

// Чете от (вече отворения) NVS: nc, c<i>i (ID), c<i>p (GPIO). nc > MAX_CONSUMERS -> 0 консуматора.
void consumers_load(ConsumerSet *cs, NvsStore *nvs);

// Конфигурира всички използваеми пинове като изходи, ИЗКЛЮЧЕНИ (съзнателно безопасно състояние при (ре)старт).
void consumers_init(ConsumerSet *cs);

// Изключва всички консуматори (пиновете LOW).
void consumers_all_off(const ConsumerSet *cs);
// Изключва ги и заключва пиновете в LOW за дълбокия сън (иначе в дълбок сън GPIO се връщат в състояние по подразбиране и не
// са гарантирано LOW). consumers_init() сваля заключването след събуждането.
void consumers_hold_for_deep_sleep(const ConsumerSet *cs);

int32_t consumers_index(const ConsumerSet *cs, const char *c_id);      // -1 ако няма такъв (сравнява 4 символа)
bool consumers_usable(const ConsumerSet *cs, int32_t idx);
void consumers_set(const ConsumerSet *cs, int32_t idx, bool on);
bool consumers_state(const ConsumerSet *cs, int32_t idx);

#endif
