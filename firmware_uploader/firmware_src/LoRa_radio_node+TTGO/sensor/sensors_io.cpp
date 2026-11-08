#include "sensors_io.h"
#include <Wire.h>
#include "debug_log.h"

// Физически разумни граници - извън тях приемаме, че стойността е боклук (счупен сензор,
// разменени байтове, шум по I2C) и я третираме като грешка на четенето.
#define SHT21_TEMP_MIN_C   -40.0f
#define SHT21_TEMP_MAX_C    85.0f
#define SHT21_RH_MIN_PCT     0.0f
#define SHT21_RH_MAX_PCT   100.0f
// SHT31 по datasheet: -40..125 C; 1 C запас, за да не стане реална стойност на границата "няма данни"
// (същите граници като в ESP-IDF прошивката)
#define SHT31_TEMP_MIN_C  -41.0f
#define SHT31_TEMP_MAX_C  126.0f

// Sensirion CRC-8 (полином 0x31). SHT31: начална стойност 0xFF; SHT21: 0x00.
static uint8_t crc8(const uint8_t *d, uint8_t n, uint8_t init) {
  uint8_t crc = init;
  for (uint8_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

// ---------------- SHT31 (почва): суров I2C, като в ESP-IDF прошивката ----------------
// Една команда = едно измерване и на температурата, и на влажността (Adafruit_SHT31 правеше ДВЕ измервания - по едно
// за readTemperature() и readHumidity() - и зависеше от версията на библиотеката).
static bool sht31_read(float &t, float &h) {
  Wire.beginTransmission(SHT31_ADDR);
  Wire.write((uint8_t)0x24);              // еднократно измерване, високо повторение,
  Wire.write((uint8_t)0x00);              // без clock stretching
  if (Wire.endTransmission() != 0) return false;

  delay(20);                              // измерването трае до 15 ms

  if (Wire.requestFrom((uint8_t)SHT31_ADDR, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (uint8_t i = 0; i < 6; i++) b[i] = Wire.read();
  if (crc8(b, 2, 0xFF) != b[2] || crc8(b + 3, 2, 0xFF) != b[5]) return false;

  uint16_t rt = ((uint16_t)b[0] << 8) | b[1];
  uint16_t rh = ((uint16_t)b[3] << 8) | b[4];
  float tc = -45.0f + 175.0f * ((float)rt / 65535.0f);
  float hc = 100.0f * ((float)rh / 65535.0f);
  if (tc < SHT31_TEMP_MIN_C || tc > SHT31_TEMP_MAX_C) return false;
  t = tc;
  h = hc;
  return true;
}

// ---------------- SHT21 raw I2C (no-hold master read) ----------------
static bool sht21_read(uint8_t cmd, float &result, bool isTemp) {
  Wire.beginTransmission(SHT21_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission() != 0) return false;

  delay(isTemp ? 90 : 30);   // макс. време за конверсия + запас

  Wire.requestFrom(SHT21_ADDR, (uint8_t)3);
  if (Wire.available() < 3) return false;

  // Редът на изчисление на операндите на << / | НЕ е дефиниран от C++ стандарта, а
  // Wire.read() има странични ефекти (консумира байт от буфера) - затова четем MSB и LSB
  // в отделни редове, за да е гарантиран редът (MSB първи, съгласно SHT21 протокола).
  uint8_t msb = Wire.read();
  uint8_t lsb = Wire.read();
  uint8_t crc = Wire.read();
  const uint8_t data[2] = {msb, lsb};
  if (crc8(data, 2, 0x00) != crc) return false;   // CRC на SHT21 (доскоро не се проверяваше)

  uint16_t raw = ((uint16_t)msb << 8) | lsb;
  raw &= 0xFFFC;

  float value = isTemp ? (-46.85f + 175.72f * ((float)raw / 65536.0f))
                        : (-6.0f  + 125.0f  * ((float)raw / 65536.0f));

  // Диапазонна проверка - хваща и грешен байт-ред (ако все пак се случи), и друг вид шум:
  // резултат извън физически възможното означава невалидно измерване, не число, на което
  // може да се разчита.
  if (isTemp) {
    if (value < SHT21_TEMP_MIN_C || value > SHT21_TEMP_MAX_C) return false;
  } else {
    if (value < SHT21_RH_MIN_PCT || value > SHT21_RH_MAX_PCT) return false;
  }

  result = value;
  return true;
}

void sensors_begin() {
  Wire.begin();
  Wire.beginTransmission(SHT31_ADDR);
  Wire.write((uint8_t)0x30);              // soft reset (както Adafruit begin() и ESP-IDF прошивката)
  Wire.write((uint8_t)0xA2);
  Wire.endTransmission();                 // липсващ сензор не е грешка тук - ще се види при четенето
  delay(2);
}

void sensors_end() {
  Wire.end();
}

void sensors_read(float &s_t, float &s_h, float &a_t, float &a_h) {
  // --- SHT31 (почва) ---
  float st, sh;
  bool soilOk = sht31_read(st, sh);
  s_t = soilOk ? st : 255.0f;
  s_h = soilOk ? sh : 255.0f;

  // --- SHT21 (въздух) ---
  float at, ah;
  bool airTOk = sht21_read(0xF3, at, true);
  bool airHOk = sht21_read(0xF5, ah, false);
  a_t = airTOk ? at : 255.0f;
  a_h = airHOk ? ah : 255.0f;

  if (!soilOk) DBGLN(F("[WARN] SHT31 (почва) - грешка при четене/CRC/извън диапазон, изпращам 255.0"));
  if (!airTOk)  DBGLN(F("[WARN] SHT21 темп. (въздух) - грешка/CRC/извън диапазон, изпращам 255.0"));
  if (!airHOk)  DBGLN(F("[WARN] SHT21 влажност (въздух) - грешка/CRC/извън диапазон, изпращам 255.0"));
}
