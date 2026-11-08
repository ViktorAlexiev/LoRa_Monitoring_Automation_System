#include "radio_tx.h"
#include <SPI.h>
#include <LoRa.h>
#include "config_storage.h"
#include "crypto_common.h"
#include "cad.h"
#include "channel_access.h"
#include "radio_timing.h"
#include "debug_log.h"
#include "power_mgmt.h"

bool lora_init() {
  // времената (CAD, изчакване) се извеждат от SF/BW, прочетени от EEPROM
  radioTimingInit(LORA_SF, LORA_BW_HZ, LORA_CR_DENOM, LORA_PREAMBLE_LEN, true);

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  // честотата вече идва от EEPROM (config пакет), не hardcoded. LoRa.begin() резултатът се проверява: при мъртъв
  // модул след SENSOR_RADIO_TRIES опита се отказваме (false), вместо да висим и да изтощим батерията.
  uint8_t tries = 0;
  while (!LoRa.begin(LORA_FREQ_HZ)) {
    if (++tries >= SENSOR_RADIO_TRIES) {
      DBGLN(F("[ERR] LoRa не стартира след 5 опита - пропускам този цикъл"));
      return false;
    }
    wdt_feed();
    delay(200);
  }

  radioApplyModemSettings();   // SF, BW, CR, преамбюл + LDRO (по реалното Ts)
  LoRa.setTxPower(LORA_TX_POWER_DBM);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  return true;
}

// Компактен формат: 4 x int16 (little-endian, native на AVR) са plaintext-ът, който се
// криптира - S_ID НЕ е част от него, пътува в чисто (wire формат) като sender identity,
// нужна е на gateway-я преди decrypt. Стойността е закръглено x * 100 (0.01 °C / 0.01 %RH,
// колкото е точността на SHT31/SHT21). Plaintext 8 B -> wire пакет 22 B (вместо 30 B с float32).
// Единственият код за "няма данни" (грешка при четене или липсващ сензор - причината не се
// предава). Gateway го публикува към бекенда като -404.
#define SENSOR_RAW_NODATA  (-32768)
struct __attribute__((packed)) myPacketData {
  int16_t S_T;
  int16_t S_H;
  int16_t A_T;
  int16_t A_H;
};
// Форматът по въздуха е ИДЕНТИЧЕН с ESP-IDF прошивката (gateway-ят е един): plaintext 8 B, wire 22 B.
static_assert(sizeof(myPacketData) == 8, "sensor plaintext: 4 x int16 little-endian, packed");
static_assert(CRYPTO_OVERHEAD + sizeof(myPacketData) == 22, "sensor wire: id(6) + ciphertext(8) + counter(4) + tag(4)");

// float -> int16 x100. 255.0f е маркерът за грешка от sensors_io -> SENSOR_RAW_NODATA.
// Реалните стойности са в -40..100 (SHT), т.е. -4000..10000 - далеч от границите на int16.
static int16_t encodeCompact(float v) {
  if (v >= 254.5f) return SENSOR_RAW_NODATA;
  return (int16_t)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

void send_sensor_packet(float s_t, float s_h, float a_t, float a_h) {
  myPacketData pkt;
  pkt.S_T = encodeCompact(s_t);
  pkt.S_H = encodeCompact(s_h);
  pkt.A_T = encodeCompact(a_t);
  pkt.A_H = encodeCompact(a_h);

  DBG(F("TX S_ID=")); DBG(SENSOR_ID);
  DBG(F(" S_T=")); DBG(s_t);
  DBG(F(" S_H=")); DBG(s_h);
  DBG(F(" A_T=")); DBG(a_t);
  DBG(F(" A_H=")); DBG(a_h);
  DBG(F(" (wire ")); DBG((uint8_t)(CRYPTO_OVERHEAD + sizeof(myPacketData)));
  DBGLN(F(" B)"));
  DBG_FLUSH();

  uint8_t wireBuf[CRYPTO_OVERHEAD + sizeof(myPacketData)];
  uint8_t wireLen = cryptoBuildWirePacket(wireBuf, NETWORK_KEY, CRYPTO_TYPE_SENSOR,
                                           (const uint8_t*)SENSOR_ID, txCounter.next(),
                                           (const uint8_t*)&pkt, sizeof(pkt));

  // Listen-before-talk (телеметрия, FORCE): един CAD; ако е зает - ЕДНО случайно изчакване до
  // airtime на пакет и втори CAD; ако пак е зает, предава въпреки това (едно измерване е
  // по-ценно от чистия ефир). Sensor няма друга работа, затова тук блокираме (и после заспива).
  ChannelAccess ca;
  channelAccessStart(&ca, CH_POLICY_TELEMETRY_FORCE);
  while (channelAccessPoll(&ca, millis(), channelActive, channelRandom) == CH_WAIT) {
    wdt_feed();
    delay(1);
  }

  if (!loraTransmit(wireBuf, wireLen)) DBGLN(F("[ERR] предаването не приключи (таймаут)"));   // endPacket() без таймаут би висял
  LoRa.sleep();
}
