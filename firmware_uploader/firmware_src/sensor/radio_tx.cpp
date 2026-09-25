#include "radio_tx.h"
#include <SPI.h>
#include <LoRa.h>
#include "config_storage.h"
#include "crypto_common.h"
#include "cad.h"
#include "channel_access.h"
#include "radio_timing.h"

void lora_init() {
  // времената (CAD, изчакване) се извеждат от SF/BW, прочетени от EEPROM
  radioTimingInit(LORA_SF, LORA_BW_HZ, LORA_CR_DENOM, LORA_PREAMBLE_LEN, true);

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);

  // честотата вече идва от EEPROM (config пакет), не hardcoded. Ако модулът не отговори,
  // остава в цикъл с периодично Serial съобщение (сериен дебъг на терен), вместо да
  // продължи напред все едно всичко е наред (LoRa.begin() резултатът се проверява).
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
}

// Тия 4 float-а са plaintext-ът, който се криптира - S_ID НЕ е част от него, пътува
// в чисто (wire формат) като sender identity, нужна е на gateway-я преди decrypt.
struct __attribute__((packed)) myPacketData {
  float S_T;
  float S_H;
  float A_T;
  float A_H;
};

void send_sensor_packet(float s_t, float s_h, float a_t, float a_h) {
  myPacketData pkt;
  pkt.S_T = s_t;
  pkt.S_H = s_h;
  pkt.A_T = a_t;
  pkt.A_H = a_h;

  delay(random(0, JITTER_MAX_MS));   // jitter - разминава TX-а с други sensor-и на честотата

  Serial.print(F("TX S_ID=")); Serial.print(SENSOR_ID);
  Serial.print(F(" S_T=")); Serial.print(pkt.S_T);
  Serial.print(F(" S_H=")); Serial.print(pkt.S_H);
  Serial.print(F(" A_T=")); Serial.print(pkt.A_T);
  Serial.print(F(" A_H=")); Serial.println(pkt.A_H);
  Serial.flush();

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
    delay(1);
  }

  LoRa.beginPacket();
  LoRa.write(wireBuf, wireLen);
  LoRa.endPacket();
  LoRa.sleep();
}
