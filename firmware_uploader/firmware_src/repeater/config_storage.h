#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>
#include "crypto_common.h"
#include "ceiling_counter.h"

#define EEPROM_ADDR_MODULE_ID  0
#define MODULE_ID_LEN           6
#define EEPROM_ADDR_FREQUENCY     67  // 4 bytes (uint32_t) - RX честота (от Sensor), от config_avr.ino
#define EEPROM_ADDR_FREQUENCY_TX  71  // 4 bytes (uint32_t) - TX честота (към Gateway), от config_avr.ino
#define EEPROM_ADDR_KEY           75  // 16 bytes - AES мрежов ключ, от config_avr.ino
#define EEPROM_ADDR_CEILING       91  // 4 bytes - nonce ceiling за собствения HB поток

// Default честоти - fallback ако EEPROM е неинициализиран (0 или 0xFFFFFFFF)
#define LORA_FREQ_RX_DEFAULT_HZ  434000000UL
#define LORA_FREQ_TX_DEFAULT_HZ  433000000UL

extern char REPEATER_ID[MODULE_ID_LEN + 1];
extern uint32_t LORA_FREQ_RX_HZ;
extern uint32_t LORA_FREQ_TX_HZ;
extern uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
extern CeilingCounter hbCounter;

void loadConfigFromEeprom();

#endif
