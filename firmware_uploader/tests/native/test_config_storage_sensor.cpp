// Тества РЕАЛНИЯ firmware_src/sensor/config_storage.cpp - EEPROM round-trip + default fallback.
#include "test_framework.h"
#include "config_storage.h"
#include "EEPROM.h"
#include <cstring>

TEST(sensor_load_config_reads_id_and_freq) {
  eeprom_reset_erased();
  const char *id = "SN0001";
  for (int i = 0; i < MODULE_ID_LEN; i++) EEPROM.write(EEPROM_ADDR_MODULE_ID + i, (uint8_t)id[i]);
  uint32_t freq = 433500000UL;
  EEPROM.put(EEPROM_ADDR_FREQUENCY, freq);
  uint8_t key[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
  for (int i = 0; i < 16; i++) EEPROM.write(EEPROM_ADDR_KEY + i, key[i]);

  loadConfigFromEeprom();

  ASSERT_TRUE(strncmp(SENSOR_ID, id, 6) == 0);
  ASSERT_EQ(LORA_FREQ_HZ, freq);
  ASSERT_MEM_EQ(NETWORK_KEY, key, 16);
}

TEST(sensor_load_config_default_freq_on_erased_eeprom) {
  eeprom_reset_erased();
  const char *id = "SN0002";
  for (int i = 0; i < MODULE_ID_LEN; i++) EEPROM.write(EEPROM_ADDR_MODULE_ID + i, (uint8_t)id[i]);
  // честотното поле остава 0xFFFFFFFF (erased) -> трябва да падне на default

  loadConfigFromEeprom();
  ASSERT_EQ(LORA_FREQ_HZ, (uint32_t)LORA_FREQ_DEFAULT_HZ);
  ASSERT_EQ((uint32_t)LORA_FREQ_DEFAULT_HZ, 433000000UL); // документира конкретната стойност след поправката (433, не 434)
}

TEST(sensor_load_config_freq_zero_falls_back) {
  eeprom_reset_erased();
  const char *id = "SN0003";
  for (int i = 0; i < MODULE_ID_LEN; i++) EEPROM.write(EEPROM_ADDR_MODULE_ID + i, (uint8_t)id[i]);
  EEPROM.put(EEPROM_ADDR_FREQUENCY, (uint32_t)0);

  loadConfigFromEeprom();
  ASSERT_EQ(LORA_FREQ_HZ, (uint32_t)LORA_FREQ_DEFAULT_HZ);
}

int main() {
  return runAllTests();
}
