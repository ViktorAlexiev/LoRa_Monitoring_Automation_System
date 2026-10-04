#include "ceiling_counter.h"
#include <EEPROM.h>

void CeilingCounter::begin(int eepromAddr) {
  addr = eepromAddr;
  uint32_t stored;
  EEPROM.get(addr, stored);
  if (stored == 0xFFFFFFFFUL) stored = 0;   // неинициализиран EEPROM (нов chip)
  current = stored;
  committed = stored;
}

uint32_t CeilingCounter::next() {
  if (current >= committed) {
    committed = current + CEILING_COMMIT_INTERVAL;
    EEPROM.put(addr, committed);
  }
  uint32_t val = current;
  current++;
  return val;
}
