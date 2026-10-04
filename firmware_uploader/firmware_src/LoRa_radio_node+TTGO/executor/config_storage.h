#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>
#include "crypto_common.h"
#include "ceiling_counter.h"
#include "radio_timing.h"

#define EEPROM_ADDR_MODULE_ID     0
#define EEPROM_ADDR_NUM_CONSUMERS 6
#define EEPROM_ADDR_CONSUMERS     7
#define CONSUMER_RECORD_SIZE      6
#define MAX_CONSUMERS             10
#define MODULE_ID_LEN             6
#define CONSUMER_ID_LEN           4
#define PIN_LEN                   2
#define EEPROM_ADDR_FREQUENCY     67  // 4 bytes (uint32_t) - LoRa честота към Gateway, от config_avr.ino
#define EEPROM_ADDR_KEY           75  // 16 bytes - AES мрежов ключ, от config_avr.ino
// Един общ nonce поток за HB И ACK/NACK - type байтът в nonce-а (CRYPTO_TYPE_EXEC_HB срещу
// CRYPTO_TYPE_EXEC_ACK) вече гарантира разграничение дори при съвпадащ counter, значи
// разделянето на два отделни ceiling-а не добавя нищо, само излишен EEPROM адрес.
#define EEPROM_ADDR_CEILING       91  // 4 bytes - nonce ceiling, общ за HB и ACK
#define EEPROM_ADDR_SF            95  // 1 byte - Spreading Factor (7..12), от config_avr.ino; 0xFF/невалиден -> default
#define EEPROM_ADDR_BW_IDX        96  // 1 byte - индекс на BW: 0=62.5k, 1=125k, 2=250k; 0xFF/невалиден -> default

// Default честота - fallback ако EEPROM е неинициализиран (0 или 0xFFFFFFFF)
#define LORA_FREQ_DEFAULT_HZ 433000000UL

extern char MY_M_ID[MODULE_ID_LEN + 1];
extern char consumerList[MAX_CONSUMERS][CONSUMER_ID_LEN + 1];
extern uint8_t consumerPins[MAX_CONSUMERS];
extern uint8_t NUM_CONSUMERS;
extern uint32_t LORA_FREQ_HZ;
extern uint8_t  LORA_SF;
extern uint32_t LORA_BW_HZ;
extern uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
extern CeilingCounter txCounter;

uint8_t pinFromString(const char *s);
void loadConfigFromEeprom();

#endif
