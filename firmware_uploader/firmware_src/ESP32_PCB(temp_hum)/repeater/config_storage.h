#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>
#include "crypto_common.h"
#include "ceiling_counter.h"
#include "radio_timing.h"

#define MODULE_ID_LEN           6

// Default честоти - fallback ако NVS е празен
#define LORA_FREQ_RX_DEFAULT_HZ  434000000UL
#define LORA_FREQ_TX_DEFAULT_HZ  433000000UL

extern char REPEATER_ID[MODULE_ID_LEN + 1];
extern uint32_t LORA_FREQ_RX_HZ;
extern uint32_t LORA_FREQ_TX_HZ;
extern uint8_t  LORA_SF;
extern uint32_t LORA_BW_HZ;
extern uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
extern CeilingCounter hbCounter;

// Чете настройките от NVS (namespace "cfg"), записани от config_esp32 (stage 1):
//   id, freq (RX), freq_tx (ключ "freqtx"), sf, bw, key
void loadConfigFromNvs();

#endif
