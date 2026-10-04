#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>
#include "crypto_common.h"
#include "ceiling_counter.h"
#include "radio_timing.h"

#define MAX_CONSUMERS             10
#define MODULE_ID_LEN             6
#define CONSUMER_ID_LEN           4

// Default честота - fallback ако NVS е празен
#define LORA_FREQ_DEFAULT_HZ 433000000UL

extern char MY_M_ID[MODULE_ID_LEN + 1];
extern char consumerList[MAX_CONSUMERS][CONSUMER_ID_LEN + 1];
extern uint8_t consumerPins[MAX_CONSUMERS];
extern uint8_t NUM_CONSUMERS;
extern uint32_t LORA_FREQ_HZ;
extern uint8_t  LORA_SF;
extern uint32_t LORA_BW_HZ;
extern uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
// Един общ nonce поток за HB И ACK/NACK - type байтът в nonce-а вече гарантира разграничение.
extern CeilingCounter txCounter;

uint8_t pinFromString(const char *s);   // ESP32: само GPIO номер (число)

// Чете настройките от NVS (namespace "cfg"), записани от config_esp32 (stage 1):
//   id, nc (брой консуматори), c<i>i (ID, string), c<i>p (GPIO пин, uchar), freq, sf, bw, key
void loadConfigFromNvs();

#endif
