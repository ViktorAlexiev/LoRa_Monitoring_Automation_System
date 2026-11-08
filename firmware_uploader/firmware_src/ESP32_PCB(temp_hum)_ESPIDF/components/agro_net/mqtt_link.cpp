#include "mqtt_link.h"
#include <stdint.h>

#include <atomic>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "agro_hal.h"
#include "mqtt_client.h"
#include "wifi_link.h"

static const char *TAG = "mqtt";

static MqttLinkConfig s_cfg;
static char s_host[65];
static char s_user[33];
static char s_pass[64];
static char s_clientId[24];
static char s_lwtTopic[48];
static char s_lwtMsg[64];
static char s_subTopics[8][MQTT_MAX_TOPIC_LEN];
static const char *s_subPtrs[8];

static esp_mqtt_client_handle_t s_client = nullptr;
static std::atomic<bool> s_connected(false);
static std::atomic<bool> s_started(false);
static std::atomic<uint32_t> s_downSinceMs(0);     // 0 = не е в състояние "Wi-Fi има, MQTT няма"
static esp_timer_handle_t s_superTimer = nullptr;

static void copyStr(char *dst, size_t cap, const char *src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

static void superCb(void *) {
  if (!wifi_link_connected() || s_connected.load()) { s_downSinceMs.store(0); return; }
  uint32_t now = hal_millis();
  if (s_downSinceMs.load() == 0) {
    s_downSinceMs.store(now ? now : 1);
    ESP_LOGW(TAG, "Wi-Fi има, MQTT няма");
  } else if ((uint32_t)(now - s_downSinceMs.load()) >= MQTT_RESTART_MS) {
    ESP_LOGE(TAG, "няма MQTT 2 мин - РЕСТАРТ");
    hal_restart();
  }
}

static void eventHandler(void *, esp_event_base_t, int32_t id, void *data) {
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      s_connected.store(true);
      s_downSinceMs.store(0);
      ESP_LOGI(TAG, "свързан с брокера");
      // clean session: абонаментите се губят при всяка нова връзка -> абонираме се наново
      for (uint8_t i = 0; i < s_cfg.subCount; i++) esp_mqtt_client_subscribe(s_client, s_cfg.subTopics[i], s_cfg.subQos);
      if (s_cfg.onConnection) s_cfg.onConnection(true, s_cfg.ctx);
      break;

    case MQTT_EVENT_DISCONNECTED:
      if (s_connected.exchange(false)) ESP_LOGW(TAG, "изгубена връзка с брокера");
      if (s_cfg.onConnection) s_cfg.onConnection(false, s_cfg.ctx);
      break;

    case MQTT_EVENT_DATA: {
      // Само цели (нефрагментирани) съобщения - нашите са < 250 B при буфер 1024 B.
      if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
        ESP_LOGW(TAG, "фрагментирано/голямо съобщение (%" PRId32 " B) - игнорирам", (int32_t)ev->total_data_len);
        break;
      }
      if (ev->topic_len <= 0 || ev->topic_len >= MQTT_MAX_TOPIC_LEN) break;
      char topic[MQTT_MAX_TOPIC_LEN];
      memcpy(topic, ev->topic, (size_t)ev->topic_len);   // topic не е NUL-терминиран
      topic[ev->topic_len] = 0;
      if (s_cfg.onMessage) s_cfg.onMessage(topic, ev->data, (size_t)ev->data_len, s_cfg.ctx);
      break;
    }

    case MQTT_EVENT_ERROR:
      ESP_LOGW(TAG, "MQTT грешка");
      break;

    default:
      break;
  }
}

bool mqtt_link_init(const MqttLinkConfig *cfg) {
  // чисто начално състояние (и при повторно стартиране на приложението в същия процес)
  s_connected.store(false);
  s_started.store(false);
  s_downSinceMs.store(0);
  s_client = nullptr;
  s_superTimer = nullptr;
  s_cfg = *cfg;
  copyStr(s_host, sizeof(s_host), cfg->host);
  copyStr(s_user, sizeof(s_user), cfg->user);
  copyStr(s_pass, sizeof(s_pass), cfg->password);
  copyStr(s_clientId, sizeof(s_clientId), cfg->clientId);
  s_cfg.host = s_host;
  s_cfg.user = s_user;
  s_cfg.password = s_pass;
  s_cfg.clientId = s_clientId;
  if (cfg->lwtTopic) {
    copyStr(s_lwtTopic, sizeof(s_lwtTopic), cfg->lwtTopic);
    copyStr(s_lwtMsg, sizeof(s_lwtMsg), cfg->lwtMessage);
    s_cfg.lwtTopic = s_lwtTopic;
    s_cfg.lwtMessage = s_lwtMsg;
  }
  uint8_t n = cfg->subCount > 8 ? 8 : cfg->subCount;
  for (uint8_t i = 0; i < n; i++) {
    copyStr(s_subTopics[i], MQTT_MAX_TOPIC_LEN, cfg->subTopics[i]);
    s_subPtrs[i] = s_subTopics[i];
  }
  s_cfg.subTopics = s_subPtrs;
  s_cfg.subCount = n;

  esp_mqtt_client_config_t mc;
  memset(&mc, 0, sizeof(mc));
  mc.broker.address.hostname = s_cfg.host;
  mc.broker.address.port = s_cfg.port;
  mc.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
  mc.credentials.client_id = s_cfg.clientId;
  if (s_user[0]) {
    mc.credentials.username = s_cfg.user;
    mc.credentials.authentication.password = s_cfg.password;
  }
  mc.session.keepalive = MQTT_KEEPALIVE_S;
  mc.session.disable_clean_session = false;          // clean session = true (изрично)
  if (cfg->lwtTopic) {
    mc.session.last_will.topic = s_cfg.lwtTopic;
    mc.session.last_will.msg = s_cfg.lwtMessage;
    mc.session.last_will.msg_len = (int32_t)strlen(s_cfg.lwtMessage);
    mc.session.last_will.qos = 1;
    mc.session.last_will.retain = 1;
  }
  mc.network.reconnect_timeout_ms = (int32_t)MQTT_RECONNECT_MS;
  mc.network.timeout_ms = 10000;
  mc.buffer.size = 1024;
  mc.buffer.out_size = 1024;

  s_client = esp_mqtt_client_init(&mc);
  if (s_client == nullptr) return false;
  if (esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, eventHandler, nullptr) != ESP_OK) return false;

  esp_timer_create_args_t sa = {};
  sa.callback = superCb;
  sa.name = "mqtt_super";
  if (esp_timer_create(&sa, &s_superTimer) != ESP_OK) return false;
  return esp_timer_start_periodic(s_superTimer, UINT64_C(1000000)) == ESP_OK;
}

void mqtt_link_network_up(void) {
  if (s_client == nullptr) return;
  if (s_started.exchange(true)) return;              // еднократно: esp-mqtt сам се свързва наново
  esp_mqtt_client_start(s_client);
}

bool mqtt_link_connected(void) { return s_connected.load(); }

bool mqtt_link_publish(const char *topic, const char *payload, size_t len, int32_t qos, bool retain) {
  if (s_client == nullptr || !s_connected.load()) return false;
  // esp_mqtt_client_publish() праща в задачата на викащия и може да блокира до network.timeout_ms (10 s) при полумъртъв
  // сокет - това спира логиката/таймаутите на викащата задача. esp_mqtt_client_enqueue() само записва съобщението в
  // outbox (store=true: и QoS 0), а изпращането става в задачата на esp-mqtt. Връща -1 при грешка, -2 при пълен outbox.
  int32_t id = esp_mqtt_client_enqueue(s_client, topic, payload, (int32_t)len, qos, retain ? 1 : 0, true);
  if (id < 0) {
    ESP_LOGW(TAG, "publish ПРОВАЛ (код %" PRId32 "), topic=%s len=%" PRIu32, id, topic, (uint32_t)len);
    return false;
  }
  return true;
}

uint32_t mqtt_link_down_ms(void) {
  uint32_t d = s_downSinceMs.load();
  return d == 0 ? 0 : (uint32_t)(hal_millis() - d);
}
