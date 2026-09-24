// Тества РЕАЛНИЯ firmware_src/repeater/config_storage.cpp - две отделни честоти
// (RX/TX), всяка с независим default fallback.
#include "test_framework.h"
#include "config_storage.h"
#include "EEPROM.h"
#include <cstring>

TEST(repeater_load_config_reads_id_and_both_freqs) {
  eeprom_reset_erased();
  const char *id = "RP0001";
  for (int i = 0; i < MODULE_ID_LEN; i++) EEPROM.write(EEPROM_ADDR_MODULE_ID + i, (uint8_t)id[i]);
  EEPROM.put(EEPROM_ADDR_FREQUENCY, (uint32_t)434200000UL);
  EEPROM.put(EEPROM_ADDR_FREQUENCY_TX, (uint32_t)433200000UL);
  uint8_t key[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)(0x50 + i);
  for (int i = 0; i < 16; i++) EEPROM.write(EEPROM_ADDR_KEY + i, key[i]);

  loadConfigFromEeprom();

  ASSERT_TRUE(strncmp(REPEATER_ID, id, 6) == 0);
  ASSERT_EQ(LORA_FREQ_RX_HZ, 434200000UL);
  ASSERT_EQ(LORA_FREQ_TX_HZ, 433200000UL);
  ASSERT_MEM_EQ(NETWORK_KEY, key, 16);
}

TEST(repeater_rx_and_tx_defaults_independent) {
  eeprom_reset_erased();
  const char *id = "RP0002";
  for (int i = 0; i < MODULE_ID_LEN; i++) EEPROM.write(EEPROM_ADDR_MODULE_ID + i, (uint8_t)id[i]);
  // само TX честотата е записана валидно, RX остава erased (0xFFFFFFFF)
  EEPROM.put(EEPROM_ADDR_FREQUENCY_TX, (uint32_t)433000000UL);

  loadConfigFromEeprom();

  ASSERT_EQ(LORA_FREQ_RX_HZ, (uint32_t)LORA_FREQ_RX_DEFAULT_HZ); // 434 MHz default
  ASSERT_EQ(LORA_FREQ_TX_HZ, 433000000UL); // реално записаната, не default
}

TEST(repeater_rx_default_is_434_tx_default_is_433) {
  // документираща инварианта - RX (от Sensor) и TX (към Gateway) НЕ трябва да се бъркат
  ASSERT_EQ((uint32_t)LORA_FREQ_RX_DEFAULT_HZ, 434000000UL);
  ASSERT_EQ((uint32_t)LORA_FREQ_TX_DEFAULT_HZ, 433000000UL);
}

int main() {
  return runAllTests();
}
