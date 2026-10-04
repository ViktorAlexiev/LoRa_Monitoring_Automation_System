#include "config_storage.h"
#include <EEPROM.h>

char REPEATER_ID[MODULE_ID_LEN + 1];
uint32_t LORA_FREQ_RX_HZ = LORA_FREQ_RX_DEFAULT_HZ;
uint32_t LORA_FREQ_TX_HZ = LORA_FREQ_TX_DEFAULT_HZ;
uint8_t  LORA_SF = RADIO_SF_DEFAULT;
uint32_t LORA_BW_HZ = RADIO_BW_DEFAULT_HZ;
uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
CeilingCounter hbCounter;

void loadConfigFromEeprom() {
  int addr = EEPROM_ADDR_MODULE_ID;
  for (int i = 0; i < MODULE_ID_LEN; i++) REPEATER_ID[i] = EEPROM.read(addr++);
  REPEATER_ID[MODULE_ID_LEN] = 0;

  uint32_t storedRx, storedTx;
  EEPROM.get(EEPROM_ADDR_FREQUENCY, storedRx);
  LORA_FREQ_RX_HZ = (storedRx == 0 || storedRx == 0xFFFFFFFFUL) ? LORA_FREQ_RX_DEFAULT_HZ : storedRx;

  EEPROM.get(EEPROM_ADDR_FREQUENCY_TX, storedTx);
  LORA_FREQ_TX_HZ = (storedTx == 0 || storedTx == 0xFFFFFFFFUL) ? LORA_FREQ_TX_DEFAULT_HZ : storedTx;

  uint8_t sf = EEPROM.read(EEPROM_ADDR_SF);
  LORA_SF = radioSfValid(sf) ? sf : RADIO_SF_DEFAULT;
  uint32_t bw = radioBwFromIndex(EEPROM.read(EEPROM_ADDR_BW_IDX));
  LORA_BW_HZ = bw ? bw : RADIO_BW_DEFAULT_HZ;

  addr = EEPROM_ADDR_KEY;
  for (uint8_t i = 0; i < CRYPTO_KEY_LEN; i++) NETWORK_KEY[i] = EEPROM.read(addr++);

  hbCounter.begin(EEPROM_ADDR_CEILING);
}
