#include "consumers.h"
#include <inttypes.h>

#include <stdio.h>
#include <string.h>
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "agro_hal.h"

static const char *TAG = "consumers";

void consumers_load(ConsumerSet *cs, NvsStore *nvs) {
  memset(cs, 0, sizeof(*cs));
  uint8_t nc = nvs->getU8("nc", 0);
  cs->count = nc > MAX_CONSUMERS ? 0 : nc;
  for (uint8_t i = 0; i < cs->count; i++) {
    char kId[8], kPin[8];
    snprintf(kId, sizeof(kId), "c%" PRIu32 "i", (uint32_t)i);
    snprintf(kPin, sizeof(kPin), "c%" PRIu32 "p", (uint32_t)i);
    nvs->getString(kId, cs->id[i], sizeof(cs->id[i]));
    cs->pin[i] = nvs->getU8(kPin, 0);
  }
}

void consumers_init(ConsumerSet *cs) {
  for (uint8_t i = 0; i < MAX_CONSUMERS; i++) cs->usable[i] = false;
  for (uint8_t i = 0; i < cs->count; i++) {
    if (!board_gpio_usable(cs->pin[i])) {
      ESP_LOGE(TAG, "консуматор %s: GPIO %" PRIu32 " не е използваем на тази платка - прескачам", cs->id[i], (uint32_t)cs->pin[i]);
      continue;
    }
    hal_gpio_output(cs->pin[i], false);           // ниво LOW и режим изход ПРЕДИ да се свали заключването
    gpio_hold_dis((gpio_num_t)cs->pin[i]);        // след дълбок сън пинът е заключен в LOW; сега е изход LOW - без импулс
    cs->usable[i] = true;
  }
}

void consumers_all_off(const ConsumerSet *cs) {
  for (uint8_t i = 0; i < cs->count; i++) if (cs->usable[i]) hal_gpio_write(cs->pin[i], false);
}

void consumers_hold_for_deep_sleep(const ConsumerSet *cs) {
  consumers_all_off(cs);
  for (uint8_t i = 0; i < cs->count; i++) if (cs->usable[i]) gpio_hold_en((gpio_num_t)cs->pin[i]);
}

int32_t consumers_index(const ConsumerSet *cs, const char *c_id) {
  for (uint8_t i = 0; i < cs->count; i++)
    if (strncmp(cs->id[i], c_id, CONSUMER_ID_LEN) == 0) return i;
  return -1;
}

bool consumers_usable(const ConsumerSet *cs, int32_t idx) { return idx >= 0 && idx < cs->count && cs->usable[idx]; }

void consumers_set(const ConsumerSet *cs, int32_t idx, bool on) {
  if (consumers_usable(cs, idx)) hal_gpio_write(cs->pin[idx], on);
}

bool consumers_state(const ConsumerSet *cs, int32_t idx) {
  return consumers_usable(cs, idx) && hal_gpio_read(cs->pin[idx]);
}
