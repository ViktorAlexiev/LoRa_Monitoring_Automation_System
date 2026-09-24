// Заместител на "Crypto" библиотеката, само CTR<Cipher> темплейт класа. Стандартен
// NIST SP800-38A CTR режим: keystream_i = E(key, counter_i), counter се увеличава
// като 128-битово big-endian число между блоковете. encrypt/decrypt са идентични
// операции (XOR с keystream), точно както при реален CTR stream cipher.
#ifndef FAKE_CTR_H
#define FAKE_CTR_H

#include <cstdint>
#include <cstring>
#include <cstddef>

template <typename Cipher>
class CTR {
public:
  void setKey(const uint8_t *key, size_t len) {
    cipher_.setKey(key, len);
  }
  void setIV(const uint8_t *iv, size_t len) {
    size_t n = len < 16 ? len : 16;
    memset(counter_, 0, 16);
    memcpy(counter_, iv, n);
  }
  void encrypt(uint8_t *output, const uint8_t *input, size_t len) {
    xorStream(output, input, len);
  }
  void decrypt(uint8_t *output, const uint8_t *input, size_t len) {
    xorStream(output, input, len); // CTR е symmetric - decrypt = encrypt
  }

private:
  Cipher cipher_;
  uint8_t counter_[16];

  void incrementCounter() {
    for (int i = 15; i >= 0; i--) {
      if (++counter_[i] != 0) break;
    }
  }

  void xorStream(uint8_t *out, const uint8_t *in, size_t len) {
    uint8_t block[16];
    uint8_t keystream[16];
    // работим върху копие на брояча, за да не мутираме състоянието между извиквания
    // по начин, различен от реалната Crypto::CTR семантика (тя пази позиция между
    // отделни encrypt() извиквания в общ stream, но crypto_common.cpp винаги прави
    // точно едно encrypt/decrypt извикване на wire пакет след setIV(), значи това е
    // еквивалентно поведение за нашите тестове).
    memcpy(block, counter_, 16);
    size_t offset = 0;
    while (offset < len) {
      cipher_.encryptBlock(keystream, block);
      size_t chunk = (len - offset) < 16 ? (len - offset) : 16;
      for (size_t i = 0; i < chunk; i++) out[offset + i] = in[offset + i] ^ keystream[i];
      offset += chunk;
      for (int i = 15; i >= 0; i--) { if (++block[i] != 0) break; }
    }
  }
};

#endif
