// executor_wifi/config_storage.cpp (NVS). Реалният файл, срещу фалшив Preferences.
#include "../test_framework.h"
#include "esp32_test_helpers.h"
#include "config_storage.h"

TEST(loads_wifi_mqtt_and_consumers) {
  resetFakes();
  nvsSetStr("id", "E007");
  nvsSetU8("nc", 2);
  nvsSetStr("c0i", "V01"); nvsSetU8("c0p", 25);
  nvsSetStr("c1i", "P01"); nvsSetU8("c1p", 26);
  nvsSetStr("wssid", "FarmNet");
  nvsSetStr("wpass", "secret123");
  nvsSetStr("mqip", "192.168.1.10");
  nvsSetStr("mqport", "8883");
  nvsSetStr("mquser", "agro");
  nvsSetStr("mqpass", "pw");
  loadConfigFromNvs();
  ASSERT_TRUE(std::string(MY_M_ID) == "E007");
  ASSERT_EQ((int)NUM_CONSUMERS, 2);
  ASSERT_EQ((int)consumerPins[1], 26);
  ASSERT_TRUE(std::string(WIFI_SSID_BUF) == "FarmNet");
  ASSERT_TRUE(std::string(WIFI_PASSWORD_BUF) == "secret123");
  ASSERT_TRUE(std::string(MQTT_IP_BUF) == "192.168.1.10");
  ASSERT_EQ((int)MQTT_PORT_VAL, 8883);
  ASSERT_TRUE(std::string(MQTT_USER_BUF) == "agro");
  ASSERT_TRUE(std::string(MQTT_PASSWORD_BUF) == "pw");
}

TEST(mqtt_port_defaults_to_1883_when_missing_or_invalid) {
  resetFakes();
  loadConfigFromNvs();
  ASSERT_EQ((int)MQTT_PORT_VAL, 1883);
  resetFakes();
  nvsSetStr("mqport", "abc");
  loadConfigFromNvs();
  ASSERT_EQ((int)MQTT_PORT_VAL, 1883);
  resetFakes();
  nvsSetStr("mqport", "0");
  loadConfigFromNvs();
  ASSERT_EQ((int)MQTT_PORT_VAL, 1883);
}

TEST(consumer_lookup_by_id) {
  resetFakes();
  nvsSetU8("nc", 3);
  nvsSetStr("c0i", "V01"); nvsSetU8("c0p", 25);
  nvsSetStr("c1i", "V02"); nvsSetU8("c1p", 26);
  nvsSetStr("c2i", "P01"); nvsSetU8("c2p", 27);
  loadConfigFromNvs();
  ASSERT_EQ(consumerIndex("V01"), 0);
  ASSERT_EQ(consumerIndex("P01"), 2);
  ASSERT_EQ(consumerIndex("V99"), -1);
  ASSERT_EQ(consumerIndex(""), -1);
}

TEST(no_user_means_empty_credentials) {
  resetFakes();
  nvsSetStr("mqip", "broker.local");
  loadConfigFromNvs();
  ASSERT_TRUE(std::string(MQTT_USER_BUF) == "");
  ASSERT_TRUE(std::string(MQTT_PASSWORD_BUF) == "");
}

TEST(clamps_consumer_count_over_ten) {
  resetFakes();
  nvsSetU8("nc", 12);
  loadConfigFromNvs();
  ASSERT_EQ((int)NUM_CONSUMERS, 0);
}

int main() { return runAllTests(); }
