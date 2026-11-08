/*
  executor_wifi (ESP-IDF) - изпълнителен модул, който приема команди директно по MQTT (Wi-Fi), без LoRa.
  Захранва се постоянно (Wi-Fi винаги включен), затова няма сън.

  Ядро 0: Wi-Fi стек + esp-mqtt (авто reconnect, QoS 1 абониране при всяко свързване)
  Ядро 1: ew_exec (приоритет 5) - изпълнение на командите и отговорите
  Опашка (8) между тях. Виж ew_logic.h за темите и формата на съобщенията.
*/
#include "consumers.h"
#include <inttypes.h>
#include "esp_log.h"
#include "ew_config.h"
#include "ew_logic.h"
#include "agro_hal.h"
#include "board_power.h"
#include "mqtt_link.h"
#include "wifi_link.h"

static const char *TAG = "executor_wifi";

static EwConfig s_cfg;
static const char *const kSubTopics[1] = {TOPIC_WIFI_COMMANDS};
static char s_lwtTopic[40], s_lwtMsg[64];

static void onGotIp(void *) { mqtt_link_network_up(); }

extern "C" void app_main(void) {
  board_power_init();
  board_iface_power(true);                 // executor (Wi-Fi): като LoRa executor-а - интерфейсите са включени постоянно
  hal_nvs_init();
  ew_config_load(&s_cfg);

  // Всички консуматори са изключени при (ре)стартиране - съзнателно безопасно състояние; бекендът го вижда
  // чрез автоматичния state response с restart:true.
  consumers_init(&s_cfg.consumers);

  if (!s_cfg.hasId || !s_cfg.hasNet) {
    ESP_LOGE(TAG, "непълна конфигурация в NVS (id=%" PRId32 " Wi-Fi/MQTT=%" PRId32 ") - не стартирам мрежата", (int32_t)s_cfg.hasId, (int32_t)s_cfg.hasNet);
    return;
  }

  QueueHandle_t msgQ = xQueueCreate(EW_MSG_QUEUE_SIZE, sizeof(EwMsg));
  if (msgQ == nullptr) hal_fatal_restart("няма памет за опашката");
  ew_reset(&s_cfg, msgQ);
  if (xTaskCreatePinnedToCore(ew_exec_task, "ew_exec", 4096, nullptr, 5, nullptr, 1) != pdPASS) {
    hal_fatal_restart("задачата ew_exec не се създаде");
  }

  ew_lwt_topic(&s_cfg, s_lwtTopic, sizeof(s_lwtTopic));
  ew_lwt_message(&s_cfg, false, s_lwtMsg, sizeof(s_lwtMsg));
  MqttLinkConfig mc = {};
  mc.host = s_cfg.mqttHost;
  mc.port = s_cfg.mqttPort;
  mc.user = s_cfg.mqttUser;
  mc.password = s_cfg.mqttPass;
  mc.clientId = s_cfg.id;
  mc.lwtTopic = s_lwtTopic;                 // retained "offline" при неочаквано прекъсване
  mc.lwtMessage = s_lwtMsg;
  mc.subTopics = kSubTopics;
  mc.subCount = 1;
  mc.subQos = 1;                            // QoS 1; команди без retain; clean session: стари команди не се доставят
  mc.onMessage = ew_on_message;
  mc.onConnection = ew_on_connection;
  if (!mqtt_link_init(&mc)) hal_fatal_restart("MQTT клиентът не се инициализира");
  if (!wifi_link_start(s_cfg.wifiSsid, s_cfg.wifiPass, onGotIp, nullptr)) hal_fatal_restart("Wi-Fi не се стартира");

  ESP_LOGI(TAG, "Executor (Wi-Fi/MQTT) готов, M_ID=%s, консуматори=%" PRIu32, s_cfg.id, (uint32_t)s_cfg.consumers.count);
}
