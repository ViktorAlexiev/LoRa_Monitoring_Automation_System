#include "ew_logic.h"
#include <inttypes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "agro_hal.h"
#include "json_util.h"
#include "mqtt_link.h"
#include "protocol.h"

static const char *TAG = "ew_logic";

static const EwConfig *s_cfg = nullptr;
static QueueHandle_t s_msgQ = nullptr;
static bool s_restartStateSent = false;     // автоматичният state response след boot - веднъж на boot
static uint32_t s_lastHb = 0;

void ew_reset(const EwConfig *cfg, QueueHandle_t msgQ) {
  s_cfg = cfg;
  s_msgQ = msgQ;
  s_restartStateSent = false;
  s_lastHb = hal_millis();
}

void ew_lwt_topic(const EwConfig *cfg, char *out, size_t cap) { snprintf(out, cap, "executors/%s/lwt", cfg->id); }
void ew_lwt_message(const EwConfig *cfg, bool online, char *out, size_t cap) {
  snprintf(out, cap, "{\"id\":\"%s\",\"online\":%s}", cfg->id, online ? "true" : "false");
}

// ---------------- Публикуване ----------------
static void publish(const char *topic, JsonOut &w, char *buf, size_t cap, int32_t qos, bool retain = false) {
  size_t len = 0;
  if (!w.print(buf, cap, &len)) { ESP_LOGE(TAG, "[MQTT] JSON не е изграден или не се побира, topic=%s", topic); return; }
  if (!mqtt_link_publish(topic, buf, len, qos, retain)) {
    ESP_LOGW(TAG, "[MQTT] не е публикувано (няма връзка/отказ), topic=%s", topic);
  }
}

static void publishCommandStatus(const char *c_id, uint8_t com, uint8_t status) {
  char buf[160];
  JsonOut w;
  char comHex[3];
  snprintf(comHex, sizeof(comHex), "%02" PRIX32, (uint32_t)com);
  w.beginObject().str("M_ID", s_cfg->id).str("C_ID", c_id).str("com", comHex).num("status", status).endObject();
  publish(TOPIC_COMMANDS_STATUS, w, buf, sizeof(buf), 1);
  ESP_LOGI(TAG, "[STATUS] %s com=%s status=%" PRIu32, c_id, comHex, (uint32_t)status);
}

// Състоянието се чете от реалния пин (както при LoRa executor-а), не от запомнена стойност.
static void publishStates(bool isRestart) {
  char buf[512];
  JsonOut w;
  w.beginObject().str("id", s_cfg->id);
  if (isRestart) w.boolean("restart", true);          // само за автоматичния отговор след boot
  w.beginArray("states");
  for (uint8_t i = 0; i < s_cfg->consumers.count; i++) {
    w.beginObject().str("id", s_cfg->consumers.id[i]).str("state", consumers_state(&s_cfg->consumers, i) ? "ON" : "OFF").endObject();
  }
  w.endArray().endObject();
  publish(TOPIC_STATE_RESP, w, buf, sizeof(buf), 1);
  ESP_LOGI(TAG, "%s%" PRIu32, isRestart ? "[STATE_RESP RESTART] " : "[STATE_RESP] ", (uint32_t)s_cfg->consumers.count);
}

// ---------------- Входящи съобщения (контекст на esp-mqtt задачата) ----------------
void ew_on_message(const char *topic, const char *data, size_t len, void *) {
  if (strcmp(topic, TOPIC_WIFI_COMMANDS) != 0) return;

  char m_id[MODULE_ID_LEN + 8];
  JsonDoc doc(data, len);
  if (!jsonGetString(doc.root(), "M_ID", m_id, sizeof(m_id))) {
    ESP_LOGW(TAG, "[ERR] Bad JSON wifi_commands");
    return;
  }
  if (strcmp(m_id, s_cfg->id) != 0) return;            // чужда команда - всички Wi-Fi модули получават темата

  char c_id[CONSUMER_ID_LEN + 1] = "";
  char com[8] = "";
  // C_ID липсва при заявка за състояние. По-дълъг от 4 символа се ОТХВЪРЛЯ (като в gateway): иначе "V0123" би се
  // отрязал до "V012" и би превключил друг консуматор вместо да върне NACK.
  if (jsonField(doc.root(), "C_ID", c_id, sizeof(c_id)) == JSON_FIELD_TOO_LONG) {
    ESP_LOGW(TAG, "[ERR] C_ID е по-дълъг от %" PRId32 " символа - командата е отхвърлена", (int32_t)CONSUMER_ID_LEN);
    return;
  }
  jsonGetString(doc.root(), "com", com, sizeof(com));
  uint8_t comByte = 0;
  if (!parseHexByte(com, &comByte)) {
    ESP_LOGW(TAG, "[ERR] Bad JSON wifi_commands (com е точно 2 hex цифри)");
    return;
  }
  EwMsg m;
  memset(&m, 0, sizeof(m));
  m.com = comByte;
  m.kind = (m.com == CMD_STATE_REQ) ? EW_MSG_STATE_REQ : EW_MSG_COMMAND;
  strncpy(m.c_id, c_id, CONSUMER_ID_LEN);
  if (xQueueSend(s_msgQ, &m, 0) != pdTRUE) ESP_LOGW(TAG, "[WARN] опашката е пълна - съобщението е изхвърлено");
}

void ew_on_connection(bool connected, void *) {
  if (!connected) return;
  EwMsg m;
  memset(&m, 0, sizeof(m));
  m.kind = EW_MSG_CONNECTED;
  xQueueSend(s_msgQ, &m, 0);        // при пълна опашка се губи само известието; следващото свързване го повтаря
}

// ---------------- Изпълнение (задачата ew_exec) ----------------
void ew_handle(const EwMsg &m) {
  const ConsumerSet *cs = &s_cfg->consumers;
  if (m.kind == EW_MSG_CONNECTED) {
    // Last Will (retained): брокерът публикува "offline", ако връзката падне без DISCONNECT; тук - "online"
    char topic[40], msg[64];
    ew_lwt_topic(s_cfg, topic, sizeof(topic));
    ew_lwt_message(s_cfg, true, msg, sizeof(msg));
    mqtt_link_publish(topic, msg, strlen(msg), 1, true);
    if (!s_restartStateSent) {
      publishStates(true);              // автоматичен state response веднага след boot (restart:true)
      s_restartStateSent = true;
    }
    return;
  }
  if (m.kind == EW_MSG_STATE_REQ) {
    publishStates(false);
    return;
  }
  int32_t idx = consumers_index(cs, m.c_id);
  bool cmdOk = (m.com == CMD_ON || m.com == CMD_OFF);
  if (idx >= 0 && consumers_usable(cs, idx) && cmdOk) {
    consumers_set(cs, idx, m.com == CMD_ON);
    ESP_LOGI(TAG, "[EXEC] %s (pin %" PRIu32 ") -> %s", m.c_id, (uint32_t)cs->pin[idx], m.com == CMD_ON ? "ON" : "OFF");
    publishCommandStatus(m.c_id, m.com, STATUS_ACK);
  } else {
    ESP_LOGW(TAG, "[REJECT] %s%s%s", m.c_id, idx < 0 ? " - консуматор не съществува" : (consumers_usable(cs, idx) ? "" : " - пинът не е използваем"),
             cmdOk ? "" : " - невалидна команда");
    publishCommandStatus(m.c_id, m.com, STATUS_NACK);
  }
}

void ew_tick(void) {
  uint32_t now = hal_millis();
  if ((uint32_t)(now - s_lastHb) >= EW_HEARTBEAT_INTERVAL_MS) {
    s_lastHb = now;
    if (mqtt_link_connected()) {
      char buf[96];
      JsonOut w;
      w.beginObject().str("id", s_cfg->id).unum("ts", now / 1000).endObject();
      publish(TOPIC_HEARTBEAT, w, buf, sizeof(buf), 0);
      ESP_LOGI(TAG, "[HEARTBEAT] %s", s_cfg->id);
    }
  }
}

uint32_t ew_next_wait_ms(void) {
  uint32_t now = hal_millis();
  uint32_t at = s_lastHb + EW_HEARTBEAT_INTERVAL_MS;
  return (int32_t)(now - at) >= 0 ? 0 : (uint32_t)(at - now);
}

void ew_exec_task(void *) {
  EwMsg m;
  for (;;) {
    HAL_STACK_PROBE();
    if (xQueueReceive(s_msgQ, &m, pdMS_TO_TICKS(ew_next_wait_ms())) == pdTRUE) ew_handle(m);
    ew_tick();
  }
}
