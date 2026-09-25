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


// ---------------- SF / BW от EEPROM (адреси 95 / 96) ----------------
TEST(repeater_sf_bw_defaults_on_erased_eeprom) {
  eeprom_reset_erased();   // 0xFF на 95 и 96
  loadConfigFromEeprom();
  ASSERT_EQ(LORA_SF, (uint8_t)7);
  ASSERT_EQ(LORA_BW_HZ, 125000UL);
}

TEST(repeater_sf_bw_read_valid_values) {
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

TEST(repeater_sf_bw_invalid_values_fall_back_to_defaults) {
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

TEST(repeater_sf_bw_addresses_do_not_overlap_ceiling_or_key) {
  // ceiling е на 91..94, ключът на 75..90 - новите байтове 95/96 не бива да ги засягат
  ASSERT_TRUE(EEPROM_ADDR_SF > EEPROM_ADDR_CEILING + 3);
  ASSERT_EQ(EEPROM_ADDR_SF, 95);
  ASSERT_EQ(EEPROM_ADDR_BW_IDX, 96);
}

int main() {
  return runAllTests();
}
