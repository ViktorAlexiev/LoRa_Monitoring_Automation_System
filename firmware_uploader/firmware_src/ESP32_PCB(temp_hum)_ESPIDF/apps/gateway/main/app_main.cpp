/*
  gateway (ESP-IDF) - мост LoRa <-> MQTT. ESP32-S3 е двуядрен; работата е разделена така:

    ядро 1:  gw_radio (приоритет 10) - единственият собственик на SX1262: приема (DIO1), валидира, предава
    ядро 0:  gw_cmd   (приоритет 5)  - команди/ACK/таймаути, JSON, MQTT публикуване
             Wi-Fi стекът и esp-mqtt (системни задачи, ядро 0)

  Task Watchdog на ESP-IDF (30 s, с рестарт): gw_radio и gw_cmd са абонирани и го "хранят" на всяка обиколка;
  ако някоя не го направи (блокирала/зациклила в логиката), чипът се рестартира. Двете задачи се будят поне
  веднъж на 5 s (ограничени таймаути в чакането), затова липсата на работа не е зависване.

  Така блокиращо Wi-Fi/MQTT свързване или бавно публикуване на ядро 0 не може да отнеме ефирно време от
  приемането. Между ядрата има само опашки: gwInQ (радио+MQTT -> логика) и radioInQ (логика -> радио).
  Gateway-ят е на мрежово захранване: няма сън.
*/
#include "board_pins.h"
#include <stdint.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "gw_config.h"
#include "gw_logic.h"
#include "gw_radio.h"
#include "agro_hal.h"
#include "board_power.h"
#include "mqtt_link.h"
#include "wifi_link.h"

static const char *TAG = "gateway";

static GwConfig s_cfg;
static const char *const kSubTopics[2] = {TOPIC_COMMANDS, TOPIC_STATE_REQ};

static void onGotIp(void *) { mqtt_link_network_up(); }

#define GW_TASK_WDT_TIMEOUT_MS UINT32_C(30000)

// Task Watchdog: 30 s и РЕСТАРТ (по подразбиране в ESP-IDF той само пише предупреждение). Системата го стартира
// преди app_main (CONFIG_ESP_TASK_WDT_INIT); ако не е - стартираме го тук.
static void configureTaskWdt() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = GW_TASK_WDT_TIMEOUT_MS;
  cfg.idle_core_mask = (UINT32_C(1) << portNUM_PROCESSORS) - 1;      // idle задачите на двете ядра остават наблюдавани
  cfg.trigger_panic = true;
  esp_err_t e = esp_task_wdt_reconfigure(&cfg);
  if (e == ESP_ERR_INVALID_STATE) e = esp_task_wdt_init(&cfg);
  if (e != ESP_OK) ESP_LOGE(TAG, "Task watchdog не се настрои (%s) - няма защита от зависване", esp_err_to_name(e));
}

extern "C" void app_main(void) {
  board_power_init();                      // gateway: ключът на интерфейсите остава ИЗКЛЮЧЕН (нищо не е закачено)
  hal_nvs_init();
  gw_config_load(&s_cfg);
  if (!s_cfg.hasId || !s_cfg.hasKey || !s_cfg.hasNet) {
    ESP_LOGE(TAG, "непълна конфигурация в NVS (id=%" PRId32 " ключ=%" PRId32 " Wi-Fi/MQTT=%" PRId32 ") - не стартирам", (int32_t)s_cfg.hasId, (int32_t)s_cfg.hasKey, (int32_t)s_cfg.hasNet);
    return;
  }

  configureTaskWdt();

  QueueHandle_t gwInQ = xQueueCreate(16, sizeof(GwMsg));              // радио + MQTT -> логика
  QueueHandle_t radioInQ = xQueueCreate(GW_TX_QUEUE_SIZE, sizeof(TxReq));   // логика -> радио
  if (gwInQ == nullptr || radioInQ == nullptr) hal_fatal_restart("няма памет за опашките");
  gw_radio_reset(&s_cfg, gwInQ, radioInQ);
  gw_logic_reset(&s_cfg, gwInQ, radioInQ);

  TaskHandle_t radioTask = nullptr;
  if (xTaskCreatePinnedToCore(gw_radio_task, "gw_radio", 5120, nullptr, 10, &radioTask, 1) != pdPASS) {
    hal_fatal_restart("задачата gw_radio не се създаде");
  }
  gw_radio_set_task(radioTask);
  gw_logic_set_radio_task(radioTask);
  if (xTaskCreatePinnedToCore(gw_logic_task, "gw_cmd", 6144, nullptr, 5, nullptr, 0) != pdPASS) {
    hal_fatal_restart("задачата gw_cmd не се създаде");
  }

  MqttLinkConfig mc = {};
  mc.host = s_cfg.mqttHost;
  mc.port = s_cfg.mqttPort;
  mc.user = s_cfg.mqttUser;
  mc.password = s_cfg.mqttPass;
  mc.clientId = s_cfg.id;
  mc.subTopics = kSubTopics;
  mc.subCount = 2;
  mc.subQos = 1;
  mc.onMessage = gw_mqtt_on_message;
  if (!mqtt_link_init(&mc)) hal_fatal_restart("MQTT клиентът не се инициализира");
  if (!wifi_link_start(s_cfg.wifiSsid, s_cfg.wifiPass, onGotIp, nullptr)) hal_fatal_restart("Wi-Fi не се стартира");

  ESP_LOGI(TAG, "Gateway готов, id=%s freq=%" PRIu32 " sf=%" PRIu32 " bw=%" PRIu32, s_cfg.id, (uint32_t)s_cfg.freqHz, (uint32_t)s_cfg.sf,
           (uint32_t)s_cfg.bwHz);
}
