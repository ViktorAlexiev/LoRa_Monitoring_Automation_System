/*
  ceiling_counter.h / ceiling_counter.cpp
  -----------------------------------------
  EEPROM "ceiling" watermark за nonce counter-и (AVR устройства). НЕ дава replay-protection
  (никой не проверява "по-голямо ли е от последно видяното на приемника") - целта е само
  подателят никога да не преизползва (ключ, nonce) двойка след unclean reset.

  При boot чете последния committed ceiling и продължава оттам. На всеки
  CEILING_COMMIT_INTERVAL (100) стойности, пише нов ceiling в EEPROM ПРЕДИ да ползва
  следващата стойност - при unclean reset устройството скача напред максимум 100, никога назад.

  Копие седи в sensor/, executor/, repeater/ (executor ползва 2 инстанции - HB и ACK потоци).
*/

#ifndef CEILING_COUNTER_H
#define CEILING_COUNTER_H

#include <Arduino.h>

#define CEILING_COMMIT_INTERVAL UINT32_C(100)

class CeilingCounter {
  public:
    void begin(uint16_t eepromAddr);
    uint32_t next();   // връща следваща безопасна стойност за ползване, commit-ва в EEPROM при нужда
    uint32_t peek() const { return current; }   // следващата стойност БЕЗ да я консумира (за seed на random())

  private:
    uint16_t addr;
    uint32_t current;
    uint32_t committed;
};

#endif
