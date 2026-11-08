// Помощници за тестове на приложенията, които зависят от радио слоя (agro_radio).
#ifndef ESPIDF_TEST_RADIO_SUPPORT_H
#define ESPIDF_TEST_RADIO_SUPPORT_H

#include "agro_radio.h"
#include "test_support.h"

// Какво получава чипът за поискани dBm: референтен прогон на radio_setup() върху чист симулатор (връща настройката на
// мощността на чипа - тя не е равна на dBm, защото RadioLib ползва таблица с оптимизирани настройки на PA).
// Нулира симулатора - вика се ПРЕДИ подготовката на теста.
inline int refPowerSetting(int dbm) {
  static int result;
  sim::reset();
  runAsTask([dbm] {
    RadioParams p;
    p.freqHz = 433000000; p.sf = 7; p.bwHz = 125000; p.txPowerDbm = (int8_t)dbm;
    radio_setup(&p, 3);
    result = sim::radio.txPowerDbm();
  }, 20000);
  sim::reset();
  return result;
}

// NVS "txp" е със знак (u8 в два комплемента), както го записва config приложението
inline void nvsTxPower(int dbm) { sim::nvs.setU8("txp", (uint8_t)(int8_t)dbm); }

// Поисканата (заявена) мощност - каквото ограничението на модула позволява
inline int allowedDbm(int dbm) { return radio_clamp_tx_power(dbm); }

#endif
