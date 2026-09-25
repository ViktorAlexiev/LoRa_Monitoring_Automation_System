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


// ---------------- SF / BW от EEPROM (адреси 95 / 96) ----------------
TEST(sensor_sf_bw_defaults_on_erased_eeprom) {
  eeprom_reset_erased();   // 0xFF на 95 и 96
  loadConfigFromEeprom();
  ASSERT_EQ(LORA_SF, (uint8_t)7);
  ASSERT_EQ(LORA_BW_HZ, 125000UL);
}

TEST(sensor_sf_bw_read_valid_values) {
  eeprom_reset_erased();
  EEPROM.write(EEPROM_ADDR_SF, 10);
  EEPROM.write(EEPROM_ADDR_BW_IDX, 2);   // 250 kHz
  loadConfigFromEeprom();
  ASSERT_EQ(LORA_SF, (uint8_t)10);
  ASSERT_EQ(LORA_BW_HZ, 250000UL);

  EEPROM.write(EEPROM_ADDR_SF, 12);
  EEPROM.write(EEPROM_ADDR_BW_IDX, 0);   // 62.5 kHz
  loadConfigFromEeprom();
  ASSERT_EQ(LORA_SF, (uint8_t)12);
  ASSERT_EQ(LORA_BW_HZ, 62500UL);
}

TEST(sensor_sf_bw_invalid_values_fall_back_to_defaults) {
  const uint8_t badSf[] = {0, 6, 13, 200};
  for (uint8_t bad : badSf) {
    eeprom_reset_erased();
    EEPROM.write(EEPROM_ADDR_SF, bad);
    loadConfigFromEeprom();
    ASSERT_EQ(LORA_SF, (uint8_t)7);
  }
  const uint8_t badBw[] = {3, 4, 100};
  for (uint8_t bad : badBw) {
    eeprom_reset_erased();
    EEPROM.write(EEPROM_ADDR_BW_IDX, bad);
    loadConfigFromEeprom();
    ASSERT_EQ(LORA_BW_HZ, 125000UL);
  }
}

TEST(sensor_sf_bw_addresses_do_not_overlap_ceiling_or_key) {
  // ceiling е на 91..94, ключът на 75..90 - новите байтове 95/96 не бива да ги засягат
  ASSERT_TRUE(EEPROM_ADDR_SF > EEPROM_ADDR_CEILING + 3);
  ASSERT_EQ(EEPROM_ADDR_SF, 95);
  ASSERT_EQ(EEPROM_ADDR_BW_IDX, 96);
}

int main() {
  return runAllTests();
}
