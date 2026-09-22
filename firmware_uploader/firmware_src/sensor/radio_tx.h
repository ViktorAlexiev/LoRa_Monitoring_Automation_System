#ifndef RADIO_TX_H
#define RADIO_TX_H

#include <Arduino.h>

#define LORA_NSS    10
#define LORA_DIO0    2
#define LORA_RST     9

#define LORA_BANDWIDTH_HZ   125E3
#define LORA_SF             7
#define LORA_CR_DENOM       5
#define LORA_TX_POWER_DBM   17
#define LORA_PREAMBLE_LEN   8
#define LORA_SYNC_WORD       0x12

#define JITTER_MAX_MS  150   // случайно закъснение преди TX - разминава колизии с други sensor-и
#define LORA_BEGIN_RETRY_MSG_MS  10000UL   // интервал между диагностични съобщения при неуспешен LoRa.begin()

// Инициализира радиото на честотата от EEPROM. Ако LoRa.begin() се провали, остава в
// цикъл и печата диагностично съобщение на всеки LORA_BEGIN_RETRY_MSG_MS (за сериен дебъг
// на терен) - вика LoRa.begin() отново на всеки опит.
void lora_init();

// Строи криптирания wire пакет (S_ID в чисто, 4-те float-а криптирани), праща по LoRa
// (с jitter преди TX), после приспива радиото.
void send_sensor_packet(float s_t, float s_h, float a_t, float a_h);

#endif
