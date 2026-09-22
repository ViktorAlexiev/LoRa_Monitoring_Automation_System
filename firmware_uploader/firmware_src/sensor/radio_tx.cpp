#include "radio_tx.h"
#include <SPI.h>
#include <LoRa.h>
#include "config_storage.h"
#include "crypto_common.h"
#include "cad.h"

void lora_init() {
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

  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH_HZ);
  LoRa.setCodingRate4(LORA_CR_DENOM);
  LoRa.setTxPower(LORA_TX_POWER_DBM);
  LoRa.setPreambleLength(LORA_PREAMBLE_LEN);
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

  // CAD преди TX - проверка дали каналът е свободен точно сега. Ако е зает, изчакваме
  // кратко и опитваме пак (по-добре кратка загуба на време, отколкото колизия и изгубено
  // измерване); ограничен брой опити, за да не задържим устройството будно неопределено.
  uint8_t cadAttempts = 0;
  while (channelActive() && cadAttempts < 5) {
    delay(random(5, 20));
    cadAttempts++;
  }

  LoRa.beginPacket();
  LoRa.write(wireBuf, wireLen);
  LoRa.endPacket();
  LoRa.sleep();
}
