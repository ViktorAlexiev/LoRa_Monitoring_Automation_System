#include "lora_handlers.h"
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include "packets.h"
#include "crypto_common.h"
#include "config_storage.h"
#include "wifi_mqtt.h"
#include "cad.h"

void lora_radio_setup() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);

  unsigned long lastMsg = 0;
  while (!LoRa.begin(LORA_FREQ_HZ)) {
    if (millis() - lastMsg >= LORA_BEGIN_RETRY_MSG_MS) {
      Serial.println("[ERR] LoRa не стартира (LoRa.begin провал), продължавам да опитвам...");
      lastMsg = millis();
    }
    delay(50);
  }

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH_HZ);
  LoRa.setCodingRate4(LORA_CR_DENOM);
  LoRa.setTxPower(LORA_TX_POWER_DBM);
  LoRa.setPreambleLength(LORA_PREAMBLE_LEN);
  LoRa.setSyncWord(LORA_SYNC_WORD);
}

// Проста CAD-защита преди TX - кратък retry прозорец вместо неопределено чакане.
static bool waitForClearChannel() {
  uint8_t attempts = 0;
  while (channelActive() && attempts < 5) {
    delay(random(5, 20));
    attempts++;
  }
  return !channelActive();
}

// ---------- Sensor/executor wire дължини (криптирани - виж crypto_common.h) ----------
#define SENSOR_PT_LEN     16   // 4 float-а (S_T,S_H,A_T,A_H)
#define SENSOR_WIRE_LEN   (CRYPTO_OVERHEAD + SENSOR_PT_LEN)      // 30
#define HB_WIRE_LEN       (CRYPTO_OVERHEAD)                       // 14 (без данни)
#define ACK_PT_LEN        6    // C_ID(4)+com(1)+status(1)
#define ACK_WIRE_LEN      (CRYPTO_OVERHEAD + ACK_PT_LEN)          // 20
#define CMD_PLAINTEXT_LEN 6    // C_ID(4)+com(1)+status(1)
#define CMD_WIRE_LEN      (MODULE_ID_LEN + CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN)  // 26 (target+wire)
// State request и state-resp ACK: target M_ID(чисто) + празен ciphertext wire - еднаква
// дължина, разграничават се по type-id при decrypt (не по дължина).
#define STATE_REQ_WIRE_LEN       (MODULE_ID_LEN + CRYPTO_OVERHEAD)   // 20 (Gateway->Executor, не се приема тук)
// State response (Executor->Gateway, нормален или RESTART): senderId+ciphertext(N*5)+counter+tag,
// N = брой консуматори (0..10) - variable дължина, разпознава се по (len-OVERHEAD) % 5 == 0.
#define STATE_ENTRY_LEN            5    // C_ID(4)+state(1), на консуматор
#define STATE_MAX_CONSUMERS        10   // трябва да съвпада с executor/config_storage.h MAX_CONSUMERS

// ---------- Dedup буфер на sensor пакети (пази от дублиране при 2+ repeater-и) ----------
// Пази суровите radio байтове (ciphertext+counter+tag, различни при всяка трансмисия
// благодарение на nonce-а) - ring buffer, без timestamp: най-старият запис просто се
// презаписва при нов. Сравнява само първите SENSOR_WIRE_LEN байта - директен (30 B) и
// препратен от Repeater (31 B, с trailing REPEATED_MARKER) вариант на едно и също
// измерване се разпознават като дубликат независимо от trailing маркера.
#define GW_DEDUP_BUFFER_SIZE  8

static uint8_t gwDedupBuf[GW_DEDUP_BUFFER_SIZE][SENSOR_WIRE_LEN];
static bool    gwDedupUsed[GW_DEDUP_BUFFER_SIZE] = {false};
static uint8_t gwDedupHead = 0;

static bool gwDedupSeen(const uint8_t *data) {
  for (uint8_t i = 0; i < GW_DEDUP_BUFFER_SIZE; i++) {
    if (gwDedupUsed[i] && memcmp(gwDedupBuf[i], data, SENSOR_WIRE_LEN) == 0) return true;
  }
  return false;
}

static void gwDedupAdd(const uint8_t *data) {
  memcpy(gwDedupBuf[gwDedupHead], data, SENSOR_WIRE_LEN);
  gwDedupUsed[gwDedupHead] = true;
  gwDedupHead = (gwDedupHead + 1) % GW_DEDUP_BUFFER_SIZE;
}

// ---------- Команда "в полет" ----------
static bool          cmdPending   = false;
static CommandPacket pendingCmd;
static unsigned long pendingSentAt = 0;
static uint8_t       pendingRetries = 0;

// ---------- State request "в полет" ----------
static bool          stateReqPending = false;
static char          stateReqM_ID[MODULE_ID_LEN + 1] = {0};
static unsigned long stateReqSentAt = 0;
static uint8_t       stateReqRetries = 0;

bool lora_command_pending() { return cmdPending; }
bool lora_state_request_pending() { return stateReqPending; }

// ---------------- Публикуване на командeн статус ----------------
static void publishCommandStatus(const CommandPacket& p, uint8_t status) {
  char m_id[MODULE_ID_LEN + 1] = {0}; memcpy(m_id, p.M_ID, MODULE_ID_LEN);
  char c_id[5] = {0}; memcpy(c_id, p.C_ID, 4);

  StaticJsonDocument<160> doc;
  doc["M_ID"]   = m_id;
  doc["C_ID"]   = c_id;
  char comHex[3];
  snprintf(comHex, sizeof(comHex), "%02X", p.com);
  doc["com"]    = comHex;
  doc["status"] = status;

  publishJson(TOPIC_COMMANDS_STATUS, doc);

  Serial.print("[STATUS] "); Serial.print(m_id); Serial.print("/"); Serial.print(c_id);
  Serial.print(" com="); Serial.print(comHex);
  Serial.print(" status="); Serial.println(status);
}

// Команда (криптирана): [target M_ID, чисто] + [wire: GATEWAY_ID(sender)+ciphertext(C_ID+com+status)+counter+tag]
// ЕДИН общ nonce поток за командите към ВСИЧКИ executor-и (не per-target) - виж cmdCeilingNext().
static void sendCommandPacket(const CommandPacket& p) {
  uint8_t plaintext[CMD_PLAINTEXT_LEN];
  memcpy(plaintext, p.C_ID, 4);
  plaintext[4] = p.com;
  plaintext[5] = p.status;

  uint8_t wireBuf[CMD_WIRE_LEN];
  memcpy(wireBuf, p.M_ID, MODULE_ID_LEN);   // target, чисто - executor-ът филтрира по това ПРЕДИ decrypt

  cryptoBuildWirePacket(wireBuf + MODULE_ID_LEN, NETWORK_KEY, CRYPTO_TYPE_GW_CMD,
                         (const uint8_t*)GATEWAY_ID, cmdCeilingNext(), plaintext, sizeof(plaintext));

  if (!waitForClearChannel()) {
    Serial.println("[TX CMD] канала зает, пропускам този опит (следващ retry цикъл ще опита пак)");
    return;
  }

  LoRa.beginPacket();
  LoRa.write(wireBuf, sizeof(wireBuf));
  LoRa.endPacket();
  LoRa.receive();
}

// State request (криптирана): [target M_ID, чисто] + [wire: GATEWAY_ID+ciphertext(0)+counter+tag]
static void sendStateRequestPacket(const char* m_id) {
  uint8_t wireBuf[STATE_REQ_WIRE_LEN];
  memset(wireBuf, 0, MODULE_ID_LEN);
  strncpy((char*)wireBuf, m_id, MODULE_ID_LEN);

  cryptoBuildWirePacket(wireBuf + MODULE_ID_LEN, NETWORK_KEY, CRYPTO_TYPE_STATE_REQ,
                         (const uint8_t*)GATEWAY_ID, cmdCeilingNext(), NULL, 0);

  if (!waitForClearChannel()) {
    Serial.println("[TX STATE_REQ] канала зает, пропускам този опит");
    return;
  }

  LoRa.beginPacket();
  LoRa.write(wireBuf, sizeof(wireBuf));
  LoRa.endPacket();
  LoRa.receive();
}

// State-resp ACK (само за RESTART варианта): [target M_ID, чисто] + [wire: GATEWAY_ID+ciphertext(0)+counter+tag]
static void sendStateRespAck(const uint8_t* targetM_ID6) {
  uint8_t wireBuf[STATE_REQ_WIRE_LEN];
  memcpy(wireBuf, targetM_ID6, MODULE_ID_LEN);

  cryptoBuildWirePacket(wireBuf + MODULE_ID_LEN, NETWORK_KEY, CRYPTO_TYPE_STATE_RESP_ACK,
                         (const uint8_t*)GATEWAY_ID, cmdCeilingNext(), NULL, 0);

  if (!waitForClearChannel()) {
    Serial.println("[TX STATE_RESP_ACK] канала зает, пропускам (Executor ще retry-ва)");
    return;
  }

  LoRa.beginPacket();
  LoRa.write(wireBuf, sizeof(wireBuf));
  LoRa.endPacket();
  LoRa.receive();

  Serial.println("[TX STATE_RESP_ACK] изпратен");
}

void lora_send_command(const char* m_id, const char* c_id, uint8_t com) {
  CommandPacket p = {0};
  strncpy(p.M_ID, m_id, MODULE_ID_LEN);
  strncpy(p.C_ID, c_id, 4);
  p.com    = com;
  p.status = 0xFF;

  pendingCmd     = p;
  pendingSentAt  = millis();
  pendingRetries = 0;
  cmdPending     = true;

  sendCommandPacket(p);
  Serial.print("[TX CMD] "); Serial.print(m_id); Serial.print("/"); Serial.println(c_id);
}

void lora_send_state_request(const char* m_id) {
  memset(stateReqM_ID, 0, MODULE_ID_LEN + 1);
  strncpy(stateReqM_ID, m_id, MODULE_ID_LEN);
  stateReqSentAt  = millis();
  stateReqRetries = 0;
  stateReqPending = true;

  sendStateRequestPacket(stateReqM_ID);
  Serial.print("[TX STATE_REQ] "); Serial.println(stateReqM_ID);
}

// ---------------- MQTT callback (обявена в wifi_mqtt.h, регистрирана там) ----------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_COMMANDS) == 0) {
    if (cmdPending) {
      Serial.println("[WARN] команда вече чака - нова игнорирана");
      return;
    }
    StaticJsonDocument<160> doc;
    if (deserializeJson(doc, payload, length)) { Serial.println("[ERR] Bad JSON commands"); return; }

    const char* m_id = doc["M_ID"] | "";
    const char* c_id = doc["C_ID"] | "";
    const char* comStr = doc["com"] | "";

    lora_send_command(m_id, c_id, (uint8_t)strtol(comStr, nullptr, 16));
  }
  else if (strcmp(topic, TOPIC_STATE_REQ) == 0) {
    if (stateReqPending) {
      Serial.println("[WARN] state request вече чака - нова игнорирана");
      return;
    }
    StaticJsonDocument<96> doc;
    if (deserializeJson(doc, payload, length)) { Serial.println("[ERR] Bad JSON state req"); return; }

    const char* m_id = doc["M_ID"] | "";
    if (strlen(m_id) == 0) return;

    lora_send_state_request(m_id);
  }
}

// ---------------- ACK/retry мениджъри ----------------
static void commandAckManager() {
  if (!cmdPending) return;
  if (millis() - pendingSentAt >= ACK_TIMEOUT_MS) {
    if (pendingRetries < MAX_RETRIES) {
      pendingRetries++;
      pendingSentAt = millis();
      sendCommandPacket(pendingCmd);
      Serial.print("[RETRY CMD] опит #"); Serial.println(pendingRetries);
    } else {
      publishCommandStatus(pendingCmd, STATUS_TIMEOUT);
      cmdPending = false;
    }
  }
}

static void stateRequestManager() {
  if (!stateReqPending) return;
  if (millis() - stateReqSentAt >= ACK_TIMEOUT_MS) {
    if (stateReqRetries < MAX_RETRIES) {
      stateReqRetries++;
      stateReqSentAt = millis();
      sendStateRequestPacket(stateReqM_ID);
      Serial.print("[RETRY STATE_REQ] опит #"); Serial.println(stateReqRetries);
    } else {
      StaticJsonDocument<96> doc;
      doc["id"] = stateReqM_ID;
      doc["status"] = "timeout";
      publishJson(TOPIC_STATE_RESP, doc);
      stateReqPending = false;
      Serial.println("[STATE_REQ] timeout");
    }
  }
}

void lora_managers_tick() {
  commandAckManager();
  stateRequestManager();
}

// ---------------- Обработка на state response (нормален или RESTART вариант) ----------------
// В MQTT двата варианта изглеждат абсолютно еднакво (без маркер) - разликата е само, че
// RESTART вариантът получава ACK обратно по радиото и НЕ е задължително отговор на изрична
// заявка (може да няма stateReqPending в момента на получаването му).
static void handleStateResponse(const uint8_t* senderId, const uint8_t* plaintext, uint8_t ptLen, bool isRestart) {
  char m_id[CRYPTO_ID_LEN + 1] = {0};
  memcpy(m_id, senderId, CRYPTO_ID_LEN);

  int entries = ptLen / STATE_ENTRY_LEN;

  StaticJsonDocument<1024> doc;
  doc["id"] = m_id;
  JsonArray states = doc.createNestedArray("states");
  for (int i = 0; i < entries; i++) {
    int off = i * STATE_ENTRY_LEN;
    char c_id[5] = {0};
    memcpy(c_id, plaintext + off, 4);
    uint8_t st = plaintext[off + 4];

    JsonObject o = states.createNestedObject();
    o["id"] = c_id;
    o["state"] = st ? "ON" : "OFF";
  }

  publishJson(TOPIC_STATE_RESP, doc);

  if (isRestart) {
    sendStateRespAck(senderId);
  } else if (stateReqPending && strncmp(m_id, stateReqM_ID, MODULE_ID_LEN) == 0) {
    stateReqPending = false;
  }

  Serial.print(isRestart ? "[STATE_RESP RESTART] " : "[STATE_RESP] ");
  Serial.print(m_id);
  Serial.print(" entries="); Serial.println(entries);
}

// ---------------- LoRa пакет -> обработка ----------------
void lora_handle_incoming(int packetSize) {
  uint8_t buf[220];
  int len = 0;
  while (LoRa.available() && len < (int)sizeof(buf)) {
    buf[len++] = (uint8_t)LoRa.read();
  }

  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();
  uint32_t ts = (uint32_t)(millis() / 1000);

  // sizeof(SensorPacket wire) - директно от sensor; +1 - препратен от repeater (trailing
  // REPEATED_MARKER, не участва в декриптирането - dedup сравнява само първите
  // SENSOR_WIRE_LEN байта, независимо дали маркерът присъства).
  if (len == SENSOR_WIRE_LEN || len == SENSOR_WIRE_LEN + 1) {
    if (gwDedupSeen(buf)) {
      Serial.println("  -> дублиран sensor пакет (вече видян), игнориран");
      return;
    }

    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[SENSOR_PT_LEN];
    uint8_t ptLen;
    if (!cryptoParseWirePacket(buf, SENSOR_WIRE_LEN, NETWORK_KEY, CRYPTO_TYPE_SENSOR,
                                senderId, plaintext, &ptLen)) {
      Serial.println("  -> sensor пакет: невалиден MAC, отхвърлен");
      return;
    }
    gwDedupAdd(buf);

    float s_t, s_h, a_t, a_h;
    memcpy(&s_t, plaintext + 0, 4);
    memcpy(&s_h, plaintext + 4, 4);
    memcpy(&a_t, plaintext + 8, 4);
    memcpy(&a_h, plaintext + 12, 4);

    char id[CRYPTO_ID_LEN + 1] = {0};
    memcpy(id, senderId, CRYPTO_ID_LEN);

    StaticJsonDocument<256> doc;
    doc["id"] = id; doc["soil_t"] = s_t; doc["soil_h"] = s_h;
    doc["air_t"] = a_t; doc["air_h"] = a_h;
    doc["rssi"] = rssi; doc["snr"] = snr; doc["ts"] = ts;

    publishJson(TOPIC_SENSORS, doc);
  }
  else if (len == ACK_WIRE_LEN) {
    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[ACK_PT_LEN];
    uint8_t ptLen;
    if (!cryptoParseWirePacket(buf, len, NETWORK_KEY, CRYPTO_TYPE_EXEC_ACK, senderId, plaintext, &ptLen)) {
      Serial.println("  -> ACK: невалиден MAC, отхвърлен");
      return;
    }

    CommandPacket p;
    memset(p.M_ID, 0, MODULE_ID_LEN);
    memcpy(p.M_ID, senderId, CRYPTO_ID_LEN);
    memcpy(p.C_ID, plaintext, 4);
    p.com    = plaintext[4];
    p.status = plaintext[5];

    if (cmdPending &&
        memcmp(p.M_ID, pendingCmd.M_ID, MODULE_ID_LEN) == 0 &&
        memcmp(p.C_ID, pendingCmd.C_ID, 4) == 0 &&
        p.com == pendingCmd.com) {
      uint8_t status = (p.status == 0) ? STATUS_ACK : STATUS_NACK;
      publishCommandStatus(p, status);
      cmdPending = false;
    }
  }
  // HB от repeater, HB от executor, и state response с 0 консуматора (ptLen=0) имат
  // еднаква wire дължина (14, без данни) - "типа" не пътува по въздуха, пробваме поред.
  else if (len == HB_WIRE_LEN) {
    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[1];
    uint8_t ptLen;

    if (cryptoParseWirePacket(buf, len, NETWORK_KEY, CRYPTO_TYPE_REPEATER_HB, senderId, plaintext, &ptLen)) {
      char id[CRYPTO_ID_LEN + 1] = {0};
      memcpy(id, senderId, CRYPTO_ID_LEN);
      StaticJsonDocument<128> doc;
      doc["id"] = id; doc["rssi"] = rssi; doc["snr"] = snr; doc["ts"] = ts;
      publishJson(TOPIC_HEARTBEAT, doc);
      Serial.print(F("[HB REPEATER] ")); Serial.println(id);
    }
    else if (cryptoParseWirePacket(buf, len, NETWORK_KEY, CRYPTO_TYPE_EXEC_HB, senderId, plaintext, &ptLen)) {
      char id[CRYPTO_ID_LEN + 1] = {0};
      memcpy(id, senderId, CRYPTO_ID_LEN);
      StaticJsonDocument<128> doc;
      doc["id"] = id; doc["ts"] = ts;
      publishJson(TOPIC_HEARTBEAT, doc);
      Serial.print(F("[HB EXEC] ")); Serial.println(id);
    }
    else if (cryptoParseWirePacket(buf, len, NETWORK_KEY, CRYPTO_TYPE_STATE_RESP, senderId, plaintext, &ptLen)) {
      handleStateResponse(senderId, plaintext, ptLen, false);
    }
    else if (cryptoParseWirePacket(buf, len, NETWORK_KEY, CRYPTO_TYPE_STATE_RESP_RESTART, senderId, plaintext, &ptLen)) {
      handleStateResponse(senderId, plaintext, ptLen, true);
    }
    else {
      Serial.println("  -> HB/STATE(0) пакет: невалиден MAC, отхвърлен");
    }
  }
  // State response с 1+ консуматор - разпознава се по (len - CRYPTO_OVERHEAD) кратно на
  // STATE_ENTRY_LEN(5). Не съвпада числено с нито един друг тип пакет (ACK ptLen=6,
  // SENSOR ptLen=16 - нито едно от тях не е кратно на 5).
  else if (len > CRYPTO_OVERHEAD &&
           (len - CRYPTO_OVERHEAD) % STATE_ENTRY_LEN == 0 &&
           (len - CRYPTO_OVERHEAD) <= STATE_MAX_CONSUMERS * STATE_ENTRY_LEN) {
    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[STATE_MAX_CONSUMERS * STATE_ENTRY_LEN];
    uint8_t ptLen;

    if (cryptoParseWirePacket(buf, len, NETWORK_KEY, CRYPTO_TYPE_STATE_RESP, senderId, plaintext, &ptLen)) {
      handleStateResponse(senderId, plaintext, ptLen, false);
    }
    else if (cryptoParseWirePacket(buf, len, NETWORK_KEY, CRYPTO_TYPE_STATE_RESP_RESTART, senderId, plaintext, &ptLen)) {
      handleStateResponse(senderId, plaintext, ptLen, true);
    }
    else {
      Serial.println("  -> STATE_RESP: невалиден MAC, отхвърлен");
    }
  }
  else {
    Serial.println("  -> UNKNOWN length, ignored");
  }
}
