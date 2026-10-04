#ifndef RADIO_TX_H
#define RADIO_TX_H

#include <Arduino.h>

#include "board_pins.h"   // LORA_SCK/MISO/MOSI/NSS/RST/DIO0 - виж board_pins.h

// SF и BW НЕ са константи - идват от NVS (LORA_SF/LORA_BW_HZ в config_storage.h)
#define LORA_CR_DENOM       5
#define LORA_TX_POWER_DBM   17
#define LORA_PREAMBLE_LEN   8
#define LORA_SYNC_WORD       0x12

#define JITTER_MAX_MS  1000  // случайно закъснение преди TX - разминава колизии с други sensor-и
#define LORA_BEGIN_RETRY_MSG_MS  10000UL   // интервал между диагностични съобщения при неуспешен LoRa.begin()

// Инициализира радиото на честотата от NVS. Ако LoRa.begin() се провали, остава в
// цикъл и печата диагностично съобщение на всеки LORA_BEGIN_RETRY_MSG_MS (за сериен дебъг
// на терен) - вика LoRa.begin() отново на всеки опит.
void lora_init();

// Строи криптирания wire пакет (S_ID в чисто, 4-те стойности като int16 x100 криптирани -
// 22 B общо), праща по LoRa (с jitter преди TX), после приспива радиото.
void send_sensor_packet(float s_t, float s_h, float a_t, float a_h);

#endif
