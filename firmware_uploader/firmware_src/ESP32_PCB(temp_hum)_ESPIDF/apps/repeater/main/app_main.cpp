/*
  repeater (ESP-IDF) - препраща uplink пакети (sensor, heartbeat-и) от RX честотата на TX честотата и праща свой
  heartbeat. НЕ препраща downlink (команди/ACK) - те минават само между gateway и executor.

  Задачи (ядро 1; ядро 0 остава свободно - няма Wi-Fi):
    rep_rx  (приоритет 6) - приема по DIO1, dedup, опашка
    rep_fwd (приоритет 5) - backoff, достъп до канала, предаване
  Радиото е едно: mutex между двете задачи. Heartbeat: esp_timer на ~5 минути.
  Енергия: автоматичен лек сън (виж power.h) - чипът спи, докато няма пакет или срок.
*/
#include "board_pins.h"
#include <inttypes.h>
#include "esp_log.h"
#include "agro_hal.h"
#include "board_power.h"
#include "power.h"
#include "rep_config.h"
#include "rep_radio.h"

static const char *TAG = "repeater";

static RepConfig s_cfg;

extern "C" void app_main(void) {
  board_power_init();                      // repeater: ключът на интерфейсите остава ИЗКЛЮЧЕН (нищо не е закачено)
  hal_nvs_init();
  rep_config_load(&s_cfg);
  if (!s_cfg.hasId || !s_cfg.hasKey) {
    ESP_LOGE(TAG, "няма id/ключ в NVS - не стартирам радиото");
    return;
  }

  // Ниска батерия: под BATT_CUTOFF_MV (до BATT_RESUME_MV) repeater-ът не стартира радиото, а заспива дълбоко
  if (s_cfg.hasBattery && board_battery_is_low(board_battery_mv())) {
    ESP_LOGW(TAG, "ниска батерия: дълбок сън за %" PRId32 " s", (int32_t)BATT_LOW_SLEEP_S);
    power_deep_sleep_us(BATT_LOW_SLEEP_US);
  }
  hal_task_wdt_start(AGRO_TASK_WDT_TIMEOUT_MS);   // task watchdog (30 s); rep_rx го храни

  QueueHandle_t fwdQ = xQueueCreate(FWD_QUEUE_SIZE, sizeof(FwdItem));
  SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  if (fwdQ == nullptr || mutex == nullptr) hal_fatal_restart("няма памет за опашката/mutex-а");
  rep_radio_reset(&s_cfg, fwdQ, mutex);

  if (xTaskCreatePinnedToCore(rep_rx_task, "rep_rx", 4096, nullptr, 6, nullptr, 1) != pdPASS) {
    hal_fatal_restart("задачата rep_rx не се създаде");
  }
  if (xTaskCreatePinnedToCore(rep_fwd_task, "rep_fwd", 4096, nullptr, 5, nullptr, 1) != pdPASS) {
    hal_fatal_restart("задачата rep_fwd не се създаде");
  }

  power_enable_auto_light_sleep(LORA_DIO1);

  ESP_LOGI(TAG, "Repeater modul готов, M_ID=%s RX_freq=%" PRIu32 " TX_freq=%" PRIu32 " sf=%" PRIu32 " bw=%" PRIu32, s_cfg.id, (uint32_t)s_cfg.freqRxHz,
           (uint32_t)s_cfg.freqTxHz, (uint32_t)s_cfg.sf, (uint32_t)s_cfg.bwHz);
}
