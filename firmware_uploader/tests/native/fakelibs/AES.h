// Заместител на "Crypto" (Rhys Weatherley) библиотеката, само класа AESTiny128, с API
// сигнатурата, която crypto_common.cpp реално ползва (setKey, encryptBlock). Реализацията
// зад него е верифицираната AES128 (../aes128.h), тествана срещу FIPS-197 вектор.
#ifndef FAKE_AES_H
#define FAKE_AES_H

#include "../aes128.h"

class AESTiny128 {
public:
  void setKey(const uint8_t *key, size_t len) {
    (void)len; // винаги 16 за AES-128, crypto_common.cpp го гарантира
    core_.setKey(key);
  }
  void encryptBlock(uint8_t *output, const uint8_t *input) {
    core_.encryptBlock(output, input);
  }
private:
  aes128::AES128 core_;
};

#endif
