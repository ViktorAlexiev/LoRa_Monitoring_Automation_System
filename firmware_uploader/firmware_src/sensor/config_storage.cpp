#include "config_storage.h"
#include <EEPROM.h>

char SENSOR_ID[MODULE_ID_LEN + 1];
uint32_t LORA_FREQ_HZ = LORA_FREQ_DEFAULT_HZ;
uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
CeilingCounter txCounter;

void loadConfigFromEeprom() {
  int addr = EEPROM_ADDR_MODULE_ID;
  for (int i = 0; i < MODULE_ID_LEN; i++) SENSOR_ID[i] = EEPROM.read(addr++);
  SENSOR_ID[MODULE_ID_LEN] = 0;

  uint32_t storedFreq;
  EEPROM.get(EEPROM_ADDR_FREQUENCY, storedFreq);
  // защита: 0 (никога не е писано) или 0xFFFFFFFF (изтрит/неинициализиран EEPROM) -> default
  if (storedFreq == 0 || storedFreq == 0xFFFFFFFFUL) {
    LORA_FREQ_HZ = LORA_FREQ_DEFAULT_HZ;
  } else {
    LORA_FREQ_HZ = storedFreq;
  }

  addr = EEPROM_ADDR_KEY;
  for (uint8_t i = 0; i < CRYPTO_KEY_LEN; i++) NETWORK_KEY[i] = EEPROM.read(addr++);

  txCounter.begin(EEPROM_ADDR_CEILING);
}
