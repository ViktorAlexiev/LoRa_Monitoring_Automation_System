#include "crypto_common.h"

#include <string.h>
#include "mbedtls/aes.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"

static void buildNonce(uint8_t *nonce16, uint8_t typeId, const uint8_t *senderId6, uint32_t counter) {
  memset(nonce16, 0, 16);
  nonce16[0] = typeId;
  memcpy(nonce16 + 1, senderId6, CRYPTO_ID_LEN);
  nonce16[7]  = (uint8_t)(counter >> 24);
  nonce16[8]  = (uint8_t)(counter >> 16);
  nonce16[9]  = (uint8_t)(counter >> 8);
  nonce16[10] = (uint8_t)(counter);
  // nonce16[11..15] остават 0 (padding до 16 bytes)
}

// AES-128-CTR: началният брояч е nonce-ът (16 B), увеличава се като 128-битово BE число - същото
// като CTR<AESTiny128> от библиотеката Crypto. Криптирането и декриптирането са една операция.
static bool ctrCrypt(const uint8_t *key16, const uint8_t *nonce16, const uint8_t *in, uint8_t *out, size_t len) {
  if (len == 0) return true;
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  bool ok = mbedtls_aes_setkey_enc(&ctx, key16, 128) == 0;
  if (ok) {
    uint8_t counter[16];
    uint8_t streamBlock[16];
    size_t ncOff = 0;
    memcpy(counter, nonce16, 16);
    ok = mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, counter, streamBlock, in, out) == 0;
  }
  mbedtls_aes_free(&ctx);
  return ok;
}

// MAC вход = nonce (носи типа/подателя/counter-а - AAD) || ciphertext; тагът е първите 4 B на CMAC.
static bool computeTag(const uint8_t *key16, const uint8_t *nonce16,
                       const uint8_t *data, uint8_t dataLen, uint8_t *tagOut) {
  uint8_t macIn[16 + CRYPTO_MAX_PT];
  uint8_t fullMac[16];
  memcpy(macIn, nonce16, 16);
  if (dataLen) memcpy(macIn + 16, data, dataLen);
  const mbedtls_cipher_info_t *info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
  if (info == NULL) return false;
  if (mbedtls_cipher_cmac(info, key16, 128, macIn, 16 + (size_t)dataLen, fullMac) != 0) return false;
  memcpy(tagOut, fullMac, CRYPTO_TAG_LEN);
  return true;
}

// сравнение за константно време - да не се разкрива докъде съвпада тагът
static bool tagEqual(const uint8_t *a, const uint8_t *b) {
  uint8_t diff = 0;
  for (int32_t i = 0; i < CRYPTO_TAG_LEN; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

uint8_t cryptoBuildWirePacket(uint8_t *wireBufOut, const uint8_t *key16, uint8_t typeId,
                               const uint8_t *senderId6, uint32_t counter,
                               const uint8_t *plaintext, uint8_t ptLen) {
  if (ptLen > CRYPTO_MAX_PT) return 0;
  uint8_t nonce[16];
  buildNonce(nonce, typeId, senderId6, counter);

  uint8_t *ciphertext = wireBufOut + CRYPTO_ID_LEN;
  if (!ctrCrypt(key16, nonce, plaintext, ciphertext, ptLen)) return 0;

  uint8_t tag[CRYPTO_TAG_LEN];
  if (!computeTag(key16, nonce, ciphertext, ptLen, tag)) return 0;

  memcpy(wireBufOut, senderId6, CRYPTO_ID_LEN);
  uint8_t *cntPos = wireBufOut + CRYPTO_ID_LEN + ptLen;
  cntPos[0] = (uint8_t)(counter >> 24);
  cntPos[1] = (uint8_t)(counter >> 16);
  cntPos[2] = (uint8_t)(counter >> 8);
  cntPos[3] = (uint8_t)(counter);
  memcpy(cntPos + CRYPTO_CNT_LEN, tag, CRYPTO_TAG_LEN);

  return (uint8_t)(CRYPTO_ID_LEN + ptLen + CRYPTO_CNT_LEN + CRYPTO_TAG_LEN);
}

bool cryptoParseWirePacket(const uint8_t *wireBuf, uint8_t wireLen, const uint8_t *key16,
                            uint8_t typeId, uint8_t *senderIdOut6, uint8_t *plaintextOut,
                            uint8_t *ptLenOut) {
  if (wireLen < CRYPTO_OVERHEAD) return false;
  uint8_t ptLen = (uint8_t)(wireLen - CRYPTO_OVERHEAD);
  if (ptLen > CRYPTO_MAX_PT) return false;

  memcpy(senderIdOut6, wireBuf, CRYPTO_ID_LEN);
  const uint8_t *ciphertext = wireBuf + CRYPTO_ID_LEN;
  const uint8_t *cntPos = ciphertext + ptLen;
  uint32_t counter = ((uint32_t)cntPos[0] << 24) | ((uint32_t)cntPos[1] << 16) |
                     ((uint32_t)cntPos[2] << 8) | (uint32_t)cntPos[3];
  const uint8_t *tag = cntPos + CRYPTO_CNT_LEN;

  uint8_t nonce[16];
  buildNonce(nonce, typeId, senderIdOut6, counter);

  uint8_t expectedTag[CRYPTO_TAG_LEN];
  if (!computeTag(key16, nonce, ciphertext, ptLen, expectedTag)) return false;
  if (!tagEqual(expectedTag, tag)) return false;

  if (!ctrCrypt(key16, nonce, ciphertext, plaintextOut, ptLen)) return false;
  *ptLenOut = ptLen;
  return true;
}
