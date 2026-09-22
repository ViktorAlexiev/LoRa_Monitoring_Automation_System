#include "radio_io.h"
#include <LoRa.h>
#include "packets.h"
#include "crypto_common.h"
#include "config_storage.h"
#include "consumers.h"
#include "queues.h"
#include "power_mgmt.h"
#include "cad.h"

// Downlink wire дължини (target M_ID, чисто, MODULE_ID_LEN bytes + crypto wire):
#define CMD_PLAINTEXT_LEN  6   // C_ID(4) + com(1) + status(1)
#define CMD_WIRE_LEN  (MODULE_ID_LEN + CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN)        // 26
// State request и state-resp ACK имат еднакъв wire формат (target M_ID + празен
// ciphertext wire) - разграничават се само по type-id при decrypt, не по дължина.
#define STATE_REQ_WIRE_LEN       (MODULE_ID_LEN + CRYPTO_OVERHEAD)                 // 20
#define STATE_RESP_ACK_WIRE_LEN  (MODULE_ID_LEN + CRYPTO_OVERHEAD)                 // 20 (същата стойност)
#define STATE_ENTRY_LEN  5      // C_ID(4) + state(1), на консуматор в state response plaintext

// Проста CAD-защита преди TX - кратък retry прозорец вместо неопределено чакане (пести
// енергия и не блокира устройството задълго при постоянно зает канал).
static bool waitForClearChannel() {
  uint8_t attempts = 0;
  while (channelActive() && attempts < 5) {
    delay(random(5, 20));
    attempts++;
  }
  return !channelActive();
}

void radio_setup() {
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  unsigned long lastMsg = 0;
  while (!LoRa.begin(LORA_FREQ_HZ)) {
    if (millis() - lastMsg >= LORA_BEGIN_RETRY_MSG_MS) {
      Serial.println(F("[ERR] LoRa не стартира (LoRa.begin провал), продължавам да опитвам..."));
      lastMsg = millis();
    }
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH_HZ);
  LoRa.setCodingRate4(LORA_CR_DENOM);
  LoRa.setTxPower(LORA_TX_POWER_DBM);
  LoRa.setPreambleLength(LORA_PREAMBLE_LEN);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.receive();
}

// ---------------- Строи и праща криптиран state response (нормален или RESTART) ----------------
static void sendStateResponseWire(uint8_t typeId) {
  uint8_t plaintext[MAX_CONSUMERS * STATE_ENTRY_LEN];
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    uint8_t off = i * STATE_ENTRY_LEN;
    memcpy(plaintext + off, consumerList[i], 4);
    plaintext[off + 4] = (digitalRead(consumerPins[i]) == HIGH) ? 1 : 0;
  }
  uint8_t ptLen = NUM_CONSUMERS * STATE_ENTRY_LEN;

  uint8_t wireBuf[CRYPTO_OVERHEAD + MAX_CONSUMERS * STATE_ENTRY_LEN];
  uint8_t wireLen = cryptoBuildWirePacket(wireBuf, NETWORK_KEY, typeId,
                                           (const uint8_t*)MY_M_ID, txCounter.next(),
                                           plaintext, ptLen);

  if (!waitForClearChannel()) {
    Serial.println(F("[STATE_RESP] канала зает, пропускам този опит"));
    return;
  }

  LoRa.beginPacket();
  LoRa.write(wireBuf, wireLen);
  LoRa.endPacket();
  LoRa.receive();

  Serial.print(F("[TX STATE_RESP")); Serial.print(typeId == CRYPTO_TYPE_STATE_RESP_RESTART ? F(" RESTART") : F(""));
  Serial.print(F("] M_ID=")); Serial.print(MY_M_ID);
  Serial.print(F(" consumers=")); Serial.println(NUM_CONSUMERS);
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    uint8_t state = (digitalRead(consumerPins[i]) == HIGH) ? 1 : 0;
    Serial.print(F("   ")); Serial.print(consumerList[i]);
    Serial.println(state ? F(" = ON") : F(" = OFF"));
  }
}

// ---------------- Отговор на изрична заявка от Gateway (без ACK очакване) ----------------
static void sendStateResponse() {
  sendStateResponseWire(CRYPTO_TYPE_STATE_RESP);
}

// ---------------- Restart state response - ACK/retry state machine ----------------
#define STATE_RESP_ACK_TIMEOUT_MS 4000UL   // същия интервал като retry-я на Gateway
#define STATE_RESP_MAX_RETRIES    2         // общо до 3 опита (1 + 2 retry-я)

static bool          restartRespPending  = false;
static unsigned long restartRespSentAt   = 0;
static uint8_t        restartRespRetries = 0;

void sendRestartStateResponse() {
  restartRespPending  = true;
  restartRespSentAt   = millis();
  restartRespRetries  = 0;
  sendStateResponseWire(CRYPTO_TYPE_STATE_RESP_RESTART);
}

void restartRespManager() {
  if (!restartRespPending) return;
  if (millis() - restartRespSentAt >= STATE_RESP_ACK_TIMEOUT_MS) {
    if (restartRespRetries < STATE_RESP_MAX_RETRIES) {
      restartRespRetries++;
      restartRespSentAt = millis();
      Serial.print(F("[RETRY STATE_RESP RESTART] опит #")); Serial.println(restartRespRetries);
      sendStateResponseWire(CRYPTO_TYPE_STATE_RESP_RESTART);
    } else {
      Serial.println(F("[STATE_RESP RESTART] няма ACK след 3 опита, отказвам се, продължавам нормално"));
      restartRespPending = false;
    }
  }
}

bool restart_response_pending() {
  return restartRespPending;
}

// ---------------- Heartbeat (криптиран, без данни - само идентичност+counter+MAC) ----------------
static bool hbPending = false;
static unsigned long hbSendAt = 0;

static void sendHeartbeatNow() {
  uint8_t wireBuf[CRYPTO_OVERHEAD];
  uint8_t wireLen = cryptoBuildWirePacket(wireBuf, NETWORK_KEY, CRYPTO_TYPE_EXEC_HB,
                                           (const uint8_t*)MY_M_ID, txCounter.next(),
                                           NULL, 0);

  if (!waitForClearChannel()) {
    Serial.println(F("[HB] канала зает, пропускам този цикъл"));
    return;
  }

  LoRa.beginPacket();
  LoRa.write(wireBuf, wireLen);
  LoRa.endPacket();
  LoRa.receive();

  Serial.print(F("[TX HB] M_ID=")); Serial.println(MY_M_ID);
}

void heartbeat_tick() {
  if (!hbPending && wdt_ticks >= HB_INTERVAL_CYCLES) {
    wdt_ticks -= HB_INTERVAL_CYCLES;
    hbPending = true;
    hbSendAt = millis() + random(0, JITTER_MAX_MS);
  }
  if (hbPending && millis() >= hbSendAt) {
    hbPending = false;
    sendHeartbeatNow();
  }
}

bool heartbeat_pending() {
  return hbPending;
}

// ---------------- RADIO RECEIVE POLL ----------------
void radioReceivePoll() {
  int packetSize = LoRa.parsePacket();

  if (packetSize == CMD_WIRE_LEN) {
    uint8_t buf[CMD_WIRE_LEN];
    LoRa.readBytes(buf, CMD_WIRE_LEN);

    // target проверка ПРЕДИ decrypt - евтино, отхвърля пакети за други executor-и веднага
    if (memcmp(buf, MY_M_ID, MODULE_ID_LEN) != 0) {
      Serial.println(F("[RX CMD] не е за мен, игнориран"));
      return;
    }

    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[CMD_PLAINTEXT_LEN];
    uint8_t ptLen;
    if (!cryptoParseWirePacket(buf + MODULE_ID_LEN, CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN,
                                NETWORK_KEY, CRYPTO_TYPE_GW_CMD, senderId, plaintext, &ptLen)) {
      Serial.println(F("[RX CMD] невалиден MAC, отхвърлен"));
      return;
    }

    CommandPacket p;
    memcpy(p.C_ID, plaintext, 4);
    p.com    = plaintext[4];
    p.status = plaintext[5];

    char c_id[5] = {0}; memcpy(c_id, p.C_ID, 4);
    Serial.print(F("[RX CMD] C_ID=")); Serial.print(c_id);
    Serial.print(F(" com=0x")); Serial.println(p.com, HEX);

    rxPush(p);
  }
  else if (packetSize == STATE_REQ_WIRE_LEN) {
    uint8_t buf[STATE_REQ_WIRE_LEN];
    LoRa.readBytes(buf, STATE_REQ_WIRE_LEN);

    // target проверка ПРЕДИ decrypt - общ формат за state request и state-resp ACK
    if (memcmp(buf, MY_M_ID, MODULE_ID_LEN) != 0) {
      Serial.println(F("[RX STATE] не е за мен, игнориран"));
      return;
    }

    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[1];
    uint8_t ptLen;

    if (cryptoParseWirePacket(buf + MODULE_ID_LEN, CRYPTO_OVERHEAD, NETWORK_KEY,
                               CRYPTO_TYPE_STATE_REQ, senderId, plaintext, &ptLen)) {
      Serial.println(F("[RX STATE_REQ] за мен, изпращам състояния"));
      sendStateResponse();
    }
    else if (cryptoParseWirePacket(buf + MODULE_ID_LEN, CRYPTO_OVERHEAD, NETWORK_KEY,
                                    CRYPTO_TYPE_STATE_RESP_ACK, senderId, plaintext, &ptLen)) {
      if (restartRespPending) {
        Serial.println(F("[RX STATE_RESP_ACK] потвърдено от Gateway, спирам retry-тата"));
        restartRespPending = false;
      }
    }
    else {
      Serial.println(F("[RX STATE] невалиден MAC, отхвърлен"));
    }
  }
  else if (packetSize > 0) {
    Serial.print(F("[RX] непознат размер=")); Serial.println(packetSize);
    while (LoRa.available()) LoRa.read();
  }
}

void processRx() {
  CommandPacket p;
  while (rxPop(&p)) {
    char c_id[5] = {0};
    memcpy(c_id, p.C_ID, 4);

    int idx = consumerIndex(p.C_ID);
    bool cmdOk = (p.com == CMD_ON || p.com == CMD_OFF);

    if (idx >= 0 && cmdOk) {
      digitalWrite(consumerPins[idx], p.com == CMD_ON ? HIGH : LOW);
      Serial.print(F("[EXEC] "));
      Serial.print(c_id);
      Serial.print(F(" (pin "));
      Serial.print(consumerPins[idx]);
      Serial.println(p.com == CMD_ON ? F(") -> ON") : F(") -> LOW"));
      p.status = STATUS_ACK;
    } else {
      Serial.print(F("[REJECT] "));
      Serial.print(c_id);
      if (idx < 0) Serial.print(F(" - консуматор не съществува"));
      if (!cmdOk)  Serial.print(F(" - невалидна команда"));
      Serial.println();
      p.status = STATUS_NACK;
    }
    ackEnqueue(p, millis() + 500);
  }
}

void ackManager() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < ackCount; ) {
    if (now >= ackSendAt[i]) {
      char c_id[5] = {0}; memcpy(c_id, ackQueue[i].C_ID, 4);

      uint8_t plaintext[CMD_PLAINTEXT_LEN];
      memcpy(plaintext, ackQueue[i].C_ID, 4);
      plaintext[4] = ackQueue[i].com;
      plaintext[5] = ackQueue[i].status;

      uint8_t wireBuf[CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN];
      uint8_t wireLen = cryptoBuildWirePacket(wireBuf, NETWORK_KEY, CRYPTO_TYPE_EXEC_ACK,
                                               (const uint8_t*)MY_M_ID, txCounter.next(),
                                               plaintext, sizeof(plaintext));

      if (!waitForClearChannel()) {
        Serial.print(F("[ACK/NACK] канала зает, отлагам за следващия loop, C_ID="));
        Serial.println(c_id);
        i++;
        continue;
      }

      LoRa.beginPacket();
      LoRa.write(wireBuf, wireLen);
      LoRa.endPacket();
      LoRa.receive();

      Serial.print(F("[TX "));
      Serial.print(ackQueue[i].status == STATUS_ACK ? F("ACK] ") : F("NACK] "));
      Serial.println(c_id);

      ackRemoveAt(i);
    } else i++;
  }
}
