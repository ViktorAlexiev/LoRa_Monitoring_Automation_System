// repeater/config_storage.cpp (ESP32 - NVS). Реалният файл, срещу фалшив Preferences.
#include "../test_framework.h"
#include "esp32_test_helpers.h"
#include "config_storage.h"

static void setKey() { nvsSetBytes("key", TEST_KEY, 16); }

TEST(loads_rx_and_tx_frequencies_separately) {
  resetFakes();
  nvsSetStr("id", "R001");
  nvsSetU32("freq", 434000000UL);       // RX (от Sensor)
  nvsSetU32("freqtx", 433000000UL);     // TX (към Gateway)
  nvsSetU8("sf", 8);
  nvsSetU32("bw", 125000UL);
  setKey();
  loadConfigFromNvs();
  ASSERT_TRUE(std::string(REPEATER_ID) == "R001");
  ASSERT_EQ((long)LORA_FREQ_RX_HZ, 434000000L);
  ASSERT_EQ((long)LORA_FREQ_TX_HZ, 433000000L);
  ASSERT_EQ((int)LORA_SF, 8);
  ASSERT_MEM_EQ(NETWORK_KEY, TEST_KEY, 16);
}

TEST(each_frequency_has_its_own_fallback) {
  resetFakes();
  nvsSetU32("freq", 435000000UL);        // само RX е зададена
  loadConfigFromNvs();
  ASSERT_EQ((long)LORA_FREQ_RX_HZ, 435000000L);
  ASSERT_EQ((long)LORA_FREQ_TX_HZ, (long)LORA_FREQ_TX_DEFAULT_HZ);

  resetFakes();
  nvsSetU32("freqtx", 432000000UL);      // само TX е зададена
  loadConfigFromNvs();
  ASSERT_EQ((long)LORA_FREQ_RX_HZ, (long)LORA_FREQ_RX_DEFAULT_HZ);
  ASSERT_EQ((long)LORA_FREQ_TX_HZ, 432000000L);
}

TEST(invalid_radio_params_fall_back) {
  resetFakes();
  nvsSetU8("sf", 6);
  nvsSetU32("bw", 500000UL);
  loadConfigFromNvs();
  ASSERT_EQ((int)LORA_SF, (int)RADIO_SF_DEFAULT);
  ASSERT_EQ((long)LORA_BW_HZ, (long)RADIO_BW_DEFAULT_HZ);
}

TEST(hb_counter_starts_from_nvs_ceiling) {
  resetFakes();
  nvsSetU32("ceil", 1234);
  loadConfigFromNvs();
  ASSERT_EQ((long)hbCounter.next(), 1234L);
}

int main() { return runAllTests(); }
