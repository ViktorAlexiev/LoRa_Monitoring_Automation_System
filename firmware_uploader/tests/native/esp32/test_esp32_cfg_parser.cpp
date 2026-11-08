// config_esp32.ino - stage-1 конфигурационният firmware за всички ESP32 устройства. Включва се
// директно РЕАЛНИЯТ .ino файл (компилира се като C++), срещу фалшив Preferences/Serial: подава се
// точно това CFG:{...} съобщение, което праща Firmware Uploader-ът, и се проверява какво се
// записва в NVS и какъв отговор (ACK / NACK:причина) излиза по серийния порт.
#include "../test_framework.h"
#include "esp32_test_helpers.h"

// прототипи (Arduino IDE ги генерира автоматично за .ino; в чист C++ са нужни ръчно)
void waitForConfigPacket();
bool extractStringField(const String &src, const char *key, String &out);
bool hexToBytes(const String &hex, uint8_t *out, uint8_t outLen);
void handleConfigLine(const String &line);
void sendAck();
void sendNack(const char *reason);

#include "config_esp32/config_esp32.ino"   // -I <папката с firmware> (виж run_esp32.sh)

static const char *KEY_HEX = "00112233445566778899AABBCCDDEEFF";

// пуска един CFG ред и връща серийния отговор
static std::string cfg(const std::string &json) {
  g_serialOut.clear();
  handleConfigLine(String(("CFG:" + json).c_str()));
  return g_serialOut;
}
static bool isAck(const std::string &r) { return r == "ACK\n"; }
static bool isNack(const std::string &r) { return r.rfind("NACK:", 0) == 0; }

TEST(sensor_config_is_acked_and_written_to_nvs) {
  resetFakes();
  std::string r = cfg(std::string("{\"id\":\"S001\",\"key\":\"") + KEY_HEX +
                      "\",\"sf\":\"7\",\"bw\":\"125000\",\"freq\":\"433000000\"}");
  ASSERT_TRUE(isAck(r));
  ASSERT_TRUE(nvsGetStr("id") == "S001");
  ASSERT_EQ((long)nvsGetNum("freq"), 433000000L);
  ASSERT_EQ((long)nvsGetNum("sf"), 7L);
  ASSERT_EQ((long)nvsGetNum("bw"), 125000L);
  std::vector<uint8_t> key = nvsGetBytes("key");
  ASSERT_EQ((int)key.size(), 16);
  ASSERT_MEM_EQ(key.data(), TEST_KEY, 16);
  ASSERT_TRUE(!nvsHas("nc"));          // сензор: без консуматори
  ASSERT_TRUE(!nvsHas("wssid"));       // и без Wi-Fi
}

TEST(repeater_config_has_two_frequencies) {
  resetFakes();
  std::string r = cfg(std::string("{\"id\":\"R001\",\"key\":\"") + KEY_HEX +
                      "\",\"sf\":\"9\",\"bw\":\"250000\",\"freq\":\"434000000\",\"freq_tx\":\"433000000\"}");
  ASSERT_TRUE(isAck(r));
  ASSERT_EQ((long)nvsGetNum("freq"), 434000000L);
  ASSERT_EQ((long)nvsGetNum("freqtx"), 433000000L);
  ASSERT_EQ((long)nvsGetNum("sf"), 9L);
}

TEST(executor_consumers_are_stored_with_gpio_pins) {
  resetFakes();
  std::string r = cfg(std::string("{\"id\":\"E001\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"25\"},"
                                  "{\"id\":\"V02\",\"pin\":\"26\"},{\"id\":\"P01\",\"pin\":\"33\"}],"
                                  "\"freq\":\"433000000\",\"key\":\"") + KEY_HEX + "\",\"sf\":\"7\",\"bw\":\"125000\"}");
  ASSERT_TRUE(isAck(r));
  ASSERT_EQ((long)nvsGetNum("nc"), 3L);
  ASSERT_TRUE(nvsGetStr("c0i") == "V01");
  ASSERT_EQ((long)nvsGetNum("c0p"), 25L);
  ASSERT_TRUE(nvsGetStr("c1i") == "V02");
  ASSERT_EQ((long)nvsGetNum("c1p"), 26L);
  ASSERT_TRUE(nvsGetStr("c2i") == "P01");
  ASSERT_EQ((long)nvsGetNum("c2p"), 33L);
}

TEST(ten_consumers_accepted_eleventh_is_ignored) {
  resetFakes();
  std::string arr = "[";
  for (int i = 0; i < 11; i++) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s{\"id\":\"C%02d\",\"pin\":\"%d\"}", i ? "," : "", i + 1, 10 + i);
    arr += buf;
  }
  arr += "]";
  std::string r = cfg("{\"id\":\"E002\",\"consumers\":" + arr + "}");
  ASSERT_TRUE(isAck(r));
  ASSERT_EQ((long)nvsGetNum("nc"), 10L);
  ASSERT_TRUE(nvsGetStr("c9i") == "C10");
  ASSERT_TRUE(!nvsHas("c10i"));
}

TEST(gateway_net_config_stores_wifi_and_mqtt) {
  resetFakes();
  std::string r = cfg(std::string("{\"id\":\"G001\",\"key\":\"") + KEY_HEX +
                      "\",\"sf\":\"7\",\"bw\":\"125000\",\"net\":\"1\",\"wifi_ssid\":\"FarmNet\","
                      "\"wifi_password\":\"secret\",\"mqtt_ip\":\"192.168.1.10\",\"mqtt_port\":\"1883\","
                      "\"mqtt_user\":\"agro\",\"mqtt_password\":\"pw\",\"freq\":\"433000000\"}");
  ASSERT_TRUE(isAck(r));
  ASSERT_TRUE(nvsGetStr("wssid") == "FarmNet");
  ASSERT_TRUE(nvsGetStr("wpass") == "secret");
  ASSERT_TRUE(nvsGetStr("mqip") == "192.168.1.10");
  ASSERT_TRUE(nvsGetStr("mqport") == "1883");
  ASSERT_TRUE(nvsGetStr("mquser") == "agro");
  ASSERT_TRUE(nvsGetStr("mqpass") == "pw");
}

TEST(executor_wifi_config_needs_no_radio_fields) {
  resetFakes();
  std::string r = cfg("{\"id\":\"E007\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"25\"}],\"net\":\"1\","
                      "\"wifi_ssid\":\"FarmNet\",\"wifi_password\":\"\",\"mqtt_ip\":\"broker.local\","
                      "\"mqtt_port\":\"\",\"mqtt_user\":\"\",\"mqtt_password\":\"\"}");
  ASSERT_TRUE(isAck(r));
  ASSERT_TRUE(!nvsHas("freq"));
  ASSERT_TRUE(!nvsHas("key"));
  ASSERT_TRUE(nvsGetStr("mqport") == "1883");     // празен порт -> 1883
  ASSERT_TRUE(nvsGetStr("wpass") == "");
  ASSERT_EQ((long)nvsGetNum("nc"), 1L);
}

TEST(missing_or_bad_id_is_nacked_and_writes_nothing) {
  resetFakes();
  ASSERT_TRUE(isNack(cfg("{\"sf\":\"7\"}")));
  ASSERT_TRUE(isNack(cfg("{\"id\":\"\"}")));
  ASSERT_TRUE(isNack(cfg("{\"id\":\"ABCDEFG\"}")));         // 7 символа > 6
  ASSERT_EQ(g_nvsWrites, 0);
}

TEST(invalid_radio_params_are_nacked) {
  resetFakes();
  ASSERT_TRUE(isNack(cfg("{\"id\":\"S001\",\"sf\":\"13\"}")));
  ASSERT_TRUE(isNack(cfg("{\"id\":\"S001\",\"sf\":\"6\"}")));
  ASSERT_TRUE(isNack(cfg("{\"id\":\"S001\",\"bw\":\"100000\"}")));
  ASSERT_TRUE(isNack(cfg("{\"id\":\"S001\",\"bw\":\"500000\"}")));
  ASSERT_EQ(g_nvsWrites, 0);
  // всички валидни стойности минават
  ASSERT_TRUE(isAck(cfg("{\"id\":\"S001\",\"sf\":\"7\",\"bw\":\"62500\"}")));
  ASSERT_TRUE(isAck(cfg("{\"id\":\"S001\",\"sf\":\"12\",\"bw\":\"250000\"}")));
}

TEST(invalid_consumers_are_nacked) {
  resetFakes();
  ASSERT_TRUE(isNack(cfg("{\"id\":\"E001\",\"consumers\":[{\"id\":\"ABCDE\",\"pin\":\"25\"}]}")));   // id > 4
  ASSERT_TRUE(isNack(cfg("{\"id\":\"E001\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"40\"}]}")));      // GPIO > 39
  ASSERT_TRUE(isNack(cfg("{\"id\":\"E001\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"123\"}]}")));     // 3 цифри
  ASSERT_TRUE(isNack(cfg("{\"id\":\"E001\",\"consumers\":[{\"id\":\"V01\"}]}")));                    // липсва pin
  ASSERT_TRUE(isNack(cfg("{\"id\":\"E001\",\"consumers\":[{\"pin\":\"25\"}]}")));                    // липсва id
  ASSERT_TRUE(isNack(cfg("{\"id\":\"E001\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"25\"}")));       // липсва ]
  ASSERT_EQ(g_nvsWrites, 0);
  ASSERT_TRUE(isAck(cfg("{\"id\":\"E001\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"0\"}]}")));       // GPIO 0 е валиден
  ASSERT_TRUE(isAck(cfg("{\"id\":\"E001\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"39\"}]}")));
}

TEST(net_mode_requires_ssid_and_mqtt_host) {
  resetFakes();
  ASSERT_TRUE(isNack(cfg("{\"id\":\"G001\",\"net\":\"1\",\"mqtt_ip\":\"1.2.3.4\"}")));                 // няма ssid
  ASSERT_TRUE(isNack(cfg("{\"id\":\"G001\",\"net\":\"1\",\"wifi_ssid\":\"X\"}")));                      // няма mqtt_ip
  ASSERT_TRUE(isNack(cfg("{\"id\":\"G001\",\"net\":\"1\",\"wifi_ssid\":\"\",\"mqtt_ip\":\"1.2.3.4\"}")));
  ASSERT_EQ(g_nvsWrites, 0);
  // без net=1 Wi-Fi полетата не са задължителни (sensor/repeater/executor)
  ASSERT_TRUE(isAck(cfg("{\"id\":\"S001\"}")));
}

TEST(too_long_net_fields_are_nacked) {
  resetFakes();
  std::string longSsid(33, 'a');
  ASSERT_TRUE(isNack(cfg("{\"id\":\"G001\",\"net\":\"1\",\"wifi_ssid\":\"" + longSsid + "\",\"mqtt_ip\":\"1.2.3.4\"}")));
  std::string longPass(64, 'p');
  ASSERT_TRUE(isNack(cfg("{\"id\":\"G001\",\"net\":\"1\",\"wifi_ssid\":\"X\",\"wifi_password\":\"" + longPass + "\",\"mqtt_ip\":\"1.2.3.4\"}")));
  std::string longUser(33, 'u');
  ASSERT_TRUE(isNack(cfg("{\"id\":\"G001\",\"net\":\"1\",\"wifi_ssid\":\"X\",\"mqtt_ip\":\"1.2.3.4\",\"mqtt_user\":\"" + longUser + "\"}")));
  ASSERT_EQ(g_nvsWrites, 0);
}

TEST(optional_fields_are_not_overwritten_when_absent) {
  resetFakes();
  nvsSetU32("freq", 111);
  nvsSetU8("sf", 9);
  nvsSetBytes("key", TEST_KEY, 16);
  g_nvsWrites = 0;
  ASSERT_TRUE(isAck(cfg("{\"id\":\"S009\"}")));
  ASSERT_EQ((long)nvsGetNum("freq"), 111L);
  ASSERT_EQ((long)nvsGetNum("sf"), 9L);
  ASSERT_MEM_EQ(nvsGetBytes("key").data(), TEST_KEY, 16);
  ASSERT_TRUE(nvsGetStr("id") == "S009");
}

TEST(nonce_ceiling_is_never_touched_by_config) {
  // "ceil" се управлява само от реалния firmware - преконфигуриране не бива да връща брояча назад
  resetFakes();
  nvsSetU32("ceil", 5000);
  ASSERT_TRUE(isAck(cfg(std::string("{\"id\":\"S001\",\"key\":\"") + KEY_HEX + "\",\"freq\":\"433000000\",\"sf\":\"7\",\"bw\":\"125000\"}")));
  ASSERT_EQ((long)nvsGetNum("ceil"), 5000L);
}

TEST(invalid_key_is_nacked_and_nothing_is_written) {
  resetFakes();
  ASSERT_TRUE(isNack(cfg("{\"id\":\"S001\",\"key\":\"00112233\"}")));                              // къс
  ASSERT_TRUE(isNack(cfg("{\"id\":\"S001\",\"key\":\"ZZ112233445566778899AABBCCDDEEFF\"}")));      // не е hex
  ASSERT_TRUE(isNack(cfg("{\"id\":\"S001\",\"key\":\"00112233445566778899AABBCCDDEEFF00\"}")));    // дълъг
  ASSERT_EQ(g_nvsWrites, 0);
  ASSERT_TRUE(!nvsHas("key"));
}

TEST(lowercase_hex_key_is_accepted) {
  resetFakes();
  std::string r = cfg("{\"id\":\"S001\",\"key\":\"00112233445566778899aabbccddeeff\"}");
  ASSERT_TRUE(isAck(r));
  ASSERT_MEM_EQ(nvsGetBytes("key").data(), TEST_KEY, 16);
}

TEST(serial_input_with_noise_before_cfg_line_is_handled) {
  resetFakes();
  g_serialIn = "garbage line\r\nfoo\nCFG:{\"id\":\"S777\"}\n";
  waitForConfigPacket();
  ASSERT_TRUE(contains(g_serialOut, "ACK"));
  ASSERT_TRUE(nvsGetStr("id") == "S777");
}

TEST(config_survives_the_round_trip_into_the_real_loaders) {
  // CFG -> NVS -> (проверка, че ключовете съвпадат с тези, които четат реалните firmware-и)
  resetFakes();
  cfg(std::string("{\"id\":\"E001\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"25\"}],\"freq\":\"433000000\",\"key\":\"") + KEY_HEX + "\",\"sf\":\"7\",\"bw\":\"125000\"}");
  const char *needed[] = {"id", "nc", "c0i", "c0p", "freq", "sf", "bw", "key"};
  for (const char *k : needed) ASSERT_TRUE(nvsHas(k));
}

int main() { return runAllTests(); }
