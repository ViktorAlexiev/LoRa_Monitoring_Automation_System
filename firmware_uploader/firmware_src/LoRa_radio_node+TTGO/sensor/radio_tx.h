#ifndef RADIO_TX_H
#define RADIO_TX_H

#include <Arduino.h>

#define LORA_NSS    10
#define LORA_DIO0    2
#define LORA_RST     9

// SF и BW НЕ са константи - идват от EEPROM (LORA_SF/LORA_BW_HZ в config_storage.h)
#define LORA_CR_DENOM       5
#define LORA_TX_POWER_DBM   12   // 433 MHz: 10 mW ERP = ~12 dBm EIRP при антена 0 dBi (усилването на антената се изважда)
#define LORA_PREAMBLE_LEN   8
#define LORA_SYNC_WORD       0x12

#define JITTER_MAX_MS  1000  // случайно закъснение (в sensor.ino, ПРЕДИ радиото да се събуди) - разминава колизии с други sensor-и
#define SENSOR_RADIO_TRIES  5   // опита за LoRa.begin() на цикъл; после цикълът се пропуска и сензорът заспива (батерия!)

// Инициализира радиото на честотата от EEPROM. Опитва до SENSOR_RADIO_TRIES пъти; false = радиото не отговаря:
// цикълът се пропуска и сензорът заспива до следващото измерване (безкраен цикъл изтощаваше батерията).
bool lora_init();

// Строи криптирания wire пакет (S_ID в чисто, 4-те стойности като int16 x100 криптирани -
// 22 B общо), праща по LoRa (с jitter преди TX), после приспива радиото.
void send_sensor_packet(float s_t, float s_h, float a_t, float a_h);

#endif
