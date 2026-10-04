#include "ceiling_counter.h"
#include <Preferences.h>
#include <esp_system.h>

// Състоянието преживява дълбок сън (не и power-on reset).
#define CEIL_RTC_MAGIC 0xC3A1BEEFUL
RTC_DATA_ATTR static uint32_t rtcMagic = 0;
RTC_DATA_ATTR static uint32_t rtcCurrent = 0;
RTC_DATA_ATTR static uint32_t rtcCommitted = 0;

void CeilingCounter::begin(const char* nvsKey) {
  key = nvsKey;
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP && rtcMagic == CEIL_RTC_MAGIC) {
    current = rtcCurrent;
    committed = rtcCommitted;
    return;
  }
  Preferences p;
  p.begin("cfg", true);
  uint32_t stored = p.getULong(key, 0);
  p.end();
  current = stored;
  committed = stored;
}

uint32_t CeilingCounter::next() {
  if (current >= committed) {
    committed = current + CEILING_COMMIT_INTERVAL;
    Preferences p;
    p.begin("cfg", false);
    p.putULong(key, committed);
    p.end();
  }
  uint32_t val = current;
  current++;
  rtcMagic = CEIL_RTC_MAGIC;
  rtcCurrent = current;
  rtcCommitted = committed;
  return val;
}
