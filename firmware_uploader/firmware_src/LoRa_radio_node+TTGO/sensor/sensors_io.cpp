#include "sensors_io.h"
#include <Wire.h>
#include <Adafruit_SHT31.h>

static Adafruit_SHT31 sht31 = Adafruit_SHT31();

// Физически разумни граници - извън тях приемаме, че стойността е боклук (счупен сензор,
// разменени байтове, шум по I2C) и я третираме като грешка на четенето.
#define SHT21_TEMP_MIN_C   -40.0f
#define SHT21_TEMP_MAX_C    85.0f
#define SHT21_RH_MIN_PCT     0.0f
#define SHT21_RH_MAX_PCT   100.0f

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
  uint16_t msb = Wire.read();
  uint16_t lsb = Wire.read();
  uint16_t raw = (msb << 8) | lsb;
  raw &= 0xFFFC;
  Wire.read(); // CRC - не се проверява

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

void sensors_init() {
  Wire.begin();
  sht31.begin(SHT31_ADDR);
}

void sensors_read(float &s_t, float &s_h, float &a_t, float &a_h) {
  // --- SHT31 (почва) ---
  float st = sht31.readTemperature();
  float sh = sht31.readHumidity();
  bool soilOk = !isnan(st) && !isnan(sh);
  s_t = soilOk ? st : 255.0f;
  s_h = soilOk ? sh : 255.0f;

  // --- SHT21 (въздух) ---
  float at, ah;
  bool airTOk = sht21_read(0xF3, at, true);
  bool airHOk = sht21_read(0xF5, ah, false);
  a_t = airTOk ? at : 255.0f;
  a_h = airHOk ? ah : 255.0f;

  if (!soilOk) Serial.println(F("[WARN] SHT31 (почва) - грешка при четене, изпращам 255.0"));
  if (!airTOk)  Serial.println(F("[WARN] SHT21 темп. (въздух) - грешка/извън диапазон, изпращам 255.0"));
  if (!airHOk)  Serial.println(F("[WARN] SHT21 влажност (въздух) - грешка/извън диапазон, изпращам 255.0"));
}
