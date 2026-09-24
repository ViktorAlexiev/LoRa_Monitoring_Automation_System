// Заместител на "AES_CMAC" библиотеката. Реализация по RFC 4493 (AES-CMAC), върху
// подадения AES block cipher (тук AESTiny128 от ../fakelibs/AES.h, зад него верифицираната
// AES128 core). Използва се само за 16-байтов ключ / произволна дължина съобщение,
// каквото е и реалната употреба в crypto_common.cpp.
#ifndef FAKE_AES_CMAC_H
#define FAKE_AES_CMAC_H

#include <cstdint>
#include <cstring>
#include <cstddef>

template <typename Cipher>
class AES_CMAC_T {
public:
  explicit AES_CMAC_T(Cipher &cipher) : cipher_(cipher) {}

  void generateMAC(uint8_t *mac, const uint8_t *key, const uint8_t *data, size_t len) {
    cipher_.setKey(key, 16);

    uint8_t L[16] = {0};
    uint8_t zero[16] = {0};
    cipher_.encryptBlock(L, zero);

    uint8_t K1[16], K2[16];
    leftShiftXorRb(K1, L);
    leftShiftXorRb(K2, K1);

    size_t n = (len == 0) ? 1 : (len + 15) / 16;
    bool lastComplete = (len != 0) && (len % 16 == 0);

    uint8_t lastBlock[16];
    if (lastComplete) {
      memcpy(lastBlock, data + (n - 1) * 16, 16);
      xorBlock(lastBlock, K1);
    } else {
      size_t rem = len - (n - 1) * 16; // байтове в последния непълен блок (0 при len==0)
      memset(lastBlock, 0, 16);
      if (rem > 0) memcpy(lastBlock, data + (n - 1) * 16, rem);
      lastBlock[rem] = 0x80; // padding по RFC 4493
      xorBlock(lastBlock, K2);
    }

    uint8_t X[16] = {0};
    for (size_t i = 0; i + 1 < n; i++) {
      uint8_t Y[16];
      for (int j = 0; j < 16; j++) Y[j] = X[j] ^ data[i * 16 + j];
      cipher_.encryptBlock(X, Y);
    }
    uint8_t Y[16];
    for (int j = 0; j < 16; j++) Y[j] = X[j] ^ lastBlock[j];
    cipher_.encryptBlock(mac, Y);
  }

private:
  Cipher &cipher_;

  static void xorBlock(uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) a[i] ^= b[i];
  }
  // left-shift 1 бит на 128-битов блок, после XOR с Rb=0x87, ако MSB на входа е бил 1
  // (стандартната subkey деривация по RFC 4493 / NIST SP800-38B).
  static void leftShiftXorRb(uint8_t *out, const uint8_t *in) {
    uint8_t msb = in[0] & 0x80;
    uint8_t carry = 0;
    for (int i = 15; i >= 0; i--) {
      uint8_t newCarry = (in[i] & 0x80) ? 1 : 0;
      out[i] = (uint8_t)((in[i] << 1) | carry);
      carry = newCarry;
    }
    if (msb) out[15] ^= 0x87;
  }
};

// crypto_common.cpp вика "AES_CMAC cmac(aes);" без темплейт параметър - самото
// AESTiny128 е дефинирано в AES.h, включен преди този файл, затова тук просто
// alias-ваме темплейта към конкретния тип.
class AESTiny128; // forward decl (реалната дефиниция е в AES.h)
using AES_CMAC = AES_CMAC_T<AESTiny128>;

#endif
