/*
  ceiling_counter.h / ceiling_counter.cpp  (ESP32 - NVS + RTC памет)
  -------------------------------------------------------------------
  "Ceiling" watermark за nonce counter-и. НЕ дава replay-protection - целта е само подателят
  никога да не преизползва (ключ, nonce) двойка след unclean reset.

  При power-on/reset чете последния committed ceiling от NVS (Preferences, namespace "cfg",
  ключ по избор) и продължава оттам. На всеки CEILING_COMMIT_INTERVAL (100) стойности пише нов
  ceiling в NVS ПРЕДИ да ползва следващата стойност - при unclean reset устройството скача напред
  максимум 100, никога назад.

  Събуждане от дълбок сън (sensor) НЕ е unclean reset: текущата и committed стойност се пазят в
  RTC паметта (RTC_DATA_ATTR), така че при всяко събуждане не се пише във flash.

  Копие седи в sensor/, executor/, repeater/ (по една инстанция на устройство).
*/

#ifndef CEILING_COUNTER_H
#define CEILING_COUNTER_H

#include <Arduino.h>

#define CEILING_COMMIT_INTERVAL 100UL

class CeilingCounter {
  public:
    void begin(const char* nvsKey);   // напр. "ceil"
    uint32_t next();                  // следваща безопасна стойност; commit-ва в NVS при нужда

  private:
    const char* key;
    uint32_t current;
    uint32_t committed;
};

#endif
