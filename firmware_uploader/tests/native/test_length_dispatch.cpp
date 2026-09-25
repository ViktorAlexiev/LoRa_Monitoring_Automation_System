// Не компилира gateway/lora_handlers.cpp или executor/radio_io.cpp директно (изисква
// тежко мокване на LoRa.h/WiFi.h/PubSubClient.h/ArduinoJson.h с ограничена
// допълнителна полза спрямо вече покрития crypto_common). Вместо това пресъздава
// точната dispatch логика по дължина от тези файлове, с РЕАЛНИТЕ константи от
// crypto_common.h, и доказва изчерпателно, че няма числена колизия между типовете
// пакети в системата - точно темата, която подробно обсъждахме в разговора.
#include "test_framework.h"
#include "crypto_common.h"
#include <set>
#include <map>
#include <string>

// Реални wire дължини, копирани от gateway/lora_handlers.cpp и executor/radio_io.cpp
// (стойностите, не логиката - логиката се пресъздава тук изрично и се сравнява).
#define MODULE_ID_LEN 6
#define SENSOR_PT_LEN 16
#define SENSOR_WIRE_LEN (CRYPTO_OVERHEAD + SENSOR_PT_LEN)                      // 30
#define HB_WIRE_LEN (CRYPTO_OVERHEAD)                                          // 14
#define ACK_PT_LEN 6
#define ACK_WIRE_LEN (CRYPTO_OVERHEAD + ACK_PT_LEN)                            // 20
#define CMD_PLAINTEXT_LEN 6
#define CMD_WIRE_LEN (MODULE_ID_LEN + CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN)     // 26
#define STATE_REQ_WIRE_LEN (MODULE_ID_LEN + CRYPTO_OVERHEAD)                   // 20
#define STATE_RESP_ACK_WIRE_LEN (MODULE_ID_LEN + CRYPTO_OVERHEAD)              // 20
#define STATE_ENTRY_LEN 5
#define STATE_MAX_CONSUMERS 10

// ---------------- Gateway: коя грана на lora_handle_incoming() хваща дадена дължина ----------------
// Възпроизвежда точния ред на проверките от gateway/lora_handlers.cpp.
static std::string gatewayBranchFor(int len) {
  if (len == SENSOR_WIRE_LEN) return "SENSOR";   // вече без +1: Repeater не добавя маркер
  if (len == ACK_WIRE_LEN) return "ACK";
  if (len == HB_WIRE_LEN) return "HB_OR_STATE0"; // пробва REPEATER_HB,EXEC_HB,STATE_RESP,STATE_RESP_RESTART
  if (len > CRYPTO_OVERHEAD && (len - CRYPTO_OVERHEAD) % STATE_ENTRY_LEN == 0 &&
      (len - CRYPTO_OVERHEAD) <= STATE_MAX_CONSUMERS * STATE_ENTRY_LEN) return "STATE_RESP_N";
  return "UNKNOWN";
}

// ---------------- Executor: коя грана на radioReceivePoll() хваща дадена дължина ----------------
static std::string executorBranchFor(int len) {
  if (len == CMD_WIRE_LEN) return "CMD";
  if (len == STATE_REQ_WIRE_LEN) return "STATE_REQ_OR_ACK"; // пробва STATE_REQ, после STATE_RESP_ACK
  return "UNKNOWN";
}

TEST(gateway_every_legit_packet_length_maps_to_exactly_one_branch) {
  // Всички дължини, които РЕАЛНО могат да пристигнат на Gateway според протокола:
  std::map<int, std::string> legit = {
    {SENSOR_WIRE_LEN,                         "SENSOR"},
    {ACK_WIRE_LEN,                            "ACK"},
    {HB_WIRE_LEN,                             "HB_OR_STATE0"},  // Repeater HB / Executor HB / state resp с 0 консуматора
  };
  for (int n = 1; n <= STATE_MAX_CONSUMERS; n++) {
    legit[CRYPTO_OVERHEAD + n * STATE_ENTRY_LEN] = "STATE_RESP_N";
  }

  for (auto &kv : legit) {
    std::string got = gatewayBranchFor(kv.first);
    if (got != kv.second) {
      printf("  [FAIL] len=%d очаквано=%s получено=%s\n", kv.first, kv.second.c_str(), got.c_str());
    }
    ASSERT_TRUE(got == kv.second);
  }
}

TEST(gateway_no_two_different_legit_lengths_collide_on_wrong_branch) {
  // Обхожда ВСИЧКИ дължини от 0 до 100 байта (далеч отвъд реалния максимум) и проверява,
  // че освен изброените "легитимни" дължини, нищо друго случайно не пада в познат клон.
  std::set<int> legitLens = {SENSOR_WIRE_LEN, ACK_WIRE_LEN, HB_WIRE_LEN};
  for (int n = 1; n <= STATE_MAX_CONSUMERS; n++) legitLens.insert(CRYPTO_OVERHEAD + n * STATE_ENTRY_LEN);

  int unexpectedMatches = 0;
  for (int len = 0; len <= 100; len++) {
    std::string branch = gatewayBranchFor(len);
    bool isLegit = legitLens.count(len) > 0;
    if (branch != "UNKNOWN" && !isLegit) {
      printf("  [FAIL] len=%d неочаквано пада в клон %s, но не е в списъка на легитимните дължини\n", len, branch.c_str());
      unexpectedMatches++;
    }
  }
  ASSERT_EQ(unexpectedMatches, 0);
}

TEST(executor_cmd_and_state_wire_lengths_do_not_collide) {
  ASSERT_TRUE(CMD_WIRE_LEN != STATE_REQ_WIRE_LEN);
  ASSERT_TRUE(executorBranchFor(CMD_WIRE_LEN) == "CMD");
  ASSERT_TRUE(executorBranchFor(STATE_REQ_WIRE_LEN) == "STATE_REQ_OR_ACK");
  ASSERT_TRUE(executorBranchFor(STATE_RESP_ACK_WIRE_LEN) == "STATE_REQ_OR_ACK"); // еднаква дължина, разпознава се по type-id при decrypt
}

TEST(documented_wire_length_constants_match_expected_values) {
  // "живи" регресионни проверки на конкретните числа, обсъждани в документацията -
  // ако някой промени CRYPTO_OVERHEAD или полетата, тестът явно се чупи.
  ASSERT_EQ((int)CRYPTO_OVERHEAD, 14);
  ASSERT_EQ((int)SENSOR_WIRE_LEN, 30);
  ASSERT_EQ((int)HB_WIRE_LEN, 14);
  ASSERT_EQ((int)ACK_WIRE_LEN, 20);
  ASSERT_EQ((int)CMD_WIRE_LEN, 26);
  ASSERT_EQ((int)STATE_REQ_WIRE_LEN, 20);
  ASSERT_EQ((int)(CRYPTO_OVERHEAD + STATE_MAX_CONSUMERS * STATE_ENTRY_LEN), 64); // worst-case пакет
}

int main() {
  return runAllTests();
}
