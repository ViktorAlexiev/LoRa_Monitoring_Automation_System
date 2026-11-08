/*
  executor (ESP-IDF, LoRa) - приема команди от gateway-я по радио, превключва консуматори (GPIO), потвърждава
  с ACK/NACK, праща heartbeat и автоматичен state response след boot.

  Задачи (ядро 1; ядро 0 остава свободно - няма Wi-Fi):
    exec_radio (приоритет 6) - единственият собственик на радиото; събужда се от DIO1 / срок / заявка
    exec_cmd   (приоритет 5) - изпълнява командите (GPIO) и заявява ACK
  Heartbeat: esp_timer на ~5 минути. Енергия: когато всички задачи чакат, FreeRTOS tickless idle + Power
  Management приспиват чипа (автоматичен лек сън); събужда го DIO1 на радиото или най-близкият срок.
*/
#include "board_pins.h"
#include <inttypes.h>
#include "esp_log.h"
#include "exec_cmd.h"
#include "exec_config.h"
#include "consumers.h"
#include "exec_radio.h"
#include "agro_hal.h"
#include "board_power.h"
#include "power.h"

static const char *TAG = "executor";

static ExecConfig s_cfg;   // статична: задачите държат указател към нея

extern "C" void app_main(void) {
  board_power_init();
  board_iface_power(true);                 // executor: интерфейсите се включват в началото и остават включени
  hal_nvs_init();
  exec_config_load(&s_cfg);

  // Всички консуматори са изключени при (ре)стартиране - съзнателно безопасно състояние; бекендът го вижда
  // чрез автоматичния state response след boot.
  consumers_init(&s_cfg.consumers);

  if (!s_cfg.hasId || !s_cfg.hasKey) {
    ESP_LOGE(TAG, "няма id/ключ в NVS - не стартирам радиото (консуматорите остават изключени)");
    return;
  }

  QueueHandle_t cmdQ = xQueueCreate(8, sizeof(CommandPacket));      // радио -> изпълнение
  QueueHandle_t ackInQ = xQueueCreate(8, sizeof(AckReq));           // изпълнение -> радио
  if (cmdQ == nullptr || ackInQ == nullptr) hal_fatal_restart("няма памет за опашките");
  exec_radio_reset(&s_cfg, cmdQ, ackInQ);

  TaskHandle_t radioTask = nullptr;
  if (xTaskCreatePinnedToCore(exec_radio_task, "exec_radio", 5120, nullptr, 6, &radioTask, 1) != pdPASS) {
    hal_fatal_restart("задачата exec_radio не се създаде");
  }
  exec_radio_set_task(radioTask);
  exec_cmd_init(&s_cfg, cmdQ, ackInQ, radioTask);
  if (xTaskCreatePinnedToCore(exec_cmd_task, "exec_cmd", 3072, nullptr, 5, nullptr, 1) != pdPASS) {
    hal_fatal_restart("задачата exec_cmd не се създаде");
  }

  power_enable_auto_light_sleep(LORA_DIO1);

  ESP_LOGI(TAG, "Executor modul готов, M_ID=%s freq=%" PRIu32 " sf=%" PRIu32 " bw=%" PRIu32 " consumers=%" PRIu32, s_cfg.id, (uint32_t)s_cfg.freqHz,
           (uint32_t)s_cfg.sf, (uint32_t)s_cfg.bwHz, (uint32_t)s_cfg.consumers.count);
}
