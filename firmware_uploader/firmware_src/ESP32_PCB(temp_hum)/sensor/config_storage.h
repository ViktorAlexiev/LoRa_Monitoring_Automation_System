#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>
#include "crypto_common.h"
#include "ceiling_counter.h"
#include "radio_timing.h"

#define MODULE_ID_LEN           6

// Default честота - fallback ако NVS е празен. Честотата "до Gateway" (не "Sensor->Repeater") -
// по-безопасен fallback: Sensor поне има шанс да достигне директно Gateway.
#define LORA_FREQ_DEFAULT_HZ 433000000UL

extern char SENSOR_ID[MODULE_ID_LEN + 1];
extern uint32_t LORA_FREQ_HZ;
extern uint8_t  LORA_SF;
extern uint32_t LORA_BW_HZ;
extern uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
extern CeilingCounter txCounter;

// Чете настройките от NVS (namespace "cfg"), записани от config_esp32 (stage 1):
//   id (string), freq (ulong Hz), sf (uchar), bw (ulong Hz), key (16 байта)
void loadConfigFromNvs();

#endif
