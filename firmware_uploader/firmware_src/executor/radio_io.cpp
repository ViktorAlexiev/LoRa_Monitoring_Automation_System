#include "radio_io.h"
#include <LoRa.h>
#include "packets.h"
#include "crypto_common.h"
#include "config_storage.h"
#include "consumers.h"
#include "queues.h"
#include "power_mgmt.h"
#include "cad.h"
#include "channel_access.h"

// Downlink wire дължини (target M_ID, чисто, MODULE_ID_LEN bytes + crypto wire):
#define CMD_PLAINTEXT_LEN  6   // C_ID(4) + com(1) + status(1)
#define CMD_WIRE_LEN  (MODULE_ID_LEN + CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN)        // 26
// State request и state-resp ACK имат еднакъв wire формат (target M_ID + празен
// ciphertext wire) - разграничават се само по type-id при decrypt, не по дължина.
#define STATE_REQ_WIRE_LEN       (MODULE_ID_LEN + CRYPTO_OVERHEAD)                 // 20
#define STATE_RESP_ACK_WIRE_LEN  (MODULE_ID_LEN + CRYPTO_OVERHEAD)                 // 20 (същата стойност)
#define STATE_ENTRY_LEN  5      // C_ID(4) + state(1), на консуматор в state response plaintext

void radio_setup() {
  // времената (CAD, ACK timeout-и) се извеждат от SF/BW, прочетени от EEPROM
  radioTimingInit(LORA_SF, LORA_BW_HZ, LORA_CR_DENOM, LORA_PREAMBLE_LEN, true);

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  unsigned long lastMsg = 0;
  while (!LoRa.begin(LORA_FREQ_HZ)) {
    if (millis() - lastMsg >= LORA_BEGIN_RETRY_MSG_MS) {
      Serial.println(F("[ERR] LoRa не стартира (LoRa.begin провал), продължавам да опитвам..."));
      lastMsg = millis();
    }
  }
  radioApplyModemSettings();   // SF, BW, CR, преамбюл + LDRO (по реалното Ts)
  LoRa.setTxPower(LORA_TX_POWER_DBM);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.receive();
}

// ---------------- Общ TX слот с неблокиращ достъп до канала ----------------
// Един пакет наведнъж. Пакетът се строи (и получава counter) при заявката; достъпът до канала
// (CAD + сондиране + backoff) тече на малки стъпки в radioTxTick() и НЕ блокира приемането
// на входящи пакети. Команден път (ACK/NACK, state response, restart response): политика
// COMMAND. Heartbeat: телеметрия, при зает канал се пропуска.
#define TXK_ACK           1
#define TXK_STATE_RESP    2
#define TXK_RESTART_RESP  3
#define TXK_HB            4

static bool          txPending = false;
static uint8_t       txKind = 0;
static uint8_t       txBuf[CRYPTO_OVERHEAD + MAX_CONSUMERS * STATE_ENTRY_LEN];
static uint8_t       txLen = 0;
static char          txTag[CONSUMER_ID_LEN + 1];   // C_ID на ACK-а, само за лог
static uint8_t       txStatus = 0;
static unsigned long txCreatedAt = 0;
static ChannelAccess txAccess;

static bool txRequest(uint8_t kind, const uint8_t *buf, uint8_t len, uint8_t policy) {
  if (txPending) return false;
  memcpy(txBuf, buf, len);
  txLen = len;
  txKind = kind;
  txCreatedAt = millis();
  channelAccessStart(&txAccess, policy);
  txPending = true;
  return true;
}

bool radio_tx_busy() {
  return txPending;
}

// CAD оставя радиото в standby - помни дали на този ход е бил изпълнен, за да върнем радиото
// на слушане само тогава (без излишни записи в регистрите на всяка обиколка).
static bool cadRan = false;
static bool cadProbe() {
  cadRan = true;
  return channelActive();
}

void radioTxTick() {
  if (!txPending) return;

  cadRan = false;
  uint8_t r = channelAccessPoll(&txAccess, millis(), cadProbe, channelRandom);
  if (r == CH_WAIT) {
    if (cadRan) LoRa.receive();   // между проверките слушаме
    return;
  }

  if (r == CH_CLEAR) {
    LoRa.beginPacket();
    LoRa.write(txBuf, txLen);
    LoRa.endPacket();
    LoRa.receive();

    switch (txKind) {
      case TXK_ACK:
        Serial.print(F("[TX ")); Serial.print(txStatus == STATUS_ACK ? F("ACK] ") : F("NACK] "));
        Serial.println(txTag);
        break;
      case TXK_STATE_RESP:
        Serial.println(F("[TX STATE_RESP] изпратен"));
        break;
      case TXK_RESTART_RESP:
        Serial.println(F("[TX STATE_RESP RESTART] изпратен"));
        break;
      case TXK_HB:
        Serial.print(F("[TX HB] M_ID=")); Serial.println(MY_M_ID);
        break;
    }
    txPending = false;
    return;
  }

  // CH_GIVEUP - срокът за сондиране изтече, каналът остана зает
  LoRa.receive();
  if (txKind == TXK_ACK && (millis() - txCreatedAt) < radioAckTimeoutCmdMs()) {
    // ACK-ът още има смисъл (Gateway още чака) - започни нов цикъл на достъп
    Serial.print(F("[ACK/NACK] канала зает, опитвам пак, C_ID=")); Serial.println(txTag);
    channelAccessStart(&txAccess, CH_POLICY_COMMAND);
    return;
  }
  Serial.print(F("[TX] канала остана зает, отказвам се (вид=")); Serial.print((int)txKind); Serial.println(F(")"));
  txPending = false;
}

// ---------------- Строи и заявява криптиран state response (нормален или RESTART) ----------------
static bool sendStateResponseWire(uint8_t typeId) {
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

  uint8_t kind = (typeId == CRYPTO_TYPE_STATE_RESP_RESTART) ? TXK_RESTART_RESP : TXK_STATE_RESP;
  if (!txRequest(kind, wireBuf, wireLen, CH_POLICY_COMMAND)) {
    Serial.println(F("[STATE_RESP] TX слотът е зает, пропускам"));
    return false;
  }

  Serial.print(F("[STATE_RESP")); Serial.print(kind == TXK_RESTART_RESP ? F(" RESTART") : F(""));
  Serial.print(F("] заявен, M_ID=")); Serial.print(MY_M_ID);
  Serial.print(F(" consumers=")); Serial.println(NUM_CONSUMERS);
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    uint8_t state = (digitalRead(consumerPins[i]) == HIGH) ? 1 : 0;
    Serial.print(F("   ")); Serial.print(consumerList[i]);
    Serial.println(state ? F(" = ON") : F(" = OFF"));
  }
  return true;
}

// ---------------- Отговор на изрична заявка от Gateway (без ACK очакване) ----------------
static void sendStateResponse() {
  sendStateResponseWire(CRYPTO_TYPE_STATE_RESP);
}

// ---------------- Restart state response - ACK/retry state machine ----------------
// Timeout-ът на ACK-а се изчислява от SF/BW (radioAckTimeoutRestartMs), не е константа.
#define STATE_RESP_MAX_RETRIES    2         // общо до 3 опита (1 + 2 retry-я)

static bool          restartRespPending  = false;
static unsigned long restartRespSentAt   = 0;
static uint8_t        restartRespRetries = 0;

void sendRestartStateResponse() {
  restartRespPending  = true;
  restartRespRetries  = 0;
  restartRespSentAt   = millis();
  sendStateResponseWire(CRYPTO_TYPE_STATE_RESP_RESTART);
}

void restartRespManager() {
  if (!restartRespPending) return;
  if (millis() - restartRespSentAt < radioAckTimeoutRestartMs()) return;

  if (restartRespRetries < STATE_RESP_MAX_RETRIES) {
    // ако TX слотът е зает, не броим опита - пробваме пак на следващата обиколка
    if (sendStateResponseWire(CRYPTO_TYPE_STATE_RESP_RESTART)) {
      restartRespRetries++;
      restartRespSentAt = millis();
      Serial.print(F("[RETRY STATE_RESP RESTART] опит #")); Serial.println(restartRespRetries);
    }
  } else {
    Serial.println(F("[STATE_RESP RESTART] няма ACK след 3 опита, отказвам се, продължавам нормално"));
    restartRespPending = false;
  }
}

bool restart_response_pending() {
  return restartRespPending;
}

// ---------------- Heartbeat (криптиран, без данни - само идентичност+counter+MAC) ----------------
static bool hbPending = false;
static unsigned long hbSendAt = 0;

void heartbeat_tick() {
  if (!hbPending && wdt_ticks >= HB_INTERVAL_CYCLES) {
    wdt_ticks -= HB_INTERVAL_CYCLES;
    hbPending = true;
    hbSendAt = millis() + random(0, JITTER_MAX_MS);
  }
  if (hbPending && (long)(millis() - hbSendAt) >= 0) {
    uint8_t wireBuf[CRYPTO_OVERHEAD];
    uint8_t wireLen = cryptoBuildWirePacket(wireBuf, NETWORK_KEY, CRYPTO_TYPE_EXEC_HB,
                                             (const uint8_t*)MY_M_ID, txCounter.next(),
                                             NULL, 0);
    // телеметрия: при зает канал (два CAD-а) heartbeat-ът се пропуска - следващият идва по график
    if (txRequest(TXK_HB, wireBuf, wireLen, CH_POLICY_TELEMETRY_SKIP)) {
      hbPending = false;
    }
    // ако TX слотът е зает с друг пакет, hbPending остава и се опитва пак на следващата обиколка
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
    // Пауза преди ACK: Gateway трябва да излезе от TX и да влезе в RX (half-duplex)
    ackEnqueue(p, millis() + RADIO_EXEC_ACK_DELAY_MS);
  }
}

void ackManager() {
  if (txPending) return;   // един пакет наведнъж; следващият ACK изчаква реда си

  unsigned long now = millis();
  for (uint8_t i = 0; i < ackCount; i++) {
    if ((long)(now - ackSendAt[i]) < 0) continue;

    uint8_t plaintext[CMD_PLAINTEXT_LEN];
    memcpy(plaintext, ackQueue[i].C_ID, 4);
    plaintext[4] = ackQueue[i].com;
    plaintext[5] = ackQueue[i].status;

    uint8_t wireBuf[CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN];
    uint8_t wireLen = cryptoBuildWirePacket(wireBuf, NETWORK_KEY, CRYPTO_TYPE_EXEC_ACK,
                                             (const uint8_t*)MY_M_ID, txCounter.next(),
                                             plaintext, sizeof(plaintext));

    memcpy(txTag, ackQueue[i].C_ID, 4); txTag[4] = 0;
    txStatus = ackQueue[i].status;
    if (txRequest(TXK_ACK, wireBuf, wireLen, CH_POLICY_COMMAND)) {
      ackRemoveAt(i);   // от тук нататък пакетът е собственост на TX слота
    }
    return;
  }
}
