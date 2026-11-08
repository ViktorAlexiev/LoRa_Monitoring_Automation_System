/*
  crypto_common.h / crypto_common.cpp  (ESP-IDF: mbedtls)
  --------------------------------------------------------
  Споделен AES-128-CTR + AES-CMAC "encrypt-then-MAC" протокол, ползван от всички устройства
  (sensor/executor/repeater/gateway) с ЕДИН общ мрежов ключ. WIRE ФОРМАТЪТ Е БАЙТ ПО БАЙТ
  ИДЕНТИЧЕН със старата Arduino версия (библиотеки Crypto + AES_CMAC) - ESP-IDF и Arduino
  модулите могат да работят в една и съща LoRa мрежа.

  Wire формат (за произволен plaintext):
    [senderId, 6 bytes, чисто] [ciphertext, ptLen bytes] [counter, 4 bytes BE, чисто] [tag, 4 bytes]

  senderId + typeId (само в кода, никога не се предава) + counter изграждат nonce-а
  (16 B: type, senderId, counter BE, нули). typeId разграничава "потоците" (sensor данни,
  executor HB, ACK, repeater HB, команди, state request/response), за да не се преизползва
  nonce между тях при еднакъв counter.

  Тук: AES-128-CTR = mbedtls_aes_crypt_ctr (хардуерно AES на ESP32-S3), CMAC = mbedtls_cipher_cmac
  (изисква CONFIG_MBEDTLS_CMAC_C=y - виж sdkconfig.defaults).
*/
#ifndef CRYPTO_COMMON_H
#define CRYPTO_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CRYPTO_KEY_LEN    16
#define CRYPTO_ID_LEN      6
#define CRYPTO_CNT_LEN     4
#define CRYPTO_TAG_LEN     4
#define CRYPTO_OVERHEAD  (CRYPTO_ID_LEN + CRYPTO_CNT_LEN + CRYPTO_TAG_LEN)  // 14 bytes
// max plaintext: state response носи до 10 консуматора x 5 bytes = 50 bytes
#define CRYPTO_MAX_PT     50

// type-id-та - вътрешни константи, НЕ се предават по въздух (същите като в Arduino версията)
#define CRYPTO_TYPE_SENSOR              1
#define CRYPTO_TYPE_REPEATER_HB         2
#define CRYPTO_TYPE_EXEC_HB             3
#define CRYPTO_TYPE_EXEC_ACK            4
#define CRYPTO_TYPE_GW_CMD              5
#define CRYPTO_TYPE_STATE_REQ           6   // Gateway -> Executor, заявка за състояния (празен plaintext)
#define CRYPTO_TYPE_STATE_RESP          7   // Executor -> Gateway, отговор на изрична заявка
#define CRYPTO_TYPE_STATE_RESP_RESTART  8   // Executor -> Gateway, автоматичен отговор след boot
#define CRYPTO_TYPE_STATE_RESP_ACK      9   // Gateway -> Executor, потвърждение само за RESTART варианта

// Строи wire пакет. wireBufOut >= ptLen + CRYPTO_OVERHEAD. Връща общата дължина; 0 при грешка
// (ptLen > CRYPTO_MAX_PT или провал на mbedtls).
uint8_t cryptoBuildWirePacket(uint8_t *wireBufOut, const uint8_t *key16, uint8_t typeId,
                               const uint8_t *senderId6, uint32_t counter,
                               const uint8_t *plaintext, uint8_t ptLen);

// Разпарсва и проверява wire пакет. false при невалиден MAC / дължина. senderIdOut6 винаги се
// попълва (при дължина >= CRYPTO_OVERHEAD), plaintextOut е валиден САМО при true.
bool cryptoParseWirePacket(const uint8_t *wireBuf, uint8_t wireLen, const uint8_t *key16,
                            uint8_t typeId, uint8_t *senderIdOut6, uint8_t *plaintextOut,
                            uint8_t *ptLenOut);

#endif
