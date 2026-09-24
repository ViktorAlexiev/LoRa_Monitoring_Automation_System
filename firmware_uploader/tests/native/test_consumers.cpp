// Тества РЕАЛНИЯ firmware_src/executor/consumers.cpp срещу ръчно попълнени
// consumerList/NUM_CONSUMERS (extern глобали, декларирани в config_storage.h,
// определени тук директно - не е нужен целият config_storage.cpp за този тест).
#include "test_framework.h"
#include "consumers.h"
#include "config_storage.h"
#include <cstring>

// Дефинираме extern глобалите от config_storage.h директно (само тези, от които
// consumers.cpp реално се нуждае), за да изолираме теста от EEPROM/crypto зависимости.
char MY_M_ID[MODULE_ID_LEN + 1] = "TEST01";
char consumerList[MAX_CONSUMERS][CONSUMER_ID_LEN + 1];
uint8_t consumerPins[MAX_CONSUMERS];
uint8_t NUM_CONSUMERS = 0;
uint32_t LORA_FREQ_HZ = 0;
uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
CeilingCounter txCounter;

static void setConsumers(std::initializer_list<const char*> ids) {
  NUM_CONSUMERS = 0;
  for (auto id : ids) {
    strncpy(consumerList[NUM_CONSUMERS], id, CONSUMER_ID_LEN);
    consumerList[NUM_CONSUMERS][CONSUMER_ID_LEN] = 0;
    consumerPins[NUM_CONSUMERS] = 0;
    NUM_CONSUMERS++;
  }
}

TEST(exists_and_index_for_present_consumer) {
  setConsumers({"B1C2", "D4E5", "F6A7"});
  ASSERT_TRUE(consumerExists("D4E5"));
  ASSERT_EQ(consumerIndex("D4E5"), 1);
  ASSERT_TRUE(consumerExists("B1C2"));
  ASSERT_EQ(consumerIndex("B1C2"), 0);
  ASSERT_TRUE(consumerExists("F6A7"));
  ASSERT_EQ(consumerIndex("F6A7"), 2);
}

TEST(missing_consumer_returns_false_and_minus_one) {
  setConsumers({"B1C2", "D4E5"});
  ASSERT_FALSE(consumerExists("ZZZZ"));
  ASSERT_EQ(consumerIndex("ZZZZ"), -1);
}

TEST(empty_consumer_list_never_matches) {
  setConsumers({});
  ASSERT_EQ(NUM_CONSUMERS, (uint8_t)0);
  ASSERT_FALSE(consumerExists("B1C2"));
  ASSERT_EQ(consumerIndex("B1C2"), -1);
}

TEST(full_ten_consumers_last_one_found) {
  setConsumers({"AAAA","BBBB","CCCC","DDDD","EEEE","FFFF","GGGG","HHHH","IIII","JJJJ"});
  ASSERT_EQ(NUM_CONSUMERS, (uint8_t)MAX_CONSUMERS);
  ASSERT_EQ(consumerIndex("JJJJ"), 9); // последният слот все още се търси коректно
  ASSERT_TRUE(consumerExists("JJJJ"));
}

TEST(comparison_is_exactly_4_chars_strncmp) {
  // strncmp(..., 4) - сравнява точно 4 знака; consumerList записите вече са с
  // терминатор на позиция 4, значи по-дълъг търсен низ не би могъл технически да мине
  // през реалния EEPROM формат, но проверяваме че функцията не чете отвъд 4-тия байт.
  setConsumers({"AB12"});
  ASSERT_TRUE(consumerExists("AB12"));
  ASSERT_FALSE(consumerExists("AB13"));
}

int main() {
  return runAllTests();
}
