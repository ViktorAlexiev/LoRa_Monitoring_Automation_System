#include "power_mgmt.h"
#include <esp_sleep.h>

void deep_sleep(uint8_t cycles) {
  esp_sleep_enable_timer_wakeup((uint64_t)cycles * 8ULL * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}
