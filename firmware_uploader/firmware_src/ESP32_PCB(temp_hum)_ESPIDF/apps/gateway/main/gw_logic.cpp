#include "gw_logic.h"
#include <inttypes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ceiling_counter.h"
#include "crypto_common.h"
#include "esp_log.h"
#include "agro_hal.h"
#include "json_util.h"
#include "mqtt_link.h"
#include "protocol.h"
#include "radio_timing.h"
#include "esp_task_wdt.h"

static const char *TAG = "gw_logic";

static const GwConfig *s_cfg = nullptr;
static QueueHandle_t s_gwInQ = nullptr, s_radioInQ = nullptr;
static TaskHandle_t s_radioTask = nullptr;

// НЕ е thread-safe по дизайн: всичко (освен gw_mqtt_on_message) се вика само от задачата gw_cmd.
static CeilingCounter s_cmdCounter;     // ЕДИН общ nonce поток за командите към ВСИЧКИ executor-и (type-id разграничава)
static bool s_counterReady = false;

// ---------- Команда "в полет" ----------
static bool s_cmdPending = false;
static char s_pM[MODULE_ID_LEN + 1], s_pC[CONSUMER_ID_LEN + 1];
static uint8_t s_pCom = 0;
static uint32_t s_pSentAt = 0;
static uint8_t s_pRetries = 0;

// ---------- State request "в полет" ----------
static bool s_stateReqPending = false;
static char s_sM[MODULE_ID_LEN + 1];
static uint32_t s_sSentAt = 0;
static uint8_t s_sRetries = 0;

static uint32_t s_lastHb = 0;

void gw_logic_reset(const GwConfig *cfg, QueueHandle_t gwInQ, QueueHandle_t radioInQ) {
  s_cfg = cfg;
  s_gwInQ = gwInQ;
  s_radioInQ = radioInQ;
  s_radioTask = nullptr;
  s_counterReady = false;
  s_cmdPending = s_stateReqPending = false;
  s_pRetries = s_sRetries = 0;
  s_lastHb = hal_millis();
}
void gw_logic_set_radio_task(TaskHandle_t t) { s_radioTask = t; }
bool gw_cmd_pending(void) { return s_cmdPending; }
bool gw_state_req_pending(void) { return s_stateReqPending; }

static inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

// ---------- MQTT изход ----------
// Печата JSON дървото в буфера на извикващия и го публикува. Неуспех (няма памет за дървото или не се
// побира в буфера) се логва и нищо не се публикува - никога отрязан/повреден JSON.
static void publish(const char *topic, JsonOut &w, char *buf, size_t cap, int32_t qos) {
  size_t len = 0;
  if (!w.print(buf, cap, &len)) { ESP_LOGE(TAG, "[MQTT] JSON не е изграден или не се побира в буфера, topic=%s", topic); return; }
  if (!mqtt_link_publish(topic, buf, len, qos, false)) {
    ESP_LOGW(TAG, "[MQTT] не е публикувано (няма връзка/отказ), topic=%s len=%" PRIu32, topic, (uint32_t)len);
  }
}

static void publishCommandStatus(const char *m_id, const char *c_id, uint8_t com, uint8_t status) {
  char buf[160];
  JsonOut w;
  char comHex[3];
  snprintf(comHex, sizeof(comHex), "%02" PRIX32, (uint32_t)com);
  w.beginObject().str("M_ID", m_id).str("C_ID", c_id).str("com", comHex).num("status", status).endObject();
  publish(TOPIC_COMMANDS_STATUS, w, buf, sizeof(buf), 1);   // QoS 1: статусът не бива да се губи при кратко прекъсване
  ESP_LOGI(TAG, "[STATUS] %s/%s com=%s status=%" PRIu32, m_id, c_id, comHex, (uint32_t)status);
}

static void putRaw(JsonOut &w, const char *key, int16_t raw) {
  if (raw == SENSOR_RAW_NODATA) w.num(key, SENSOR_NODATA_JSON);   // "няма данни" - без деление на 100
  else w.fixed2(key, raw);
}

// ---------- Радио изход ----------
static bool enqueueTx(const uint8_t *buf, uint8_t len) {
  TxReq r;
  r.len = len;
  memcpy(r.buf, buf, len);
  if (xQueueSend(s_radioInQ, &r, 0) != pdTRUE) {
    ESP_LOGE(TAG, "[ERR] TX опашката е пълна, пакетът е изгубен");
    return false;
  }
  if (s_radioTask) xTaskNotifyGive(s_radioTask);
  return true;
}

static bool nextCounter(uint32_t *c) {
  if (!s_counterReady) { s_cmdCounter.begin("cmdceil"); s_counterReady = true; }
  return s_cmdCounter.next(c);
}

// Команда (криптирана): [target M_ID, чисто] + [wire: GATEWAY_ID(sender)+ciphertext(C_ID+com+status)+counter+tag]
static void sendCommandPacket() {
  uint8_t plaintext[CMD_PLAINTEXT_LEN];
  memcpy(plaintext, s_pC, 4);
  plaintext[4] = s_pCom;
  plaintext[5] = 0xFF;

  uint32_t ctr;
  if (!nextCounter(&ctr)) return;
  uint8_t wire[CMD_WIRE_LEN];
  memset(wire, 0, MODULE_ID_LEN);
  memcpy(wire, s_pM, strnlen(s_pM, MODULE_ID_LEN));            // target, чисто - executor-ът филтрира по това ПРЕДИ decrypt
  uint8_t n = cryptoBuildWirePacket(wire + MODULE_ID_LEN, s_cfg->key, CRYPTO_TYPE_GW_CMD, (const uint8_t *)s_cfg->id, ctr,
                                    plaintext, sizeof(plaintext));
  if (n) enqueueTx(wire, (uint8_t)(MODULE_ID_LEN + n));
}

// State request (криптирана): [target M_ID, чисто] + [wire: GATEWAY_ID+ciphertext(0)+counter+tag]
static void sendStateRequestPacket() {
  uint32_t ctr;
  if (!nextCounter(&ctr)) return;
  uint8_t wire[STATE_REQ_WIRE_LEN];
  memset(wire, 0, MODULE_ID_LEN);
  memcpy(wire, s_sM, strnlen(s_sM, MODULE_ID_LEN));
  uint8_t n = cryptoBuildWirePacket(wire + MODULE_ID_LEN, s_cfg->key, CRYPTO_TYPE_STATE_REQ, (const uint8_t *)s_cfg->id, ctr, NULL, 0);
  if (n) enqueueTx(wire, (uint8_t)(MODULE_ID_LEN + n));
}

// State-resp ACK (само за RESTART варианта): [target M_ID, чисто] + [wire: GATEWAY_ID+ciphertext(0)+counter+tag]
static void sendStateRespAck(const char *target6) {
  uint32_t ctr;
  if (!nextCounter(&ctr)) return;
  uint8_t wire[STATE_REQ_WIRE_LEN];
  memset(wire, 0, MODULE_ID_LEN);
  memcpy(wire, target6, strnlen(target6, MODULE_ID_LEN));
  uint8_t n = cryptoBuildWirePacket(wire + MODULE_ID_LEN, s_cfg->key, CRYPTO_TYPE_STATE_RESP_ACK, (const uint8_t *)s_cfg->id, ctr, NULL, 0);
  if (n && enqueueTx(wire, (uint8_t)(MODULE_ID_LEN + n))) ESP_LOGI(TAG, "[TX STATE_RESP_ACK] заявен");
}

static void startCommand(const char *m_id, const char *c_id, uint8_t com) {
  memset(s_pM, 0, sizeof(s_pM));
  memset(s_pC, 0, sizeof(s_pC));
  strncpy(s_pM, m_id, MODULE_ID_LEN);
  strncpy(s_pC, c_id, CONSUMER_ID_LEN);
  s_pCom = com;
  s_pSentAt = hal_millis();
  s_pRetries = 0;
  s_cmdPending = true;
  sendCommandPacket();
  ESP_LOGI(TAG, "[TX CMD] %s/%s", s_pM, s_pC);
}

static void startStateRequest(const char *m_id) {
  memset(s_sM, 0, sizeof(s_sM));
  strncpy(s_sM, m_id, MODULE_ID_LEN);
  s_sSentAt = hal_millis();
  s_sRetries = 0;
  s_stateReqPending = true;
  sendStateRequestPacket();
  ESP_LOGI(TAG, "[TX STATE_REQ] %s", s_sM);
}

// ---------- MQTT вход ----------
void gw_mqtt_on_message(const char *topic, const char *data, size_t len, void *) {
  GwMsg m;
  memset(&m, 0, sizeof(m));
  if (strcmp(topic, TOPIC_COMMANDS) == 0) m.type = GM_MQTT_CMD;
  else if (strcmp(topic, TOPIC_STATE_REQ) == 0) m.type = GM_MQTT_STATE_REQ;
  else return;
  if (len > GW_MQTT_PAYLOAD_MAX) { ESP_LOGW(TAG, "[ERR] прекалено дълго MQTT съобщение (%" PRIu32 " B) на %s", (uint32_t)len, topic); return; }
  memcpy(m.payload, data, len);
  m.payloadLen = (uint16_t)len;
  if (xQueueSend(s_gwInQ, &m, 0) != pdTRUE) ESP_LOGE(TAG, "[ERR] входната опашка е пълна, MQTT съобщение от %s е изгубено", topic);
}

static void handleMqttCommand(const GwMsg &m) {
  if (s_cmdPending) {
    ESP_LOGW(TAG, "[WARN] команда вече чака - нова игнорирана");
    return;
  }
  char m_id[MODULE_ID_LEN + 1], c_id[CONSUMER_ID_LEN + 1], com[8];
  JsonDoc doc(m.payload, m.payloadLen);
  bool okM = jsonGetString(doc.root(), "M_ID", m_id, sizeof(m_id));
  bool okC = jsonGetString(doc.root(), "C_ID", c_id, sizeof(c_id));
  bool okCom = jsonGetString(doc.root(), "com", com, sizeof(com));
  uint8_t comByte = 0;
  if (!okM || !okC || !okCom || m_id[0] == 0 || c_id[0] == 0 || !parseHexByte(com, &comByte)) {
    ESP_LOGW(TAG, "[ERR] Bad JSON commands (липсва/невалидно M_ID, C_ID или com - com е точно 2 hex цифри)");
    return;
  }
  startCommand(m_id, c_id, comByte);
}

static void handleMqttStateReq(const GwMsg &m) {
  if (s_stateReqPending) {
    ESP_LOGW(TAG, "[WARN] state request вече чака - нова игнорирана");
    return;
  }
  char m_id[MODULE_ID_LEN + 1];
  JsonDoc doc(m.payload, m.payloadLen);
  if (!jsonGetString(doc.root(), "M_ID", m_id, sizeof(m_id))) { ESP_LOGW(TAG, "[ERR] Bad JSON state req"); return; }
  if (m_id[0] == 0) return;
  startStateRequest(m_id);
}

// ---------- Радио събития -> MQTT ----------
static void handleSensor(const GwMsg &m) {
  char buf[256];
  JsonOut w;
  w.beginObject().str("id", m.id);
  putRaw(w, "soil_t", m.v[0]);
  putRaw(w, "soil_h", m.v[1]);
  putRaw(w, "air_t", m.v[2]);
  putRaw(w, "air_h", m.v[3]);
  w.num("battery_mv", m.batMv);               // BATTERY_NODATA_JSON (-32768) = пакетът е без батерия или няма измерване
  w.num("rssi", m.rssi).fixed2("snr", m.snrX100).unum("ts", m.tsSec).endObject();
  publish(TOPIC_SENSORS, w, buf, sizeof(buf), 0);                       // QoS 0: загуба на едно показание не е проблем; стари показания са по-лоши
}

static void handleAck(const GwMsg &m) {
  if (s_cmdPending && strncmp(m.id, s_pM, MODULE_ID_LEN) == 0 && memcmp(m.cId, s_pC, 4) == 0 && m.com == s_pCom) {
    uint8_t status = (m.status == 0) ? STATUS_ACK : STATUS_NACK;
    publishCommandStatus(m.id, m.cId, m.com, status);
    s_cmdPending = false;
  }
}

static void handleHeartbeat(const GwMsg &m) {
  char buf[128];
  JsonOut w;
  w.beginObject().str("id", m.id);
  if (m.type == GM_HB_REPEATER) w.num("rssi", m.rssi).fixed2("snr", m.snrX100);
  w.num("battery_mv", m.batMv);               // -32768 = heartbeat без батерия (стар формат, 14 B) или няма измерване
  w.unum("ts", m.tsSec).endObject();
  publish(TOPIC_HEARTBEAT, w, buf, sizeof(buf), 0);
  ESP_LOGI(TAG, "[HB %s] %s", m.type == GM_HB_REPEATER ? "REPEATER" : "EXEC", m.id);
}

// В MQTT двата варианта (нормален/RESTART) изглеждат еднакво, освен "restart":true - RESTART вариантът получава
// ACK обратно по радиото и НЕ е задължително отговор на изрична заявка.
static void handleState(const GwMsg &m) {
  int32_t entries = m.stateLen / STATE_ENTRY_LEN;
  char buf[1024];
  JsonOut w;
  w.beginObject().str("id", m.id);
  if (m.restart) w.boolean("restart", true);          // изпуснато (не false) при нормален отговор
  w.beginArray("states");
  for (int32_t i = 0; i < entries; i++) {
    int32_t off = i * STATE_ENTRY_LEN;
    char c_id[5] = {0};
    memcpy(c_id, m.state + off, 4);
    w.beginObject().str("id", c_id).str("state", m.state[off + 4] ? "ON" : "OFF").endObject();
  }
  w.endArray().endObject();
  publish(TOPIC_STATE_RESP, w, buf, sizeof(buf), 1);

  if (m.restart) sendStateRespAck(m.id);
  else if (s_stateReqPending && strncmp(m.id, s_sM, MODULE_ID_LEN) == 0) s_stateReqPending = false;

  ESP_LOGI(TAG, "%s%s entries=%" PRId32, m.restart ? "[STATE_RESP RESTART] " : "[STATE_RESP] ", m.id, entries);
}

void gw_logic_handle(const GwMsg &m) {
  switch (m.type) {
    case GM_SENSOR: handleSensor(m); break;
    case GM_ACK: handleAck(m); break;
    case GM_HB_REPEATER:
    case GM_HB_EXEC: handleHeartbeat(m); break;
    case GM_STATE: handleState(m); break;
    case GM_MQTT_CMD: handleMqttCommand(m); break;
    case GM_MQTT_STATE_REQ: handleMqttStateReq(m); break;
    default: break;
  }
}

// ---------- Таймаути, повторения, heartbeat ----------
void gw_logic_tick(void) {
  uint32_t now = hal_millis();

  if (s_cmdPending && (uint32_t)(now - s_pSentAt) >= radioAckTimeoutCmdMs()) {
    if (s_pRetries < MAX_RETRIES) {
      s_pRetries++;
      s_pSentAt = now;
      sendCommandPacket();
      ESP_LOGW(TAG, "[RETRY CMD] опит #%" PRIu32, (uint32_t)s_pRetries);
    } else {
      publishCommandStatus(s_pM, s_pC, s_pCom, STATUS_TIMEOUT);
      s_cmdPending = false;
    }
  }

  if (s_stateReqPending && (uint32_t)(now - s_sSentAt) >= radioStateReqTimeoutMs()) {
    if (s_sRetries < MAX_RETRIES) {
      s_sRetries++;
      s_sSentAt = now;
      sendStateRequestPacket();
      ESP_LOGW(TAG, "[RETRY STATE_REQ] опит #%" PRIu32, (uint32_t)s_sRetries);
    } else {
      char buf[96];
      JsonOut w;
      w.beginObject().str("id", s_sM).str("status", "timeout").endObject();
      publish(TOPIC_STATE_RESP, w, buf, sizeof(buf), 1);
      s_stateReqPending = false;
      ESP_LOGW(TAG, "[STATE_REQ] timeout");
    }
  }

  if ((uint32_t)(now - s_lastHb) >= GW_HEARTBEAT_INTERVAL_MS) {
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

uint32_t gw_logic_next_wait_ms(void) {
  uint32_t now = hal_millis();
  uint32_t best = GW_LOGIC_MAX_WAIT_MS;
  auto consider = [&](uint32_t at) {
    uint32_t d = reached(now, at) ? 0 : (uint32_t)(at - now);
    if (d < best) best = d;
  };
  if (s_cmdPending) consider(s_pSentAt + radioAckTimeoutCmdMs());
  if (s_stateReqPending) consider(s_sSentAt + radioStateReqTimeoutMs());
  consider(s_lastHb + GW_HEARTBEAT_INTERVAL_MS);
  return best;
}

void gw_logic_task(void *) {
  GwMsg m;
  esp_task_wdt_add(nullptr);
  for (;;) {
    HAL_STACK_PROBE();
    uint32_t w = gw_logic_next_wait_ms();
    if (xQueueReceive(s_gwInQ, &m, pdMS_TO_TICKS(w)) == pdTRUE) gw_logic_handle(m);
    gw_logic_tick();
    esp_task_wdt_reset();                        // признак на живот към Task Watchdog
  }
}
