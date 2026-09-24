// Тества РЕАЛНИЯ firmware_src/sensor/sensors_io.cpp - конкретно поправките от тази
// сесия: разделеното MSB/LSB четене (без зависимост от реда на оценка) и добавената
// диапазонна проверка (-40..85°C, 0..100% RH) за SHT21. Мокнатите Wire/Adafruit_SHT31
// позволяват пълен round-trip през реалната sensors_read().
#include "test_framework.h"
#include "sensors_io.h"
#include "Wire.h"
#include <cmath>

// Изгражда 3-байтов I2C отговор [MSB][LSB][CRC], точно както sht21_read() го чете -
// CRC байтът се чете, но не се проверява, значи можем да сложим произволна стойност.
static std::vector<uint8_t> sht21Response(uint16_t rawBeforeMask) {
  return { (uint8_t)(rawBeforeMask >> 8), (uint8_t)(rawBeforeMask & 0xFF), 0x00 };
}

// Обратна формула: за да получим желана температура T, каква "raw" стойност
// трябва да подадем? (по същата формула, каквато е в production кода)
static uint16_t rawForTemp(float T) {
  float frac = (T + 46.85f) / 175.72f;
  return (uint16_t)(frac * 65536.0f) & 0xFFFC;
}
static uint16_t rawForHumidity(float RH) {
  float frac = (RH + 6.0f) / 125.0f;
  return (uint16_t)(frac * 65536.0f) & 0xFFFC;
}

TEST(normal_reading_within_range_succeeds) {
  Wire.reset();
  // SHT31 (почва) - истинска Adafruit_SHT31 не е свързана тук, sensors_read() я вика
  // директно (не през Wire мока) - виж отделния тест за soil чрез global sht31 обекта
  // в sensors_io.cpp (static, не е достъпен отвън) - затова тук проверяваме само
  // SHT21 (въздух) частта, която реално минава през Wire.

  // Температура ~23.5°C, валидна
  uint16_t rawT = rawForTemp(23.5f);
  Wire.queueEndTransmissionResult(0);
  Wire.queueResponse(sht21Response(rawT));
  // Влажност ~55%, валидна
  uint16_t rawH = rawForHumidity(55.0f);
  Wire.queueEndTransmissionResult(0);
  Wire.queueResponse(sht21Response(rawH));

  float s_t, s_h, a_t, a_h;
  sensors_read(s_t, s_h, a_t, a_h);

  ASSERT_TRUE(fabs(a_t - 23.5f) < 0.05f);
  ASSERT_TRUE(fabs(a_h - 55.0f) < 0.05f);
}

TEST(end_transmission_failure_yields_sentinel_255) {
  Wire.reset();
  Wire.queueEndTransmissionResult(1); // != 0 = грешка -> sht21_read връща false веднага
  Wire.queueResponse({});             // temp транзакцията никога не стига до read()
  Wire.queueEndTransmissionResult(1);
  Wire.queueResponse({});

  float s_t, s_h, a_t, a_h;
  sensors_read(s_t, s_h, a_t, a_h);

  ASSERT_TRUE(fabs(a_t - 255.0f) < 0.001f);
  ASSERT_TRUE(fabs(a_h - 255.0f) < 0.001f);
}

TEST(insufficient_bytes_available_yields_sentinel_255) {
  Wire.reset();
  Wire.queueEndTransmissionResult(0);
  Wire.queueResponse({0x12}); // само 1 байт вместо 3 - available()<3 -> false
  Wire.queueEndTransmissionResult(0);
  Wire.queueResponse({0x34});

  float s_t, s_h, a_t, a_h;
  sensors_read(s_t, s_h, a_t, a_h);

  ASSERT_TRUE(fabs(a_t - 255.0f) < 0.001f);
  ASSERT_TRUE(fabs(a_h - 255.0f) < 0.001f);
}

TEST(out_of_range_temperature_rejected_as_sentinel) {
  Wire.reset();
  // конструираме raw, който по формулата дава температура далеч извън -40..85°C
  // (напр. raw=0xFFFC -> T = -46.85 + 175.72*(65532/65536) ≈ 128.85°C - невъзможно физически)
  Wire.queueEndTransmissionResult(0);
  Wire.queueResponse(sht21Response(0xFFFC));
  Wire.queueEndTransmissionResult(0);
  Wire.queueResponse(sht21Response(rawForHumidity(50.0f))); // валидна влажност, за контраст

  float s_t, s_h, a_t, a_h;
  sensors_read(s_t, s_h, a_t, a_h);

  ASSERT_TRUE(fabs(a_t - 255.0f) < 0.001f);   // температурата е отхвърлена
  ASSERT_TRUE(fabs(a_h - 50.0f) < 0.05f);     // влажността остава валидна (независима проверка)
}

TEST(out_of_range_humidity_rejected_as_sentinel) {
  Wire.reset();
  Wire.queueEndTransmissionResult(0);
  Wire.queueResponse(sht21Response(rawForTemp(20.0f))); // валидна температура
  Wire.queueEndTransmissionResult(0);
  // raw=0xFFFC за влажност -> RH = -6 + 125*(65532/65536) ≈ 118.99% - невъзможно физически
  Wire.queueResponse(sht21Response(0xFFFC));

  float s_t, s_h, a_t, a_h;
  sensors_read(s_t, s_h, a_t, a_h);

  ASSERT_TRUE(fabs(a_t - 20.0f) < 0.05f);
  ASSERT_TRUE(fabs(a_h - 255.0f) < 0.001f);
}

TEST(msb_lsb_order_matters_asymmetric_raw_decoded_correctly) {
  // Конкретен regression тест за самата поправка: raw стойност, при която
  // разменени MSB/LSB би дала СЪВСЕМ различна (и невалидна) температура -
  // ако кодът някога регресира към недефинирания ред на Wire.read() извиквания
  // и компилаторът реши да ги размени, този тест би хванал грешния резултат.
  Wire.reset();
  // Избираме temp=10.0°C (валидна, в средата на диапазона), но с asymметрични MSB/LSB байтове
  uint16_t rawT = rawForTemp(10.0f);
  uint8_t msb = (uint8_t)(rawT >> 8);
  uint8_t lsb = (uint8_t)(rawT & 0xFF);
  ASSERT_TRUE(msb != lsb); // тестът има смисъл само ако MSB и LSB реално се различават

  Wire.queueEndTransmissionResult(0);
  Wire.queueResponse({msb, lsb, 0x00});
  Wire.queueEndTransmissionResult(0);
  Wire.queueResponse(sht21Response(rawForHumidity(40.0f)));

  float s_t, s_h, a_t, a_h;
  sensors_read(s_t, s_h, a_t, a_h);

  ASSERT_TRUE(fabs(a_t - 10.0f) < 0.05f); // потвърждава MSB е бил третиран като старша половина
}

int main() {
  return runAllTests();
}
