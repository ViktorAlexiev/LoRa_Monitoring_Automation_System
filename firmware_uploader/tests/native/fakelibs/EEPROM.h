// Заместител на Arduino EEPROM.h - EEPROM instance backed от статичен масив в паметта
// (1024 байта, колкото има ATmega328P). Тестовете могат директно да четат/пишат
// eepromBackingStore[], за да проверят инвариантите на CeilingCounter/config_storage.
#ifndef FAKE_EEPROM_H
#define FAKE_EEPROM_H

#include <cstdint>
#include <cstring>
#include <cstddef>

#define EEPROM_TEST_SIZE 1024
extern uint8_t eepromBackingStore[EEPROM_TEST_SIZE];

class FakeEEPROMClass {
public:
  uint8_t read(int addr) const {
    return eepromBackingStore[addr];
  }
  void write(int addr, uint8_t val) {
    eepromBackingStore[addr] = val;
  }
  void update(int addr, uint8_t val) {
    if (eepromBackingStore[addr] != val) eepromBackingStore[addr] = val;
  }
  template <typename T>
  T& get(int addr, T &t) {
    memcpy(&t, eepromBackingStore + addr, sizeof(T));
    return t;
  }
  template <typename T>
  const T& put(int addr, const T &t) {
    memcpy(eepromBackingStore + addr, &t, sizeof(T));
    return t;
  }
};
extern FakeEEPROMClass EEPROM;

// Помощна функция за тестовете - запълва EEPROM с 0xFF (фабрично неинициализирано
// състояние), точно както реален нов/изтрит AVR EEPROM chip.
inline void eeprom_reset_erased() {
  memset(eepromBackingStore, 0xFF, EEPROM_TEST_SIZE);
}

#endif
