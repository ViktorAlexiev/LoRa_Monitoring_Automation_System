/*
  ceiling_counter.h / ceiling_counter.cpp  (ESP-IDF - NVS + RTC памет)
  -------------------------------------------------------------------
  "Ceiling" watermark за nonce counter-и. НЕ дава replay-protection - целта е само подателят
  никога да не преизползва (ключ, nonce) двойка след unclean reset.

  При power-on/reset чете последния committed ceiling от NVS (namespace "cfg", ключ по избор) и
  продължава оттам. На всеки CEILING_COMMIT_INTERVAL (100) стойности пише нов ceiling в NVS ПРЕДИ
  да ползва следващата стойност - при unclean reset устройството скача напред максимум 100, никога
  назад.

  Събуждане от дълбок сън (sensor) НЕ е unclean reset: текущата и committed стойност се пазят в
  RTC паметта (RTC_DATA_ATTR), така че при всяко събуждане не се пише във flash.

  next() се вика от ЕДНА задача на устройство (виж приложенията) - класът не е thread-safe.
*/
#ifndef CEILING_COUNTER_H
#define CEILING_COUNTER_H

#include <stdint.h>

#define CEILING_COMMIT_INTERVAL UINT32_C(100)

class CeilingCounter {
 public:
  void begin(const char *nvsKey);   // напр. "ceil"
  // Следваща безопасна стойност; commit-ва в NVS при нужда. false = записът в NVS се провали: НЕ се
  // връща стойност (повторен nonce е по-лошо от пропуснат пакет) - извикващият пропуска предаването.
  bool next(uint32_t *out);

 private:
  const char *key = nullptr;
  uint32_t current = 0;
  uint32_t committed = 0;
};

#endif
