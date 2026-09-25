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

void radio_setup() {
  // времената (CAD, изчакване) се извеждат от SF/BW, прочетени от EEPROM
  radioTimingInit(LORA_SF, LORA_BW_HZ, LORA_CR_DENOM, LORA_PREAMBLE_LEN, true);

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  unsigned long lastMsg = 0;
  while (!LoRa.begin(LORA_FREQ_RX_HZ)) {
    if (millis() - lastMsg >= LORA_BEGIN_RETRY_MSG_MS) {
      Serial.println(F("[ERR] LoRa не стартира (LoRa.begin провал), продължавам да опитвам..."));
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
  unsigned long sendAt;
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

static inline bool reached(unsigned long now, unsigned long t) {
  return (long)(now - t) >= 0;
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
  LoRa.beginPacket();
  LoRa.write(buf, len);
  LoRa.endPacket();
  enter_rx_mode();
}

void receive_and_queue(int len) {
  uint8_t buf[DEDUP_MAX_LEN];
  int i = 0;
  while (LoRa.available() && i < len && i < (int)sizeof(buf)) {
    buf[i++] = (uint8_t)LoRa.read();
  }
  while (LoRa.available()) LoRa.read();   // изхвърли остатъка, ако пакетът е бил по-голям от буфера ни
  if (i == 0) return;

  // Вече сме препратили точно тези байтове наскоро (дублиран прием, напр. чут и от двама
  // repeater-и на предишното ниво) - пропусни. Пакетът не се променя при препращане, затова
  // дубликатите са байт по байт идентични.
  if (dedupSeen(buf, i)) {
    Serial.println(F("[RX] дублиран пакет (dedup), игнориран"));
    return;
  }
  dedupAdd(buf, i);

  for (uint8_t s = 0; s < FWD_QUEUE_SIZE; s++) {
    if (!fwdQueue[s].active) {
      memcpy(fwdQueue[s].buf, buf, i);
      fwdQueue[s].len = i;
      fwdQueue[s].sendAt = millis() + random(0, FORWARD_BACKOFF_MAX_MS);
      fwdQueue[s].active = true;
      Serial.print(F("[RX] пакет приет, len=")); Serial.print(i);
      Serial.println(F(" -> опашка за препращане"));
      return;
    }
  }
  // опашката е пълна (много рядко - 4 едновременни forward-а) - пакетът се губи
  Serial.println(F("[ERR] forward опашка пълна, пакет изгубен"));
}

void process_pending_forwards() {
  unsigned long now = millis();

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
  Serial.print(F("[TX FORWARD] len=")); Serial.println(fwdQueue[accessSlot].len);
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
static unsigned long hbSendAt = 0;

void heartbeat_tick() {
  unsigned long now = millis();

  if (!hbPending && wdt_ticks >= HB_INTERVAL_CYCLES) {
    wdt_ticks -= HB_INTERVAL_CYCLES;
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
    Serial.print(F("[TX HB] M_ID=")); Serial.println(REPEATER_ID);
  } else {
    enter_rx_mode();
    Serial.println(F("[HB] канала зает, пропускам този heartbeat"));
  }
  hbPending   = false;
  accessOwner = OWNER_NONE;
}

bool heartbeat_pending() {
  return hbPending;
}
