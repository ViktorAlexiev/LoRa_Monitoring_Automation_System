/*
  config_avr.ino
  --------------
  Stage-1 firmware за two-stage upload процеса.

  Качва се ПЪРВО (преди реалния sensor/executor/repeater firmware).
  Слуша серийния порт за пакет от Python програмата във формат:

      CFG:{"id":"CS001","consumers":[{"id":"AB12","pin":"A3"},{"id":"CD34","pin":"10"}]}

  За sensor/repeater (без консуматори) пакетът е просто:

      CFG:{"id":"CS001"}

  Записва данните ПОСЛЕДОВАТЕЛНО в EEPROM (byte по byte, фиксиран layout),
  после отговаря ACK или NACK:<причина> по серийния.

  ВАЖНО: НЕ ползва Arduino String клас никъде - само char[] буфери.
  ATmega328P има само 2KB RAM; String heap allocation-ите лесно
  фрагментират паметта и водят до freeze на чипа.

  След това Python програмата качва РЕАЛНИЯ firmware, който само ЧЕТЕ
  EEPROM при boot (не съдържа parsing/config логика изобщо).
*/

#include <EEPROM.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------
// EEPROM LAYOUT (фиксиран, споделен между config-firmware и реалния firmware)
// ---------------------------------------------------------------------
#define EEPROM_ADDR_MODULE_ID      0   // 6 bytes  (ASCII, null-padded)
#define EEPROM_ADDR_NUM_CONSUMERS  6   // 1 byte
#define EEPROM_ADDR_CONSUMERS      7   // 10 x 6 bytes = 60 bytes
// консуматор запис = 4 bytes ID (ASCII, null-padded) + 2 bytes PIN (ASCII, null-padded)
#define CONSUMER_RECORD_SIZE       6
#define MAX_CONSUMERS              10
#define MODULE_ID_LEN              6
#define CONSUMER_ID_LEN            4
#define PIN_LEN                    2
#define EEPROM_ADDR_FREQUENCY      67  // 4 bytes (uint32_t) - LoRa честота в Hz
                                        // sensor: избраната честота (gateway/repeater)
                                        // executor: честотата към Gateway
                                        // repeater: RX честотата (от Sensor)
#define EEPROM_ADDR_FREQUENCY_TX   71  // 4 bytes (uint32_t) - само за repeater: TX честотата (към Gateway)
#define EEPROM_ADDR_KEY            75  // 16 bytes - AES-128 мрежов ключ (споделен, hardcoded в апа)
#define KEY_LEN                    16
// EEPROM_ADDR_CEILING (адрес 91, 4 bytes) НЕ се пипа тук - управлява се изцяло от реалния
// firmware (nonce counter watermark), config-firmware никога не го докосва, за да не
// reuse-ва nonce след reconfigure на същото устройство. Общ поток за всичко, което
// устройството праща (sensor/repeater HB; executor - и HB, и ACK/NACK споделят го).
// следващ свободен адрес: 75 + 16 = 91

#define CONFIG_TIMEOUT_MS  60000  // общ прозорец - firmware-ът праща READY на всеки 500ms дотогава
// 400 - достатъчен запас над най-дългия реален CFG ред (Executor с 10 консуматора,
// компактен JSON с всички полета вкл. key, ~343 знака) - преди беше 320 и се отрязваше.
#define LINE_BUF_SIZE       400   // статичен буфер (не heap) за входящата линия

char lineBuf[LINE_BUF_SIZE];
uint16_t lineBufPos = 0;

// ---------------------------------------------------------------------
// Forward declarations (arduino-cli auto-prototype generation се дави
// на 2D array параметри - декларираме ръчно, за да няма изненади)
// ---------------------------------------------------------------------
void waitForConfigPacket();
void handleConfigLine(char *json);
void writeConfigToEeprom(const char *moduleId, uint8_t numConsumers,
                          char consumerIds[][CONSUMER_ID_LEN + 1],
                          char consumerPins[][PIN_LEN + 1], uint32_t freqHz, uint32_t freqTxHz,
                          const uint8_t *keyBytes, bool hasKey);
bool extractStringField(const char *src, const char *key, char *out, uint8_t outLen);
bool extractArrayField(const char *src, const char *key, const char **arrStart, const char **arrEnd);
bool hexToBytes(const char *hex, uint8_t *out, uint8_t outLen);
void sendAck();
void sendNack(const char *reason);

// ---------------------------------------------------------------------

void setup() {
  Serial.begin(57600);   // 115200 @ 8MHz има ~8.5% грешка (виж AVR datasheet) - 57600 е стабилен (0.2%)
  waitForConfigPacket();
}

void loop() {
  // config-firmware не прави нищо друго - само чака в setup().
  // Ако е стигнал дотук, значи вече е отговорил ACK/NACK и чака нов upload.
}

// ---------------------------------------------------------------------
// Чака до CONFIG_TIMEOUT_MS, но НЕ разчита на точен timing -
// праща "READY" на всеки 500ms, за да знае Python кога firmware-ът
// реално слуша сериен порт (независимо кога е натиснат reset).
// Чете байт по байт в статичен буфер (без String клас).
// ---------------------------------------------------------------------
void waitForConfigPacket() {
  unsigned long startTime = millis();
  unsigned long lastReadyMsg = 0;
  lineBufPos = 0;

  while (millis() - startTime < CONFIG_TIMEOUT_MS) {
    if (millis() - lastReadyMsg >= 500) {
      Serial.println(F("READY"));
      lastReadyMsg = millis();
    }

    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') {
        lineBuf[lineBufPos] = '\0';
        if (strncmp(lineBuf, "CFG:", 4) == 0) {
          handleConfigLine(lineBuf + 4);
          return;
        }
        lineBufPos = 0; // не е CFG линия - изчисти и продължи да чакаш
      } else if (c != '\r' && lineBufPos < LINE_BUF_SIZE - 1) {
        lineBuf[lineBufPos++] = c;
      }
    }
  }
  sendNack("Timeout - няма CFG пакет в рамките на прозореца");
}

// ---------------------------------------------------------------------
void handleConfigLine(char *json) {
  char moduleId[MODULE_ID_LEN + 1];
  if (!extractStringField(json, "id", moduleId, sizeof(moduleId))) {
    sendNack("Липсва 'id' поле");
    return;
  }
  if (strlen(moduleId) == 0) {
    sendNack("Невалидна дължина на module id");
    return;
  }

  const char *arrStart = NULL;
  const char *arrEnd = NULL;
  bool hasConsumers = extractArrayField(json, "consumers", &arrStart, &arrEnd);

  char consumerIds[MAX_CONSUMERS][CONSUMER_ID_LEN + 1];
  char consumerPins[MAX_CONSUMERS][PIN_LEN + 1];
  uint8_t numConsumers = 0;

  if (hasConsumers) {
    const char *pos = arrStart;
    while (numConsumers < MAX_CONSUMERS && pos < arrEnd) {
      const char *objStart = strchr(pos, '{');
      if (!objStart || objStart >= arrEnd) break;
      const char *objEnd = strchr(objStart, '}');
      if (!objEnd || objEnd > arrEnd) break;

      char objBuf[48];
      size_t objLen = objEnd - objStart + 1;
      if (objLen >= sizeof(objBuf)) objLen = sizeof(objBuf) - 1;
      memcpy(objBuf, objStart, objLen);
      objBuf[objLen] = '\0';

      char cid[CONSUMER_ID_LEN + 1];
      char cpin[PIN_LEN + 1];
      if (!extractStringField(objBuf, "id", cid, sizeof(cid)) ||
          !extractStringField(objBuf, "pin", cpin, sizeof(cpin))) {
        sendNack("Невалиден консуматор запис");
        return;
      }
      if (strlen(cid) == 0) {
        sendNack("Невалидна дължина на consumer id");
        return;
      }
      if (strlen(cpin) == 0) {
        sendNack("Невалидна дължина на pin");
        return;
      }

      strncpy(consumerIds[numConsumers], cid, CONSUMER_ID_LEN + 1);
      strncpy(consumerPins[numConsumers], cpin, PIN_LEN + 1);
      numConsumers++;

      pos = objEnd + 1;
    }
  }

  // freq / freq_tx (опционални - изпращат се от sensor/executor/repeater; repeater праща и двете)
  char freqStr[16];
  uint32_t freqHz = 0;
  if (extractStringField(json, "freq", freqStr, sizeof(freqStr))) {
    freqHz = (uint32_t)strtoul(freqStr, NULL, 10);
  }
  char freqTxStr[16];
  uint32_t freqTxHz = 0;
  if (extractStringField(json, "freq_tx", freqTxStr, sizeof(freqTxStr))) {
    freqTxHz = (uint32_t)strtoul(freqTxStr, NULL, 10);
  }

  // key (опционален по формат на пакета, но апа винаги го праща) - 32 hex символа = 16 bytes
  char keyStr[2 * KEY_LEN + 1];
  uint8_t keyBytes[KEY_LEN];
  bool hasKey = false;
  if (extractStringField(json, "key", keyStr, sizeof(keyStr)) && strlen(keyStr) == 2 * KEY_LEN) {
    hasKey = hexToBytes(keyStr, keyBytes, KEY_LEN);
  }

  writeConfigToEeprom(moduleId, numConsumers, consumerIds, consumerPins, freqHz, freqTxHz,
                       keyBytes, hasKey);
  sendAck();
}

// ---------------------------------------------------------------------
// Записва последователно: module id -> num_consumers -> всеки консуматор -> честота(и)
// ---------------------------------------------------------------------
void writeConfigToEeprom(const char *moduleId, uint8_t numConsumers,
                          char consumerIds[][CONSUMER_ID_LEN + 1],
                          char consumerPins[][PIN_LEN + 1], uint32_t freqHz, uint32_t freqTxHz,
                          const uint8_t *keyBytes, bool hasKey) {
  int addr = EEPROM_ADDR_MODULE_ID;

  // 1. Module ID (6 bytes, null-padded), byte по byte
  for (int i = 0; i < MODULE_ID_LEN; i++) {
    char c = (i < (int)strlen(moduleId)) ? moduleId[i] : 0x00;
    EEPROM.update(addr, c);
    addr++;
  }

  // 2. Брой консуматори (1 byte)
  EEPROM.update(EEPROM_ADDR_NUM_CONSUMERS, numConsumers);
  addr = EEPROM_ADDR_CONSUMERS;

  // 3. Всеки консуматор последователно: 4 bytes ID + 2 bytes PIN
  for (uint8_t i = 0; i < MAX_CONSUMERS; i++) {
    if (i < numConsumers) {
      for (int j = 0; j < CONSUMER_ID_LEN; j++) {
        char c = (j < (int)strlen(consumerIds[i])) ? consumerIds[i][j] : 0x00;
        EEPROM.update(addr, c);
        addr++;
      }
      for (int j = 0; j < PIN_LEN; j++) {
        char c = (j < (int)strlen(consumerPins[i])) ? consumerPins[i][j] : 0x00;
        EEPROM.update(addr, c);
        addr++;
      }
    } else {
      // празен слот - запълни с 0x00
      for (int j = 0; j < CONSUMER_RECORD_SIZE; j++) {
        EEPROM.update(addr, 0x00);
        addr++;
      }
    }
  }

  // 4. Честота(и) - пишат се само ако са подадени ("freq"/"freq_tx" присъстват в пакета).
  // Ако не са подадени, старата стойност в EEPROM остава недокосната.
  if (freqHz != 0) {
    EEPROM.put(EEPROM_ADDR_FREQUENCY, freqHz);
  }
  if (freqTxHz != 0) {
    EEPROM.put(EEPROM_ADDR_FREQUENCY_TX, freqTxHz);
  }

  // 5. Мрежов ключ - пише се само ако е подаден валиден hex низ; иначе старата
  // стойност в EEPROM остава недокосната (същия pattern като честотата).
  if (hasKey) {
    addr = EEPROM_ADDR_KEY;
    for (uint8_t i = 0; i < KEY_LEN; i++) {
      EEPROM.update(addr, keyBytes[i]);
      addr++;
    }
  }
}

// ---------------------------------------------------------------------
// Малки помощни функции за parsing (НЕ е пълен JSON parser -
// разчита на фиксирания формат, който винаги праща Python програмата).
// Работят с char* буфери - без динамична алокация.
// ---------------------------------------------------------------------
bool extractStringField(const char *src, const char *key, char *out, uint8_t outLen) {
  char pattern[16];
  snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
  const char *p = strstr(src, pattern);
  if (!p) return false;
  p += strlen(pattern);
  const char *end = strchr(p, '"');
  if (!end) return false;
  size_t len = end - p;
  if (len >= outLen) len = outLen - 1;
  strncpy(out, p, len);
  out[len] = '\0';
  return true;
}

bool extractArrayField(const char *src, const char *key, const char **arrStart, const char **arrEnd) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
  const char *p = strstr(src, pattern);
  if (!p) return false;
  p += strlen(pattern);
  const char *end = strchr(p, ']');
  if (!end) return false;
  *arrStart = p;
  *arrEnd = end;
  return true;
}

bool hexToBytes(const char *hex, uint8_t *out, uint8_t outLen) {
  for (uint8_t i = 0; i < outLen; i++) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    int8_t hiVal = -1, loVal = -1;
    if (hi >= '0' && hi <= '9') hiVal = hi - '0';
    else if (hi >= 'a' && hi <= 'f') hiVal = hi - 'a' + 10;
    else if (hi >= 'A' && hi <= 'F') hiVal = hi - 'A' + 10;
    if (lo >= '0' && lo <= '9') loVal = lo - '0';
    else if (lo >= 'a' && lo <= 'f') loVal = lo - 'a' + 10;
    else if (lo >= 'A' && lo <= 'F') loVal = lo - 'A' + 10;
    if (hiVal < 0 || loVal < 0) return false;
    out[i] = (uint8_t)((hiVal << 4) | loVal);
  }
  return true;
}

void sendAck() {
  Serial.println(F("ACK"));
}

void sendNack(const char *reason) {
  Serial.print(F("NACK:"));
  Serial.println(reason);
}
