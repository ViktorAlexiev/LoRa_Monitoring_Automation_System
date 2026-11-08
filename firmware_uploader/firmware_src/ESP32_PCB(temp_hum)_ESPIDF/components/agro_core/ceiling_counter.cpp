#include "ceiling_counter.h"
#include <stdint.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "agro_hal.h"
#include "nvs_store.h"

static const char *TAG = "ceiling";

// Състоянието преживява дълбок сън (не и power-on reset).
#define CEIL_RTC_MAGIC UINT32_C(0xC3A1BEEF)
RTC_DATA_ATTR static uint32_t rtcMagic = 0;
RTC_DATA_ATTR static uint32_t rtcCurrent = 0;
RTC_DATA_ATTR static uint32_t rtcCommitted = 0;

void CeilingCounter::begin(const char *nvsKey) {
  key = nvsKey;
  if (hal_woke_from_deep_sleep() && rtcMagic == CEIL_RTC_MAGIC) {
    current = rtcCurrent;
    committed = rtcCommitted;
    return;
  }
  NvsStore s;
  uint32_t stored = 0;
  if (s.open(NVS_NAMESPACE, true)) stored = s.getU32(key, 0);
  current = stored;
  committed = stored;
}

bool CeilingCounter::next(uint32_t *out) {
  if (current >= committed) {
    uint32_t newCeiling = current + CEILING_COMMIT_INTERVAL;
    NvsStore s;
    // Ако записът се провали, НЕ продължаваме с непотвърден ceiling: иначе след рестарт броячът би се
    // върнал назад и (ключ, nonce) би се повторил.
    if (!s.open(NVS_NAMESPACE, false) || !s.setU32(key, newCeiling) || !s.commit()) {
      ESP_LOGE(TAG, "NVS запис на ceiling '%s' ПРОВАЛ - пакетът не се излъчва (без риск от повторен nonce)", key);
      return false;
    }
    committed = newCeiling;
  }
  *out = current;
  current++;
  rtcMagic = CEIL_RTC_MAGIC;
  rtcCurrent = current;
  rtcCommitted = committed;
  return true;
}
