#include "config_storage.h"
#include <EEPROM.h>

char MY_M_ID[MODULE_ID_LEN + 1];
char consumerList[MAX_CONSUMERS][CONSUMER_ID_LEN + 1];
uint8_t consumerPins[MAX_CONSUMERS];
uint8_t NUM_CONSUMERS = 0;
uint32_t LORA_FREQ_HZ = LORA_FREQ_DEFAULT_HZ;
uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
CeilingCounter txCounter;

uint8_t pinFromString(const char *s) {
  if (s[0] >= 'A' && s[0] <= 'Z' && s[1] != 0) {
    return A0 + atoi(s + 1);   // буква+число -> аналогов пин
  }
  return (uint8_t)atoi(s);      // чисто число -> цифров пин
}

void loadConfigFromEeprom() {
  int addr = EEPROM_ADDR_MODULE_ID;
  for (int i = 0; i < MODULE_ID_LEN; i++) MY_M_ID[i] = EEPROM.read(addr++);
  MY_M_ID[MODULE_ID_LEN] = 0;

  NUM_CONSUMERS = EEPROM.read(EEPROM_ADDR_NUM_CONSUMERS);
  if (NUM_CONSUMERS > MAX_CONSUMERS) NUM_CONSUMERS = 0;

  addr = EEPROM_ADDR_CONSUMERS;
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    for (int j = 0; j < CONSUMER_ID_LEN; j++) consumerList[i][j] = EEPROM.read(addr++);
    consumerList[i][CONSUMER_ID_LEN] = 0;

    char pinStr[PIN_LEN + 1];
    for (int j = 0; j < PIN_LEN; j++) pinStr[j] = EEPROM.read(addr++);
    pinStr[PIN_LEN] = 0;
    consumerPins[i] = pinFromString(pinStr);
  }

  uint32_t storedFreq;
  EEPROM.get(EEPROM_ADDR_FREQUENCY, storedFreq);
  if (storedFreq == 0 || storedFreq == 0xFFFFFFFFUL) {
    LORA_FREQ_HZ = LORA_FREQ_DEFAULT_HZ;
  } else {
    LORA_FREQ_HZ = storedFreq;
  }

  addr = EEPROM_ADDR_KEY;
  for (uint8_t i = 0; i < CRYPTO_KEY_LEN; i++) NETWORK_KEY[i] = EEPROM.read(addr++);

  txCounter.begin(EEPROM_ADDR_CEILING);
}
