#include "radio_io.h"
#include <SPI.h>
#include <LoRa.h>
#include <string.h>
#include "config_storage.h"
#include "crypto_common.h"
#include "cad.h"
#include "dedup.h"
#include "power_mgmt.h"

void radio_setup() {
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  unsigned long lastMsg = 0;
  while (!LoRa.begin(LORA_FREQ_RX_HZ)) {
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

void receive_and_queue(int len) {
  uint8_t buf[DEDUP_MAX_LEN];
  int i = 0;
  while (LoRa.available() && i < len && i < (int)sizeof(buf)) {
    buf[i++] = (uint8_t)LoRa.read();
  }
  while (LoRa.available()) LoRa.read();   // изхвърли остатъка, ако пакетът е бил по-голям от буфера ни
  if (i == 0) return;

  // Пакетът вече е препратен от друг repeater - не го препращай втори път
  if (buf[i - 1] == REPEATED_MARKER) {
    Serial.println(F("[RX] вече препратен пакет (marker), игнориран"));
    return;
  }

  // Вече сме препратили точно тези байтове наскоро (дублиран прием) - пропусни
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
  // опашката е пълна (много рядко - 4 едновременни forward-а) - пакетът се губи мълчаливо
  Serial.println(F("[ERR] forward опашка пълна, пакет изгубен"));
}

void process_pending_forwards() {
  unsigned long now = millis();
  for (uint8_t s = 0; s < FWD_QUEUE_SIZE; s++) {
    if (!fwdQueue[s].active || now < fwdQueue[s].sendAt) continue;

    LoRa.setFrequency(LORA_FREQ_TX_HZ);
    if (channelActive()) continue;   // канала е зает точно сега - опитай пак следващия loop()

    uint8_t len = fwdQueue[s].len;
    uint8_t buf[DEDUP_MAX_LEN + 1];
    memcpy(buf, fwdQueue[s].buf, len);
    if (len < sizeof(buf)) buf[len++] = REPEATED_MARKER;

    LoRa.beginPacket();
    LoRa.write(buf, len);
    LoRa.endPacket();
    enter_rx_mode();

    Serial.print(F("[TX FORWARD] len=")); Serial.println(len);

    fwdQueue[s].active = false;
  }
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

static void send_heartbeat_now() {
  LoRa.setFrequency(LORA_FREQ_TX_HZ);
  if (channelActive()) return;   // канала е зает - hbPending си остава true, опитваме пак следващия loop()

  uint8_t wireBuf[CRYPTO_OVERHEAD];
  uint8_t wireLen = cryptoBuildWirePacket(wireBuf, NETWORK_KEY, CRYPTO_TYPE_REPEATER_HB,
                                           (const uint8_t*)REPEATER_ID, hbCounter.next(),
                                           NULL, 0);

  LoRa.beginPacket();
  LoRa.write(wireBuf, wireLen);
  LoRa.endPacket();
  enter_rx_mode();

  Serial.print(F("[TX HB] M_ID=")); Serial.println(REPEATER_ID);

  hbPending = false;
}

void heartbeat_tick() {
  if (!hbPending && wdt_ticks >= HB_INTERVAL_CYCLES) {
    wdt_ticks -= HB_INTERVAL_CYCLES;
    hbPending = true;
    hbSendAt = millis() + random(0, HB_JITTER_MAX_MS);
  }
  if (hbPending && millis() >= hbSendAt) {
    send_heartbeat_now();   // сама изчиства hbPending само при успешно предаване (не при busy channel)
  }
}

bool heartbeat_pending() {
  return hbPending;
}
