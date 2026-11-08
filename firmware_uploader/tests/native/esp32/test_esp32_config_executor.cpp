// executor/config_storage.cpp (ESP32 - NVS). Реалният файл, срещу фалшив Preferences.
#include "../test_framework.h"
#include "esp32_test_helpers.h"
#include "config_storage.h"

static void setKey() { nvsSetBytes("key", TEST_KEY, 16); }

static void setConsumer(int i, const char *id, uint8_t pin) {
  char kI[8], kP[8];
  snprintf(kI, sizeof(kI), "c%di", i);
  snprintf(kP, sizeof(kP), "c%dp", i);
  nvsSetStr(kI, id);
  nvsSetU8(kP, pin);
}

TEST(loads_consumers_pins_and_radio_settings) {
  resetFakes();
  nvsSetStr("id", "E001");
  nvsSetU8("nc", 3);
  setConsumer(0, "V01", 25);
  setConsumer(1, "V02", 26);
  setConsumer(2, "P01", 33);
  nvsSetU32("freq", 433000000UL);
  nvsSetU8("sf", 7);
  nvsSetU32("bw", 125000UL);
  setKey();
  loadConfigFromNvs();
  ASSERT_TRUE(std::string(MY_M_ID) == "E001");
  ASSERT_EQ((int)NUM_CONSUMERS, 3);
  ASSERT_TRUE(std::string(consumerList[0]) == "V01");
  ASSERT_TRUE(std::string(consumerList[2]) == "P01");
  ASSERT_EQ((int)consumerPins[0], 25);
  ASSERT_EQ((int)consumerPins[1], 26);
  ASSERT_EQ((int)consumerPins[2], 33);
  ASSERT_EQ((long)LORA_FREQ_HZ, 433000000L);
  ASSERT_MEM_EQ(NETWORK_KEY, TEST_KEY, 16);
}

TEST(ten_consumers_ok_more_than_ten_is_clamped_to_zero) {
  resetFakes();
  nvsSetU8("nc", 10);
  for (int i = 0; i < 10; i++) setConsumer(i, "C01", (uint8_t)(10 + i));
  loadConfigFromNvs();
  ASSERT_EQ((int)NUM_CONSUMERS, 10);
  ASSERT_EQ((int)consumerPins[9], 19);

  resetFakes();
  nvsSetU8("nc", 11);                      // повредена стойност
  loadConfigFromNvs();
  ASSERT_EQ((int)NUM_CONSUMERS, 0);
}

TEST(no_consumers_by_default_and_defaults_for_radio) {
  resetFakes();
  loadConfigFromNvs();
  ASSERT_EQ((int)NUM_CONSUMERS, 0);
  ASSERT_EQ((long)LORA_FREQ_HZ, (long)LORA_FREQ_DEFAULT_HZ);
  ASSERT_EQ((int)LORA_SF, (int)RADIO_SF_DEFAULT);
}

TEST(pin_from_string_is_gpio_number) {
  ASSERT_EQ((int)pinFromString("25"), 25);
  ASSERT_EQ((int)pinFromString("5"), 5);
  ASSERT_EQ((int)pinFromString("39"), 39);
}

TEST(shared_nonce_counter_starts_from_nvs) {
  resetFakes();
  nvsSetU32("ceil", 700);
  loadConfigFromNvs();
  ASSERT_EQ((long)txCounter.next(), 700L);
}

int main() { return runAllTests(); }
