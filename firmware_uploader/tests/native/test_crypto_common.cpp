// Тества РЕАЛНИЯ firmware_src/sensor/crypto_common.cpp (идентичен и в четирите
// папки) срещу верифицирана AES-128 (виж aes_selfcheck.cpp) зад заместителите на
// "Crypto"/"AES_CMAC" библиотеките. Тества логиката на протокола (wire формат,
// nonce разделяне по type-id, отхвърляне на подправени пакети) - не преоткрива
// сигурността на самия AES-CMAC алгоритъм (той е established, доверен трети код).
#include "test_framework.h"
#include "crypto_common.h"
#include <cstring>

static void fillIncrementing(uint8_t *buf, uint8_t len, uint8_t start = 0) {
  for (uint8_t i = 0; i < len; i++) buf[i] = (uint8_t)(start + i);
}

TEST(round_trip_all_types_and_lengths) {
  uint8_t key[16]; fillIncrementing(key, 16, 0x10);
  uint8_t sender[6] = {'C','S','0','0','1', 0};

  uint8_t types[] = {
    CRYPTO_TYPE_SENSOR, CRYPTO_TYPE_REPEATER_HB, CRYPTO_TYPE_EXEC_HB,
    CRYPTO_TYPE_EXEC_ACK, CRYPTO_TYPE_GW_CMD, CRYPTO_TYPE_STATE_REQ,
    CRYPTO_TYPE_STATE_RESP, CRYPTO_TYPE_STATE_RESP_RESTART, CRYPTO_TYPE_STATE_RESP_ACK
  };
  uint8_t lens[] = {0, 1, 6, 16, 20, 49, 50}; // 0 = heartbeat-стил; 50 = максимумът (CRYPTO_MAX_PT)

  for (uint8_t t : types) {
    for (uint8_t L : lens) {
      uint8_t plaintext[64]; fillIncrementing(plaintext, L, t);
      uint8_t wire[64 + CRYPTO_OVERHEAD];
      uint8_t wireLen = cryptoBuildWirePacket(wire, key, t, sender, 12345, plaintext, L);
      ASSERT_EQ(wireLen, (uint8_t)(L + CRYPTO_OVERHEAD));

      uint8_t senderOut[6], ptOut[64], ptLenOut;
      bool ok = cryptoParseWirePacket(wire, wireLen, key, t, senderOut, ptOut, &ptLenOut);
      ASSERT_TRUE(ok);
      ASSERT_EQ(ptLenOut, L);
      ASSERT_MEM_EQ(senderOut, sender, 6);
      if (L > 0) ASSERT_MEM_EQ(ptOut, plaintext, L);
    }
  }
}

TEST(wrong_key_rejected) {
  uint8_t key[16]; fillIncrementing(key, 16, 0);
  uint8_t wrongKey[16]; fillIncrementing(wrongKey, 16, 1); // само 1 байт разлика
  uint8_t sender[6] = {'S','N','0','0','1',0};
  uint8_t pt[4] = {1,2,3,4};
  uint8_t wire[4 + CRYPTO_OVERHEAD];
  uint8_t wireLen = cryptoBuildWirePacket(wire, key, CRYPTO_TYPE_SENSOR, sender, 1, pt, 4);

  uint8_t senderOut[6], ptOut[4], ptLenOut;
  bool ok = cryptoParseWirePacket(wire, wireLen, wrongKey, CRYPTO_TYPE_SENSOR, senderOut, ptOut, &ptLenOut);
  ASSERT_FALSE(ok);
}

TEST(wrong_type_id_rejected_even_with_correct_key) {
  // Точно това гарантира разделянето на nonce пространството между потоците -
  // ключово свойство на протокола (виж crypto_common.h коментара за type-id).
  uint8_t key[16]; fillIncrementing(key, 16, 0x20);
  uint8_t sender[6] = {'E','X','0','0','1',0};
  uint8_t pt[6] = {0xA1,0xB2,0xC3,0xD4,0xE5,0xF6};
  uint8_t wire[6 + CRYPTO_OVERHEAD];
  uint8_t wireLen = cryptoBuildWirePacket(wire, key, CRYPTO_TYPE_EXEC_ACK, sender, 7, pt, 6);

  uint8_t senderOut[6], ptOut[6], ptLenOut;
  // опит за декриптиране като GW_CMD (различен type-id, същия wire дължина формат)
  bool ok = cryptoParseWirePacket(wire, wireLen, key, CRYPTO_TYPE_GW_CMD, senderOut, ptOut, &ptLenOut);
  ASSERT_FALSE(ok);

  // но с правилния type-id минава
  ok = cryptoParseWirePacket(wire, wireLen, key, CRYPTO_TYPE_EXEC_ACK, senderOut, ptOut, &ptLenOut);
  ASSERT_TRUE(ok);
}

TEST(tampered_ciphertext_byte_rejected) {
  uint8_t key[16]; fillIncrementing(key, 16, 0x30);
  uint8_t sender[6] = {'G','W','0','0','1',0};
  uint8_t pt[10]; fillIncrementing(pt, 10, 5);
  uint8_t wire[10 + CRYPTO_OVERHEAD];
  uint8_t wireLen = cryptoBuildWirePacket(wire, key, CRYPTO_TYPE_STATE_RESP, sender, 99, pt, 10);

  wire[CRYPTO_ID_LEN + 3] ^= 0x01; // флип 1 бит в шифротекста

  uint8_t senderOut[6], ptOut[10], ptLenOut;
  bool ok = cryptoParseWirePacket(wire, wireLen, key, CRYPTO_TYPE_STATE_RESP, senderOut, ptOut, &ptLenOut);
  ASSERT_FALSE(ok);
}

TEST(tampered_tag_byte_rejected) {
  uint8_t key[16]; fillIncrementing(key, 16, 0x40);
  uint8_t sender[6] = {'R','P','0','0','1',0};
  uint8_t pt[3] = {7,8,9};
  uint8_t wire[3 + CRYPTO_OVERHEAD];
  uint8_t wireLen = cryptoBuildWirePacket(wire, key, CRYPTO_TYPE_REPEATER_HB, sender, 1, pt, 3);

  wire[wireLen - 1] ^= 0x01; // флип последния байт (част от tag-а)

  uint8_t senderOut[6], ptOut[3], ptLenOut;
  bool ok = cryptoParseWirePacket(wire, wireLen, key, CRYPTO_TYPE_REPEATER_HB, senderOut, ptOut, &ptLenOut);
  ASSERT_FALSE(ok);
}

TEST(tampered_counter_rejected) {
  uint8_t key[16]; fillIncrementing(key, 16, 0x50);
  uint8_t sender[6] = {'C','S','0','0','2',0};
  uint8_t pt[2] = {0xAA, 0xBB};
  uint8_t wire[2 + CRYPTO_OVERHEAD];
  uint8_t wireLen = cryptoBuildWirePacket(wire, key, CRYPTO_TYPE_SENSOR, sender, 500, pt, 2);

  // counter полето е точно след ciphertext-a (CRYPTO_ID_LEN + ptLen), 4 байта big-endian
  wire[CRYPTO_ID_LEN + 2] ^= 0xFF;

  uint8_t senderOut[6], ptOut[2], ptLenOut;
  bool ok = cryptoParseWirePacket(wire, wireLen, key, CRYPTO_TYPE_SENSOR, senderOut, ptOut, &ptLenOut);
  ASSERT_FALSE(ok);
}

TEST(tampered_sender_id_rejected) {
  // senderId участва в nonce-а (nonce[1..6]) - подмяна на подателя, без да се пипа
  // друго, трябва да чупи проверката, защото верификаторът пресъздава nonce-а от
  // ПОЛУЧЕНИЯ (вече подменен) senderId, не от оригиналния.
  uint8_t key[16]; fillIncrementing(key, 16, 0x60);
  uint8_t sender[6] = {'A','A','A','A','A','A'};
  uint8_t pt[4] = {1,1,1,1};
  uint8_t wire[4 + CRYPTO_OVERHEAD];
  uint8_t wireLen = cryptoBuildWirePacket(wire, key, CRYPTO_TYPE_EXEC_HB, sender, 1, pt, 4);

  wire[0] = 'B'; // подмяна на първия байт от senderId в wire буфера

  uint8_t senderOut[6], ptOut[4], ptLenOut;
  bool ok = cryptoParseWirePacket(wire, wireLen, key, CRYPTO_TYPE_EXEC_HB, senderOut, ptOut, &ptLenOut);
  ASSERT_FALSE(ok);
  // senderIdOut трябва да е попълнен ДОРИ при невалиден MAC (по дизайн - виж коментара в .h)
  ASSERT_EQ(senderOut[0], (uint8_t)'B');
}

TEST(determinism_same_params_same_ciphertext) {
  uint8_t key[16]; fillIncrementing(key, 16, 0x70);
  uint8_t sender[6] = {'D','E','T','0','0','1'};
  uint8_t pt[5] = {9,8,7,6,5};
  uint8_t wireA[5 + CRYPTO_OVERHEAD], wireB[5 + CRYPTO_OVERHEAD];
  cryptoBuildWirePacket(wireA, key, CRYPTO_TYPE_SENSOR, sender, 42, pt, 5);
  cryptoBuildWirePacket(wireB, key, CRYPTO_TYPE_SENSOR, sender, 42, pt, 5);
  ASSERT_MEM_EQ(wireA, wireB, 5 + CRYPTO_OVERHEAD); // чисто детерминистичен build при еднакви входове
}

TEST(different_type_id_gives_different_ciphertext_same_counter) {
  // ядрото на защитата: споделен counter между потоци не води до nonce reuse,
  // защото type-id-то влиза в nonce-а и променя keystream-а.
  uint8_t key[16]; fillIncrementing(key, 16, 0x80);
  uint8_t sender[6] = {'S','H','R','0','0','1'};
  uint8_t pt[6] = {1,2,3,4,5,6};
  uint8_t wireCmd[6 + CRYPTO_OVERHEAD], wireReq[6 + CRYPTO_OVERHEAD];
  cryptoBuildWirePacket(wireCmd, key, CRYPTO_TYPE_GW_CMD, sender, 77, pt, 6);
  cryptoBuildWirePacket(wireReq, key, CRYPTO_TYPE_STATE_REQ, sender, 77, pt, 6);
  // senderId частта (първите 6 байта) е еднаква (чист текст), но ciphertext+tag частта трябва да се различава
  ASSERT_MEM_NE(wireCmd + CRYPTO_ID_LEN, wireReq + CRYPTO_ID_LEN, 6 + CRYPTO_CNT_LEN + CRYPTO_TAG_LEN);
}

TEST(wire_len_too_short_rejected) {
  uint8_t key[16]; fillIncrementing(key, 16, 0x90);
  uint8_t senderOut[6], ptOut[8], ptLenOut;
  uint8_t shortWire[CRYPTO_OVERHEAD - 1] = {0};
  bool ok = cryptoParseWirePacket(shortWire, sizeof(shortWire), key, CRYPTO_TYPE_SENSOR, senderOut, ptOut, &ptLenOut);
  ASSERT_FALSE(ok);
}

TEST(exact_overhead_length_zero_plaintext_ok) {
  // wireLen == CRYPTO_OVERHEAD точно (граничен случай, ptLen=0) - валиден heartbeat формат
  uint8_t key[16]; fillIncrementing(key, 16, 0xA0);
  uint8_t sender[6] = {'H','B','0','0','1',0};
  uint8_t wire[CRYPTO_OVERHEAD];
  uint8_t wireLen = cryptoBuildWirePacket(wire, key, CRYPTO_TYPE_EXEC_HB, sender, 1, nullptr, 0);
  ASSERT_EQ(wireLen, (uint8_t)CRYPTO_OVERHEAD);

  uint8_t senderOut[6], ptOut[1], ptLenOut;
  bool ok = cryptoParseWirePacket(wire, wireLen, key, CRYPTO_TYPE_EXEC_HB, senderOut, ptOut, &ptLenOut);
  ASSERT_TRUE(ok);
  ASSERT_EQ(ptLenOut, (uint8_t)0);
}

TEST(crypto_max_pt_matches_state_response_10_consumers) {
  // документираща инварианта: CRYPTO_MAX_PT трябва да е >= MAX_CONSUMERS(10) x 5 байта
  ASSERT_TRUE(CRYPTO_MAX_PT >= 50);
}

int main() {
  return runAllTests();
}
