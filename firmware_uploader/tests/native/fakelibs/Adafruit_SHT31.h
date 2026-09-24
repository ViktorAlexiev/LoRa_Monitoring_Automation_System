// Заместител на Adafruit_SHT31.h - stub клас, чиито readTemperature()/readHumidity()
// връщат стойности, зададени изрично от теста (вкл. NAN за симулиране на грешка).
#ifndef FAKE_ADAFRUIT_SHT31_H
#define FAKE_ADAFRUIT_SHT31_H

#include <cmath>
#include <cstdint>

class Adafruit_SHT31 {
public:
  bool begin(uint8_t = 0x44) { return true; }
  float readTemperature() { return nextTemp_; }
  float readHumidity() { return nextHumidity_; }

  // тестова помощна функция
  void setNextReadings(float t, float h) { nextTemp_ = t; nextHumidity_ = h; }

private:
  float nextTemp_ = 25.0f;
  float nextHumidity_ = 50.0f;
};

#endif
