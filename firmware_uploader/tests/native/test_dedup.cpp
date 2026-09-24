// Тества РЕАЛНИЯ firmware_src/repeater/dedup.cpp - кръгов буфер за "виждал ли съм
// тези байтове" (DEDUP_BUFFER_SIZE=6 записа, DEDUP_MAX_LEN=32 байта).
#include "test_framework.h"
#include "dedup.h"
#include <cstring>

static void fillBuf(uint8_t *buf, uint8_t len, uint8_t seed) {
  for (uint8_t i = 0; i < len; i++) buf[i] = (uint8_t)(seed + i);
}

TEST(unseen_initially) {
  uint8_t data[10]; fillBuf(data, 10, 1);
  ASSERT_FALSE(dedupSeen(data, 10));
}

TEST(seen_after_add_exact_match) {
  uint8_t data[10]; fillBuf(data, 10, 2);
  dedupAdd(data, 10);
  ASSERT_TRUE(dedupSeen(data, 10));
}

TEST(different_length_not_seen_even_if_prefix_matches) {
  uint8_t data[10]; fillBuf(data, 10, 3);
  dedupAdd(data, 10);
  ASSERT_FALSE(dedupSeen(data, 9));  // по-къс префикс - различна дължина, НЕ дубликат
  ASSERT_FALSE(dedupSeen(data, 11)); // по-дълъг (данните отвъд 10 са боклук) - НЕ дубликат
}

TEST(one_bit_different_not_seen) {
  uint8_t data[8]; fillBuf(data, 8, 4);
  dedupAdd(data, 8);
  uint8_t tampered[8]; memcpy(tampered, data, 8);
  tampered[3] ^= 0x01;
  ASSERT_FALSE(dedupSeen(tampered, 8));
}

TEST(ring_buffer_wraparound_evicts_oldest) {
  // DEDUP_BUFFER_SIZE = 6 - добавяме 7 различни записа, първият трябва да е "забравен"
  uint8_t records[7][4];
  for (int i = 0; i < 7; i++) {
    fillBuf(records[i], 4, (uint8_t)(100 + i)); // гарантирано различни едно от друго
    dedupAdd(records[i], 4);
  }
  ASSERT_FALSE(dedupSeen(records[0], 4)); // изместен от кръговия буфер
  for (int i = 1; i < 7; i++) {
    ASSERT_TRUE(dedupSeen(records[i], 4)); // последните 6 все още се помнят
  }
}

TEST(max_len_32_respected) {
  uint8_t data[DEDUP_MAX_LEN]; fillBuf(data, DEDUP_MAX_LEN, 9);
  dedupAdd(data, DEDUP_MAX_LEN);
  ASSERT_TRUE(dedupSeen(data, DEDUP_MAX_LEN));
}

int main() {
  return runAllTests();
}
