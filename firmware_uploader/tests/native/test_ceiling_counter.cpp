// Тества РЕАЛНИЯ firmware_src/sensor/ceiling_counter.cpp (идентичен в sensor/executor/
// repeater) срещу заместена EEPROM (fakelibs/EEPROM.h) - позволява да симулираме
// "unclean reset" (нов CeilingCounter обект без предишния in-RAM state) и да проверим
// инварианта: никога не се връща назад, никога не преизползва вече committed стойност.
#include "test_framework.h"
#include "ceiling_counter.h"
#include "EEPROM.h"
#include <cstdint>

static const int ADDR = 91; // истинският EEPROM адрес, ползван във всички устройства

TEST(fresh_eeprom_starts_at_zero) {
  eeprom_reset_erased(); // 0xFF навсякъде - фабрично неинициализиран chip
  CeilingCounter c;
  c.begin(ADDR);
  ASSERT_EQ(c.next(), 0UL);
}

TEST(first_call_commits_ceiling_100_before_returning_0) {
  eeprom_reset_erased();
  CeilingCounter c;
  c.begin(ADDR);
  ASSERT_EQ(c.next(), 0UL);
  uint32_t committed;
  EEPROM.get(ADDR, committed);
  ASSERT_EQ(committed, 100UL); // записано ПРЕДИ да се ползва стойността, по дизайн
}

TEST(no_write_between_commit_boundaries) {
  eeprom_reset_erased();
  CeilingCounter c;
  c.begin(ADDR);
  c.next(); // 0 -> commit 100
  uint32_t afterFirst;
  EEPROM.get(ADDR, afterFirst);
  ASSERT_EQ(afterFirst, 100UL);

  for (int i = 0; i < 98; i++) c.next(); // изразходва 1..98, все още < 100 committed
  uint32_t stillSame;
  EEPROM.get(ADDR, stillSame);
  ASSERT_EQ(stillSame, 100UL); // не е писано пак

  uint32_t v = c.next(); // 99-та стойност (индекс 99, точно под тавана 100)
  ASSERT_EQ(v, 99UL);
  uint32_t stillSame2;
  EEPROM.get(ADDR, stillSame2);
  ASSERT_EQ(stillSame2, 100UL); // все още не е писано - 99 < 100 committed

  uint32_t next = c.next(); // сега current(100) >= committed(100) -> нов commit на 200
  ASSERT_EQ(next, 100UL);
  uint32_t afterSecond;
  EEPROM.get(ADDR, afterSecond);
  ASSERT_EQ(afterSecond, 200UL);
}

TEST(unclean_reset_resumes_from_committed_ceiling_not_last_used) {
  eeprom_reset_erased();
  {
    CeilingCounter c;
    c.begin(ADDR);
    for (int i = 0; i < 57; i++) c.next(); // ползвани 0..56, committed=100 (записано на 1-вия next())
  }
  // симулираме "unclean reset" - нов обект, все едно чипът се е рестартирал внезапно
  // (in-RAM състоянието на стария CeilingCounter е изгубено, EEPROM остава)
  CeilingCounter c2;
  c2.begin(ADDR);
  uint32_t v = c2.next();
  ASSERT_EQ(v, 100UL); // продължава от committed тавана (100), НЕ от 57 - никога назад
}

TEST(never_goes_backward_across_many_unclean_resets) {
  eeprom_reset_erased();
  uint32_t lastSeen = 0;
  for (int reset = 0; reset < 20; reset++) {
    CeilingCounter c;
    c.begin(ADDR);
    // всеки "живот" ползва случаен (тук: фиксиран) брой стойности между 1 и 50,
    // после "крашва" - обектът излиза от scope без чист shutdown
    for (int i = 0; i < 13; i++) {
      uint32_t v = c.next();
      ASSERT_TRUE(v >= lastSeen); // монотонно ненамаляващо през целия живот на теста
      lastSeen = v;
    }
  }
}

TEST(ffffffff_treated_as_uninitialized_same_as_zero) {
  eeprom_reset_erased(); // 0xFFFFFFFF на адрес 91 след memset(0xFF)
  CeilingCounter c;
  c.begin(ADDR);
  ASSERT_EQ(c.next(), 0UL); // не 0xFFFFFFFF - изрично третирано като "неинициализиран"
}

TEST(commit_interval_is_exactly_100) {
  ASSERT_EQ((unsigned long)CEILING_COMMIT_INTERVAL, 100UL);
}

int main() {
  return runAllTests();
}
