#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>
#include "crypto_common.h"
#include "ceiling_counter.h"
#include "radio_timing.h"

#define EEPROM_ADDR_MODULE_ID   0
#define MODULE_ID_LEN           6
#define EEPROM_ADDR_FREQUENCY   67  // 4 bytes (uint32_t) - записва се от config_avr.ino
#define EEPROM_ADDR_KEY         75  // 16 bytes - AES мрежов ключ, от config_avr.ino
#define EEPROM_ADDR_CEILING     91  // 4 bytes (uint32_t) - nonce counter ceiling watermark
#define EEPROM_ADDR_SF          95  // 1 byte - Spreading Factor (7..12), от config_avr.ino; 0xFF/невалиден -> default
#define EEPROM_ADDR_BW_IDX      96  // 1 byte - индекс на BW: 0=62.5k, 1=125k, 2=250k; 0xFF/невалиден -> default

// Default честота - fallback ако EEPROM е неинициализиран (0 или 0xFFFFFFFF).
// Честотата "до Gateway" (не "Sensor->Repeater") - по-безопасен fallback: Sensor поне
// има шанс да достигне директно Gateway, дори без Repeater в мрежата.
#define LORA_FREQ_DEFAULT_HZ 433000000UL

extern char SENSOR_ID[MODULE_ID_LEN + 1];
extern uint32_t LORA_FREQ_HZ;
extern uint8_t  LORA_SF;
extern uint32_t LORA_BW_HZ;
extern uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
extern CeilingCounter txCounter;

void loadConfigFromEeprom();

#endif
