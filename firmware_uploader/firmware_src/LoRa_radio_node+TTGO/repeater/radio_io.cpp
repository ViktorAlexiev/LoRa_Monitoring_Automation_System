#include "radio_io.h"
#include <SPI.h>
#include <LoRa.h>
#include <string.h>
#include "config_storage.h"
#include "crypto_common.h"
#include "cad.h"
#include "channel_access.h"
#include "dedup.h"
#include "power_mgmt.h"
#include "debug_log.h"

void radio_setup() {
  // времената (CAD, изчакване) се извеждат от SF/BW, прочетени от EEPROM
  radioTimingInit(LORA_SF, LORA_BW_HZ, LORA_CR_DENOM, LORA_PREAMBLE_LEN, true);

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  uint32_t lastMsg = 0;
  while (!LoRa.begin(LORA_FREQ_RX_HZ)) {
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

  enter_rx_mode();
}

void enter_rx_mode() {
  LoRa.setFrequency(LORA_FREQ_RX_HZ);
  LoRa.receive();
}

// ---------------- Буфер на пакети, чакащи backoff преди forward ----------------
// Non-blocking - вместо delay() в самото приемане, пакетът се пази тук с планирано
// време за изпращане; loop() го проверява всяка обиколка. Позволява да продължим да
// приемаме нови пакети, докато друг чака своя backoff прозорец.
#define FWD_QUEUE_SIZE  4
struct PendingForward {
  uint8_t buf[DEDUP_MAX_LEN];
  uint8_t len;
  uint32_t sendAt;
  bool active;
};
static PendingForward fwdQueue[FWD_QUEUE_SIZE];

// ---------------- Достъп до канала (общ за forward-и и heartbeat) ----------------
// Радиото е едно, затова само един "собственик" наведнъж върти state machine-ата на достъпа.
// Между проверките радиото се връща на RX честотата, за да не се изпуснат входящи пакети.
#define OWNER_NONE 0
#define OWNER_FWD  1
#define OWNER_HB   2
static uint8_t       accessOwner = OWNER_NONE;
static int8_t        accessSlot  = -1;
static ChannelAccess ca;

static inline bool reached(uint32_t now, uint32_t t) {
  return (int32_t)(now - t) >= 0;
}

// CAD се прави на TX честотата (там ще предаваме); помним дали е бил изпълнен на този ход,
// за да върнем радиото на RX честотата само тогава.
static bool cadRan = false;
static bool cadOnTxFreq() {
  cadRan = true;
  LoRa.setFrequency(LORA_FREQ_TX_HZ);
  return channelActive();
}

static void transmitNow(const uint8_t *buf, uint8_t len) {
  // вика се при радио на TX честотата
  if (!loraTransmit(buf, len)) DBGLN(F("[TX] предаването не приключи (таймаут)"));
  enter_rx_mode();
}

void receive_and_queue(uint8_t len) {
  // По-дълъг пакет не минава през repeater-а: отрязано копие би се препратило като повреден кадър (приемникът го
  // отхвърля по MAC) и би хабило ефирно време. Изхвърля се цял - както в ESP-IDF прошивката.
  if (len > DEDUP_MAX_LEN) {
    while (LoRa.available()) LoRa.read();
    DBG(F("[RX] пакет от ")); DBG(len); DBG(F(" B е над границата ")); DBG(DEDUP_MAX_LEN); DBGLN(F(" B - не се препраща"));
    return;
  }
  uint8_t buf[DEDUP_MAX_LEN];
  uint8_t i = 0;
  while (LoRa.available() && i < len) {
    buf[i++] = (uint8_t)LoRa.read();
  }
  if (i == 0) return;

  // Вече сме препратили точно тези байтове наскоро (дублиран прием, напр. чут и от двама
  // repeater-и на предишното ниво) - пропусни. Пакетът не се променя при препращане, затова
  // дубликатите са байт по байт идентични.
  if (dedupSeen(buf, i)) {
    DBGLN(F("[RX] дублиран пакет (dedup), игнориран"));
    return;
  }
  dedupAdd(buf, i);

  for (uint8_t s = 0; s < FWD_QUEUE_SIZE; s++) {
    if (!fwdQueue[s].active) {
      memcpy(fwdQueue[s].buf, buf, i);
      fwdQueue[s].len = i;
      fwdQueue[s].sendAt = millis() + random(0, FORWARD_BACKOFF_MAX_MS);
      fwdQueue[s].active = true;
      DBG(F("[RX] пакет приет, len=")); DBG(i);
      DBGLN(F(" -> опашка за препращане"));
      return;
    }
  }
  // опашката е пълна (много рядко - 4 едновременни forward-а) - пакетът се губи
  DBGLN(F("[ERR] forward опашка пълна, пакет изгубен"));
}

void process_pending_forwards() {
  uint32_t now = millis();

  if (accessOwner == OWNER_NONE) {
    for (uint8_t s = 0; s < FWD_QUEUE_SIZE; s++) {
      if (fwdQueue[s].active && reached(now, fwdQueue[s].sendAt)) {
        accessOwner = OWNER_FWD;
        accessSlot  = s;
        // телеметрия/FORCE: едно измерване е по-ценно от чистия ефир - при два пъти зает канал
        // пакетът се предава въпреки това (по-добре колизия, отколкото загубено измерване)
        channelAccessStart(&ca, CH_POLICY_TELEMETRY_FORCE);
        break;
      }
    }
  }
  if (accessOwner != OWNER_FWD) return;

  cadRan = false;
  uint8_t r = channelAccessPoll(&ca, now, cadOnTxFreq, channelRandom);
  if (r == CH_WAIT) {
    if (cadRan) enter_rx_mode();   // между проверките слушаме на RX честотата
    return;
  }

  transmitNow(fwdQueue[accessSlot].buf, fwdQueue[accessSlot].len);
  DBG(F("[TX FORWARD] len=")); DBGLN(fwdQueue[accessSlot].len);
  fwdQueue[accessSlot].active = false;
  accessSlot  = -1;
  accessOwner = OWNER_NONE;
}

bool any_forward_pending() {
  for (uint8_t s = 0; s < FWD_QUEUE_SIZE; s++) {
    if (fwdQueue[s].active) return true;
  }
  return false;
}

// ---------------- Heartbeat ----------------
static bool hbPending = false;
static uint32_t hbSendAt = 0;

void heartbeat_tick() {
  uint32_t now = millis();

  if (!hbPending && wdt_ticks_consume(HB_INTERVAL_CYCLES)) {
    hbPending = true;
    hbSendAt = now + random(0, HB_JITTER_MAX_MS);
  }
  if (!hbPending) return;

  if (accessOwner == OWNER_NONE && reached(now, hbSendAt)) {
    accessOwner = OWNER_HB;
    channelAccessStart(&ca, CH_POLICY_TELEMETRY_SKIP);   // heartbeat-ът се пропуска при зает канал
  }
  if (accessOwner != OWNER_HB) return;

  cadRan = false;
  uint8_t r = channelAccessPoll(&ca, now, cadOnTxFreq, channelRandom);
  if (r == CH_WAIT) {
    if (cadRan) enter_rx_mode();
    return;
  }

  if (r == CH_CLEAR) {
    uint8_t wireBuf[CRYPTO_OVERHEAD];
    uint8_t wireLen = cryptoBuildWirePacket(wireBuf, NETWORK_KEY, CRYPTO_TYPE_REPEATER_HB,
                                             (const uint8_t*)REPEATER_ID, hbCounter.next(),
                                             NULL, 0);
    transmitNow(wireBuf, wireLen);
    DBG(F("[TX HB] M_ID=")); DBGLN(REPEATER_ID);
  } else {
    enter_rx_mode();
    DBGLN(F("[HB] канала зает, пропускам този heartbeat"));
  }
  hbPending   = false;
  accessOwner = OWNER_NONE;
}

bool heartbeat_pending() {
  return hbPending;
}
