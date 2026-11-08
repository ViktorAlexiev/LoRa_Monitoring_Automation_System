#include "power.h"
#include "board_power.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"

static const char *TAG = "power";

bool power_enable_auto_light_sleep(int32_t wake_gpio) {
  // Събуждане от лек сън по пин: нивото HIGH на DIO1 = има пакет (RxDone).
  gpio_wakeup_enable((gpio_num_t)wake_gpio, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  esp_pm_config_t cfg = {};
  cfg.max_freq_mhz = 80;          // радио логиката не е тежка; по-ниска честота = по-малък ток
  cfg.min_freq_mhz = 40;          // XTAL
  cfg.light_sleep_enable = true;
  esp_err_t e = esp_pm_configure(&cfg);
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "автоматичният лек сън не е включен (%s) - провери CONFIG_PM_ENABLE / TICKLESS_IDLE", esp_err_to_name(e));
    return false;
  }
  return true;
}

void power_deep_sleep_us(uint64_t us) {
  board_power_hold_for_deep_sleep();   // ключовете остават ИЗКЛЮЧЕНИ по време на съня (пиновете не "плават")
  esp_sleep_enable_timer_wakeup(us);
  esp_deep_sleep_start();
  for (;;) {}   // esp_deep_sleep_start() не връща
}
