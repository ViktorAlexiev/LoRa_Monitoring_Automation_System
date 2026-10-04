/*
  crypto_common.h / crypto_common.cpp
  ------------------------------------
  Споделен AES-128-CTR + AES-CMAC "encrypt-then-MAC" протокол, ползван от всички
  устройства (sensor/executor/repeater/gateway) с ЕДИН общ мрежов ключ.

  Wire формат (генеричен, за произволен plaintext):
    [senderId, 6 bytes, чисто] [ciphertext, ptLen bytes] [counter, 4 bytes BE, чисто] [tag, 4 bytes]

  senderId + typeId (само в кода, никога не се предава) + counter изграждат nonce-а -
  затова senderId и counter пътуват в чисто (получателят се нуждае от тях, за да
  реконструира nonce-а преди декриптиране). typeId се определя от контекста (кой пакет
  очакваш), не се праща по въздух - разграничава различните "потоци" (sensor данни,
  executor HB, executor ACK, repeater HB, gateway команди, state request/response), за
  да не се преизползва nonce между тях дори при еднакъв counter.

  Копие на тези 2 файла седи във всяка от папките sensor/, executor/, repeater/,
  gateway/ (arduino-cli компилира всяка папка като отделен sketch - няма споделени
  файлове между sketch-ове, само инсталирани библиотеки).

  Изисква Arduino библиотеки: "Crypto" (Rhys Weatherley) + "AES_CMAC".
*/

#ifndef CRYPTO_COMMON_H
#define CRYPTO_COMMON_H

#include <Arduino.h>

#define CRYPTO_KEY_LEN    16
#define CRYPTO_ID_LEN      6
#define CRYPTO_CNT_LEN     4
#define CRYPTO_TAG_LEN     4
#define CRYPTO_OVERHEAD  (CRYPTO_ID_LEN + CRYPTO_CNT_LEN + CRYPTO_TAG_LEN)  // 14 bytes
// max plaintext, който протоколът поддържа - state response носи до MAX_CONSUMERS(10) х
// 5 bytes (C_ID 4 + state 1) = 50 bytes, най-големият ни payload.
#define CRYPTO_MAX_PT     50

// type-id-та - вътрешни константи, НЕ се предават по въздух. Разграничават потоците,
// за да имат отделен nonce space дори при споделен ключ и евентуално съвпадащ counter.
#define CRYPTO_TYPE_SENSOR              1
#define CRYPTO_TYPE_REPEATER_HB         2
#define CRYPTO_TYPE_EXEC_HB             3
#define CRYPTO_TYPE_EXEC_ACK            4
#define CRYPTO_TYPE_GW_CMD              5
#define CRYPTO_TYPE_STATE_REQ           6   // Gateway -> Executor, заявка за състояния (празен plaintext)
#define CRYPTO_TYPE_STATE_RESP          7   // Executor -> Gateway, отговор на изрична заявка
#define CRYPTO_TYPE_STATE_RESP_RESTART  8   // Executor -> Gateway, автоматичен отговор веднага след boot
#define CRYPTO_TYPE_STATE_RESP_ACK      9   // Gateway -> Executor, потвърждение само за RESTART варианта

// Строи wire пакет от plaintext. wireBufOut трябва да е >= ptLen + CRYPTO_OVERHEAD bytes.
// Връща общата дължина на построения wire пакет.
uint8_t cryptoBuildWirePacket(uint8_t *wireBufOut, const uint8_t *key16, uint8_t typeId,
                               const uint8_t *senderId6, uint32_t counter,
                               const uint8_t *plaintext, uint8_t ptLen);

// Разпарсва и проверява wire пакет. Връща false при невалиден MAC (tampered/грешен ключ) -
// НЕ се доверявай на plaintextOut в такъв случай. senderIdOut6 винаги се попълва
// (нужен е дори при невалиден MAC, за да прочетеш "кой се представя за подател").
bool cryptoParseWirePacket(const uint8_t *wireBuf, uint8_t wireLen, const uint8_t *key16,
                            uint8_t typeId, uint8_t *senderIdOut6, uint8_t *plaintextOut,
                            uint8_t *ptLenOut);

#endif
