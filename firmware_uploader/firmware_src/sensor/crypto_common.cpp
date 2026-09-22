#include "crypto_common.h"
#include <AES.h>
#include <CTR.h>
#include <AES_CMAC.h>
#include <string.h>

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

// MAC вход = nonce (носи типа/подателя/counter-а - AAD) || ciphertext.
// Промяна във всяко от тях (nonce компонент или данни) чупи веригата и проличава при verify.
static void computeTag(const uint8_t *key16, const uint8_t *nonce16,
                        const uint8_t *data, uint8_t dataLen, uint8_t *tagOut) {
  AESTiny128 aes;
  AES_CMAC cmac(aes);
  uint8_t macIn[16 + CRYPTO_MAX_PT];
  uint8_t fullMac[16];

  memcpy(macIn, nonce16, 16);
  memcpy(macIn + 16, data, dataLen);
  cmac.generateMAC(fullMac, key16, macIn, 16 + dataLen);
  memcpy(tagOut, fullMac, CRYPTO_TAG_LEN);
}

uint8_t cryptoBuildWirePacket(uint8_t *wireBufOut, const uint8_t *key16, uint8_t typeId,
                               const uint8_t *senderId6, uint32_t counter,
                               const uint8_t *plaintext, uint8_t ptLen) {
  uint8_t nonce[16];
  buildNonce(nonce, typeId, senderId6, counter);

  uint8_t *ciphertext = wireBufOut + CRYPTO_ID_LEN;
  CTR<AESTiny128> ctr;
  ctr.setKey(key16, CRYPTO_KEY_LEN);
  ctr.setIV(nonce, 16);
  ctr.encrypt(ciphertext, plaintext, ptLen);

  uint8_t tag[CRYPTO_TAG_LEN];
  computeTag(key16, nonce, ciphertext, ptLen, tag);

  memcpy(wireBufOut, senderId6, CRYPTO_ID_LEN);
  uint8_t *cntPos = wireBufOut + CRYPTO_ID_LEN + ptLen;
  cntPos[0] = (uint8_t)(counter >> 24);
  cntPos[1] = (uint8_t)(counter >> 16);
  cntPos[2] = (uint8_t)(counter >> 8);
  cntPos[3] = (uint8_t)(counter);
  memcpy(cntPos + CRYPTO_CNT_LEN, tag, CRYPTO_TAG_LEN);

  return CRYPTO_ID_LEN + ptLen + CRYPTO_CNT_LEN + CRYPTO_TAG_LEN;
}

bool cryptoParseWirePacket(const uint8_t *wireBuf, uint8_t wireLen, const uint8_t *key16,
                            uint8_t typeId, uint8_t *senderIdOut6, uint8_t *plaintextOut,
                            uint8_t *ptLenOut) {
  if (wireLen < CRYPTO_OVERHEAD) return false;
  uint8_t ptLen = wireLen - CRYPTO_OVERHEAD;
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
  computeTag(key16, nonce, ciphertext, ptLen, expectedTag);
  if (memcmp(expectedTag, tag, CRYPTO_TAG_LEN) != 0) return false;

  CTR<AESTiny128> ctr;
  ctr.setKey(key16, CRYPTO_KEY_LEN);
  ctr.setIV(nonce, 16);
  ctr.decrypt(plaintextOut, ciphertext, ptLen);

  *ptLenOut = ptLen;
  return true;
}
