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
#include "debug_log.h"

// Downlink wire дължини (target M_ID, чисто, MODULE_ID_LEN bytes + crypto wire):
#define CMD_PLAINTEXT_LEN  6   // C_ID(4) + com(1) + status(1)
#define CMD_WIRE_LEN  (MODULE_ID_LEN + CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN)        // 26
// State request и state-resp ACK имат еднакъв wire формат (target M_ID + празен
// ciphertext wire) - разграничават се само по type-id при decrypt, не по дължина.
#define STATE_REQ_WIRE_LEN       (MODULE_ID_LEN + CRYPTO_OVERHEAD)                 // 20
#define STATE_RESP_ACK_WIRE_LEN  (MODULE_ID_LEN + CRYPTO_OVERHEAD)                 // 20 (същата стойност)
#define STATE_ENTRY_LEN  5      // C_ID(4) + state(1), на консуматор в state response plaintext

// Форматът по въздуха е ИДЕНТИЧЕН с ESP-IDF прошивката (gateway-ят е един): сменена константа тук чупи връзката.
static_assert(CRYPTO_OVERHEAD == 14, "wire: id(6) + counter(4) + tag(4)");
static_assert(CMD_WIRE_LEN == 26, "команда: target(6) + wire(14) + plaintext(6)");
static_assert(STATE_REQ_WIRE_LEN == 20, "state request / state-resp ACK: target(6) + wire(14)");
static_assert(sizeof(CommandPacket) == 6, "CommandPacket е packed: C_ID(4) + com(1) + status(1)");

void radio_setup() {
  // времената (CAD, ACK timeout-и) се извеждат от SF/BW, прочетени от EEPROM
  radioTimingInit(LORA_SF, LORA_BW_HZ, LORA_CR_DENOM, LORA_PREAMBLE_LEN, true);

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  uint32_t lastMsg = 0;
  while (!LoRa.begin(LORA_FREQ_HZ)) {
    wdt_feed();   // мъртво радио не е забиване: продължава да опитва
    if (millis() - lastMsg >= LORA_BEGIN_RETRY_MSG_MS) {
      DBGLN(F("[ERR] LoRa не стартира (LoRa.begin провал), продължавам да опитвам..."));
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
static uint32_t txCreatedAt = 0;
static ChannelAccess txAccess;

// Пакетът вече е построен в txBuf (без второ копие в стека и без memcpy): предава собствеността на TX слота.
static void txCommit(uint8_t kind, uint8_t len, uint8_t policy) {
  txLen = len;
  txKind = kind;
  txCreatedAt = millis();
  channelAccessStart(&txAccess, policy);
  txPending = true;
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
    bool sent = loraTransmit(txBuf, txLen);   // асинхронно, с таймаут (endPacket() без async виси при мъртъв чип)
    LoRa.receive();
    if (!sent) {
      DBG(F("[TX] предаването не приключи (таймаут), вид=")); DBGLN(txKind);
      txPending = false;
      return;
    }

    switch (txKind) {
      case TXK_ACK:
        DBG(F("[TX ")); DBG(txStatus == STATUS_ACK ? F("ACK] ") : F("NACK] "));
        DBGLN(txTag);
        break;
      case TXK_STATE_RESP:
        DBGLN(F("[TX STATE_RESP] изпратен"));
        break;
      case TXK_RESTART_RESP:
        DBGLN(F("[TX STATE_RESP RESTART] изпратен"));
        break;
      case TXK_HB:
        DBG(F("[TX HB] M_ID=")); DBGLN(MY_M_ID);
        break;
    }
    txPending = false;
    return;
  }

  // CH_GIVEUP - срокът за сондиране изтече, каналът остана зает
  LoRa.receive();
  if (txKind == TXK_ACK && (millis() - txCreatedAt) < radioAckTimeoutCmdMs()) {
    // ACK-ът още има смисъл (Gateway още чака) - започни нов цикъл на достъп
    DBG(F("[ACK/NACK] канала зает, опитвам пак, C_ID=")); DBGLN(txTag);
    channelAccessStart(&txAccess, CH_POLICY_COMMAND);
    return;
  }
  DBG(F("[TX] канала остана зает, отказвам се (вид=")); DBG(txKind); DBGLN(F(")"));
  txPending = false;
}

// ---------------- Строи и заявява криптиран state response (нормален или RESTART) ----------------
static bool sendStateResponseWire(uint8_t typeId) {
  // Слотът се проверява ПРЕДИ строенето: иначе всеки отказ харчи стойност на nonce брояча (и EEPROM запис на 100-ата).
  if (txPending) {
    DBGLN(F("[STATE_RESP] TX слотът е зает, пропускам"));
    return false;
  }
  uint8_t plaintext[MAX_CONSUMERS * STATE_ENTRY_LEN];
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    uint8_t off = i * STATE_ENTRY_LEN;
    memcpy(plaintext + off, consumerList[i], 4);
    plaintext[off + 4] = (digitalRead(consumerPins[i]) == HIGH) ? 1 : 0;
  }
  uint8_t ptLen = NUM_CONSUMERS * STATE_ENTRY_LEN;

  uint8_t wireLen = cryptoBuildWirePacket(txBuf, NETWORK_KEY, typeId,
                                           (const uint8_t*)MY_M_ID, txCounter.next(),
                                           plaintext, ptLen);

  uint8_t kind = (typeId == CRYPTO_TYPE_STATE_RESP_RESTART) ? TXK_RESTART_RESP : TXK_STATE_RESP;
  txCommit(kind, wireLen, CH_POLICY_COMMAND);

  DBG(F("[STATE_RESP")); DBG(kind == TXK_RESTART_RESP ? F(" RESTART") : F(""));
  DBG(F("] заявен, M_ID=")); DBG(MY_M_ID);
  DBG(F(" consumers=")); DBGLN(NUM_CONSUMERS);
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    uint8_t state = (digitalRead(consumerPins[i]) == HIGH) ? 1 : 0;
    DBG(F("   ")); DBG(consumerList[i]);
    DBGLN(state ? F(" = ON") : F(" = OFF"));
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
static uint32_t restartRespSentAt   = 0;
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
      DBG(F("[RETRY STATE_RESP RESTART] опит #")); DBGLN(restartRespRetries);
    }
  } else {
    DBGLN(F("[STATE_RESP RESTART] няма ACK след 3 опита, отказвам се, продължавам нормално"));
    restartRespPending = false;
  }
}

bool restart_response_pending() {
  return restartRespPending;
}

// ---------------- Heartbeat (криптиран, без данни - само идентичност+counter+MAC) ----------------
static bool hbPending = false;
static uint32_t hbSendAt = 0;

void heartbeat_tick() {
  if (!hbPending && wdt_ticks_consume(HB_INTERVAL_CYCLES)) {
    hbPending = true;
    hbSendAt = millis() + random(0, JITTER_MAX_MS);
  }
  // Ако TX слотът е зает с друг пакет, hbPending остава и се опитва пак на следващата обиколка; пакетът не се строи
  // (и nonce броячът не се харчи) докато слотът не е свободен - преди това всяка обиколка изгаряше по една стойност.
  if (hbPending && !txPending && (int32_t)(millis() - hbSendAt) >= 0) {
    uint8_t wireLen = cryptoBuildWirePacket(txBuf, NETWORK_KEY, CRYPTO_TYPE_EXEC_HB,
                                             (const uint8_t*)MY_M_ID, txCounter.next(),
                                             NULL, 0);
    // телеметрия: при зает канал (два CAD-а) heartbeat-ът се пропуска - следващият идва по график
    txCommit(TXK_HB, wireLen, CH_POLICY_TELEMETRY_SKIP);
    hbPending = false;
  }
}

bool heartbeat_pending() {
  return hbPending;
}

// ---------------- RADIO RECEIVE POLL ----------------
void radioReceivePoll() {
  uint8_t packetSize = (uint8_t)LoRa.parsePacket();   // 0..255

  if (packetSize == CMD_WIRE_LEN) {
    uint8_t buf[CMD_WIRE_LEN];
    LoRa.readBytes(buf, CMD_WIRE_LEN);

    // target проверка ПРЕДИ decrypt - евтино, отхвърля пакети за други executor-и веднага
    if (memcmp(buf, MY_M_ID, MODULE_ID_LEN) != 0) {
      DBGLN(F("[RX CMD] не е за мен, игнориран"));
      return;
    }

    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[CMD_PLAINTEXT_LEN];
    uint8_t ptLen;
    if (!cryptoParseWirePacket(buf + MODULE_ID_LEN, CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN,
                                NETWORK_KEY, CRYPTO_TYPE_GW_CMD, senderId, plaintext, &ptLen)) {
      DBGLN(F("[RX CMD] невалиден MAC, отхвърлен"));
      return;
    }

    CommandPacket p;
    memcpy(p.C_ID, plaintext, 4);
    p.com    = plaintext[4];
    p.status = plaintext[5];

    char c_id[5] = {0}; memcpy(c_id, p.C_ID, 4);
    DBG(F("[RX CMD] C_ID=")); DBG(c_id);
    DBG(F(" com=0x")); DBGLN(p.com, HEX);

    rxPush(p);
  }
  else if (packetSize == STATE_REQ_WIRE_LEN) {
    uint8_t buf[STATE_REQ_WIRE_LEN];
    LoRa.readBytes(buf, STATE_REQ_WIRE_LEN);

    // target проверка ПРЕДИ decrypt - общ формат за state request и state-resp ACK
    if (memcmp(buf, MY_M_ID, MODULE_ID_LEN) != 0) {
      DBGLN(F("[RX STATE] не е за мен, игнориран"));
      return;
    }

    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[1];
    uint8_t ptLen;

    if (cryptoParseWirePacket(buf + MODULE_ID_LEN, CRYPTO_OVERHEAD, NETWORK_KEY,
                               CRYPTO_TYPE_STATE_REQ, senderId, plaintext, &ptLen)) {
      DBGLN(F("[RX STATE_REQ] за мен, изпращам състояния"));
      sendStateResponse();
    }
    else if (cryptoParseWirePacket(buf + MODULE_ID_LEN, CRYPTO_OVERHEAD, NETWORK_KEY,
                                    CRYPTO_TYPE_STATE_RESP_ACK, senderId, plaintext, &ptLen)) {
      if (restartRespPending) {
        DBGLN(F("[RX STATE_RESP_ACK] потвърдено от Gateway, спирам retry-тата"));
        restartRespPending = false;
      }
    }
    else {
      DBGLN(F("[RX STATE] невалиден MAC, отхвърлен"));
    }
  }
  else if (packetSize > 0) {
    DBG(F("[RX] непознат размер=")); DBGLN(packetSize);
    while (LoRa.available()) LoRa.read();
  }
}

void processRx() {
  CommandPacket p;
  while (rxPop(&p)) {
    char c_id[5] = {0};
    memcpy(c_id, p.C_ID, 4);

    int8_t idx = consumerIndex(p.C_ID);
    bool cmdOk = (p.com == CMD_ON || p.com == CMD_OFF);

    if (idx >= 0 && cmdOk) {
      digitalWrite(consumerPins[idx], p.com == CMD_ON ? HIGH : LOW);
      DBG(F("[EXEC] "));
      DBG(c_id);
      DBG(F(" (pin "));
      DBG(consumerPins[idx]);
      DBGLN(p.com == CMD_ON ? F(") -> ON") : F(") -> LOW"));
      p.status = STATUS_ACK;
    } else {
      DBG(F("[REJECT] "));
      DBG(c_id);
      if (idx < 0) DBG(F(" - консуматор не съществува"));
      if (!cmdOk)  DBG(F(" - невалидна команда"));
      DBGLN();
      p.status = STATUS_NACK;
    }
    // Пауза преди ACK: Gateway трябва да излезе от TX и да влезе в RX (half-duplex)
    ackEnqueue(p, millis() + RADIO_EXEC_ACK_DELAY_MS);
  }
}

void ackManager() {
  if (txPending) return;   // един пакет наведнъж; следващият ACK изчаква реда си

  uint32_t now = millis();
  for (uint8_t i = 0; i < ackCount; i++) {
    if ((int32_t)(now - ackSendAt[i]) < 0) continue;

    uint8_t plaintext[CMD_PLAINTEXT_LEN];
    memcpy(plaintext, ackQueue[i].C_ID, 4);
    plaintext[4] = ackQueue[i].com;
    plaintext[5] = ackQueue[i].status;

    uint8_t wireLen = cryptoBuildWirePacket(txBuf, NETWORK_KEY, CRYPTO_TYPE_EXEC_ACK,
                                             (const uint8_t*)MY_M_ID, txCounter.next(),
                                             plaintext, sizeof(plaintext));

    memcpy(txTag, ackQueue[i].C_ID, 4); txTag[4] = 0;
    txStatus = ackQueue[i].status;
    txCommit(TXK_ACK, wireLen, CH_POLICY_COMMAND);   // слотът е свободен (проверено най-горе)
    ackRemoveAt(i);                                   // от тук нататък пакетът е собственост на TX слота
    return;
  }
}
