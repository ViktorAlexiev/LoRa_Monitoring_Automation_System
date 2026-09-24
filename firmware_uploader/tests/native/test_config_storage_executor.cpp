// Тества РЕАЛНИЯ firmware_src/executor/config_storage.cpp: pinFromString() и
// пълния round-trip на loadConfigFromEeprom() срещу заместена EEPROM.
#include "test_framework.h"
#include "config_storage.h"
#include "EEPROM.h"
#include <cstring>

TEST(pin_from_string_numeric) {
  ASSERT_EQ(pinFromString("10"), (uint8_t)10);
  ASSERT_EQ(pinFromString("5"), (uint8_t)5);
  ASSERT_EQ(pinFromString("0"), (uint8_t)0);
  ASSERT_EQ(pinFromString("99"), (uint8_t)99);
}

TEST(pin_from_string_analog) {
  ASSERT_EQ(pinFromString("A3"), (uint8_t)(A0 + 3));
  ASSERT_EQ(pinFromString("A0"), (uint8_t)(A0 + 0));
  ASSERT_EQ(pinFromString("A9"), (uint8_t)(A0 + 9));
}

TEST(pin_from_string_single_char_letter_not_treated_as_analog) {
  // s[1] трябва да е != 0 за аналогов клон; единичен символ "A" (терминиран на s[1])
  // пада в else клона -> atoi("A") == 0
  ASSERT_EQ(pinFromString("A"), (uint8_t)0);
}

// ---------------- loadConfigFromEeprom() пълен round-trip ----------------
static void writeModuleIdAndFreqAndKey(const char *id, uint32_t freq, const uint8_t *key) {
  int addr = EEPROM_ADDR_MODULE_ID;
  for (int i = 0; i < MODULE_ID_LEN; i++) EEPROM.write(addr++, (uint8_t)id[i]);
  EEPROM.put(EEPROM_ADDR_FREQUENCY, freq);
  addr = EEPROM_ADDR_KEY;
  for (int i = 0; i < CRYPTO_KEY_LEN; i++) EEPROM.write(addr++, key[i]);
}

TEST(load_config_reads_id_freq_key_and_zero_consumers) {
  eeprom_reset_erased();
  uint8_t key[16]; for (int i=0;i<16;i++) key[i] = (uint8_t)(0x10+i);
  writeModuleIdAndFreqAndKey("CS0001", 434000000UL, key);
  EEPROM.write(EEPROM_ADDR_NUM_CONSUMERS, 0);

  loadConfigFromEeprom();

  ASSERT_TRUE(strncmp(MY_M_ID, "CS0001", 6) == 0);
  ASSERT_EQ(LORA_FREQ_HZ, 434000000UL);
  ASSERT_EQ(NUM_CONSUMERS, (uint8_t)0);
  ASSERT_MEM_EQ(NETWORK_KEY, key, 16);
}

TEST(load_config_freq_zero_falls_back_to_default) {
  eeprom_reset_erased();
  uint8_t key[16] = {0};
  writeModuleIdAndFreqAndKey("EX0002", 0, key); // freq=0 -> fallback
  EEPROM.write(EEPROM_ADDR_NUM_CONSUMERS, 0);

  loadConfigFromEeprom();
  ASSERT_EQ(LORA_FREQ_HZ, (uint32_t)LORA_FREQ_DEFAULT_HZ);
}

TEST(load_config_freq_0xFFFFFFFF_falls_back_to_default) {
  eeprom_reset_erased(); // цялата EEPROM вече е 0xFF, вкл. честотното поле
  uint8_t key[16] = {0};
  writeModuleIdAndFreqAndKey("EX0003", 0xFFFFFFFFUL, key);
  EEPROM.write(EEPROM_ADDR_NUM_CONSUMERS, 0);

  loadConfigFromEeprom();
  ASSERT_EQ(LORA_FREQ_HZ, (uint32_t)LORA_FREQ_DEFAULT_HZ);
}

TEST(load_config_reads_consumers_with_mixed_pin_types) {
  eeprom_reset_erased();
  uint8_t key[16] = {0};
  writeModuleIdAndFreqAndKey("EX0004", 433000000UL, key);

  // 3 консуматора: числов пин "10", аналогов "A3", числов "07"
  EEPROM.write(EEPROM_ADDR_NUM_CONSUMERS, 3);
  int addr = EEPROM_ADDR_CONSUMERS;
  auto writeConsumer = [&](const char *id, const char *pin) {
    for (int i = 0; i < CONSUMER_ID_LEN; i++) EEPROM.write(addr++, (uint8_t)id[i]);
    for (int i = 0; i < PIN_LEN; i++) EEPROM.write(addr++, (uint8_t)pin[i]);
  };
  writeConsumer("B1C2", "10");
  writeConsumer("D4E5", "A3");
  writeConsumer("F6A7", "07");

  loadConfigFromEeprom();

  ASSERT_EQ(NUM_CONSUMERS, (uint8_t)3);
  ASSERT_TRUE(strncmp(consumerList[0], "B1C2", 4) == 0);
  ASSERT_EQ(consumerPins[0], (uint8_t)10);
  ASSERT_TRUE(strncmp(consumerList[1], "D4E5", 4) == 0);
  ASSERT_EQ(consumerPins[1], (uint8_t)(A0 + 3));
  ASSERT_TRUE(strncmp(consumerList[2], "F6A7", 4) == 0);
  ASSERT_EQ(consumerPins[2], (uint8_t)7);
}

TEST(load_config_clamps_num_consumers_above_max_to_zero) {
  eeprom_reset_erased();
  uint8_t key[16] = {0};
  writeModuleIdAndFreqAndKey("EX0005", 433000000UL, key);
  EEPROM.write(EEPROM_ADDR_NUM_CONSUMERS, 200); // явно невалидна стойност (> MAX_CONSUMERS=10)

  loadConfigFromEeprom();
  ASSERT_EQ(NUM_CONSUMERS, (uint8_t)0); // защитна клампа в кода
}

int main() {
  return runAllTests();
}
