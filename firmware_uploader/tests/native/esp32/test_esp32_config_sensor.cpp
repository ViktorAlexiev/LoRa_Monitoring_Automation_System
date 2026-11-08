// sensor/config_storage.cpp (ESP32 - NVS). Реалният файл, срещу фалшив Preferences.
#include "../test_framework.h"
#include "esp32_test_helpers.h"
#include "config_storage.h"

static void setKey() { nvsSetBytes("key", TEST_KEY, 16); }

TEST(loads_all_fields_from_nvs) {
  resetFakes();
  nvsSetStr("id", "S001");
  nvsSetU32("freq", 433500000UL);
  nvsSetU8("sf", 9);
  nvsSetU32("bw", 250000UL);
  setKey();
  g_nvsWrites = 0;
  loadConfigFromNvs();
  ASSERT_TRUE(std::string(SENSOR_ID) == "S001");
  ASSERT_EQ((long)LORA_FREQ_HZ, 433500000L);
  ASSERT_EQ((int)LORA_SF, 9);
  ASSERT_EQ((long)LORA_BW_HZ, 250000L);
  ASSERT_MEM_EQ(NETWORK_KEY, TEST_KEY, 16);
  ASSERT_EQ(g_nvsReadOnlyWriteAttempts, 0);
  ASSERT_EQ((long)txCounter.next(), 0L);        // броячът е инициализиран от "ceil"
}

TEST(defaults_when_nvs_is_empty) {
  resetFakes();
  loadConfigFromNvs();
  ASSERT_TRUE(std::string(SENSOR_ID) == "");
  ASSERT_EQ((long)LORA_FREQ_HZ, (long)LORA_FREQ_DEFAULT_HZ);
  ASSERT_EQ((int)LORA_SF, (int)RADIO_SF_DEFAULT);
  ASSERT_EQ((long)LORA_BW_HZ, (long)RADIO_BW_DEFAULT_HZ);
  ASSERT_TRUE(contains(g_serialOut, "[WARN]"));  // липсва ключ -> предупреждение
}

TEST(invalid_sf_bw_freq_fall_back_to_defaults) {
  resetFakes();
  nvsSetStr("id", "S002");
  nvsSetU32("freq", 0);
  nvsSetU8("sf", 13);            // невалиден SF
  nvsSetU32("bw", 100000UL);     // неподдържан BW
  setKey();
  loadConfigFromNvs();
  ASSERT_EQ((long)LORA_FREQ_HZ, (long)LORA_FREQ_DEFAULT_HZ);
  ASSERT_EQ((int)LORA_SF, (int)RADIO_SF_DEFAULT);
  ASSERT_EQ((long)LORA_BW_HZ, (long)RADIO_BW_DEFAULT_HZ);
}

TEST(every_valid_sf_and_bw_is_accepted) {
  uint32_t bws[3] = {62500, 125000, 250000};
  for (int sf = 7; sf <= 12; sf++) {
    for (int b = 0; b < 3; b++) {
      resetFakes();
      nvsSetU8("sf", (uint8_t)sf);
      nvsSetU32("bw", bws[b]);
      loadConfigFromNvs();
      ASSERT_EQ((int)LORA_SF, sf);
      ASSERT_EQ((long)LORA_BW_HZ, (long)bws[b]);
    }
  }
}

TEST(too_long_id_is_truncated_to_6_chars) {
  resetFakes();
  nvsSetStr("id", "ABCDEFGHIJ");
  setKey();
  loadConfigFromNvs();
  ASSERT_TRUE(std::string(SENSOR_ID) == "ABCDEF");
}

TEST(missing_key_prints_warning_but_key_present_does_not) {
  resetFakes();
  nvsSetStr("id", "S001");
  loadConfigFromNvs();
  ASSERT_TRUE(contains(g_serialOut, "[WARN]"));
  resetFakes();
  nvsSetStr("id", "S001");
  setKey();
  loadConfigFromNvs();
  ASSERT_TRUE(!contains(g_serialOut, "[WARN]"));
}

int main() { return runAllTests(); }
