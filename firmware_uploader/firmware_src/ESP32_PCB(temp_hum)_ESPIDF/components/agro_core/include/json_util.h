/*
  json_util.h - тънък слой над cJSON (проверената JSON библиотека, която идва с ESP-IDF).

  Самият разбор и печат е на cJSON. Тук има само това, което проектът изисква отгоре му:

  JsonDoc      разбира текст (указател + дължина, НЕ е нужен терминатор) и е валиден само ако текстът е
               ТОЧНО ЕДИН JSON обект без остатък (освен интервали). Отрязан/повреден ред се отхвърля цял -
               нужно е за конфигурацията по сериен порт. Вложеност над JSON_MAX_DEPTH се отхвърля ПРЕДИ
               разбора: cJSON е рекурсивна, а стековете на задачите са малки (MQTT съобщение от мрежата
               не бива да може да ги препълни).
  jsonField    стойност-низ на ключ от обект (с различаване на малки/главни букви). Различава
               "липсва / не е низ" от "не се побира в буфера" (по-дълго от очакваното е невалидно - НЕ се
               реже тихо: отрязан M_ID може да съвпадне с друг модул).
  JsonOut      строи JSON дърво с cJSON и го печата в буфер на извикващия (cJSON_PrintPreallocated - без
               втори heap буфер за изхода). При неуспех (липса на памет, не се побира) ok() е false и
               буферът НЕ се доверява. Числата с десетични знаци се подават като цели стотни (fixed2).

  cJSON заделя памет (malloc) за дървото; JsonDoc и JsonOut я освобождават в деструктора. Ползва се само
  в gateway, executor_wifi и config (устройствата на батерия не работят с JSON).
*/
#ifndef AGRO_JSON_UTIL_H
#define AGRO_JSON_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "cJSON.h"

#define JSON_MAX_DEPTH 8

class JsonDoc {
 public:
  JsonDoc(const char *text, size_t len);
  ~JsonDoc();
  JsonDoc(const JsonDoc &) = delete;
  JsonDoc &operator=(const JsonDoc &) = delete;

  bool valid() const { return root_ != nullptr; }
  const cJSON *root() const { return root_; }

 private:
  cJSON *root_;
};

enum JsonField { JSON_FIELD_MISSING, JSON_FIELD_OK, JSON_FIELD_TOO_LONG };

// out е винаги валиден низ (празен при MISSING/TOO_LONG).
JsonField jsonField(const cJSON *obj, const char *key, char *out, size_t cap);

inline bool jsonGetString(const cJSON *obj, const char *key, char *out, size_t cap) {
  return jsonField(obj, key, out, cap) == JSON_FIELD_OK;
}

class JsonOut {
 public:
  JsonOut();
  ~JsonOut();
  JsonOut(const JsonOut &) = delete;
  JsonOut &operator=(const JsonOut &) = delete;

  JsonOut &beginObject();                       // най-горно ниво (един път) или елемент на масив
  JsonOut &endObject();
  JsonOut &beginArray(const char *key);
  JsonOut &endArray();

  JsonOut &str(const char *key, const char *val);
  JsonOut &num(const char *key, int32_t v);
  JsonOut &unum(const char *key, uint32_t v);
  JsonOut &fixed2(const char *key, int32_t hundredths);   // 2257 -> 22.57, -5 -> -0.05
  JsonOut &boolean(const char *key, bool v);

  bool ok() const { return ok_ && root_ != nullptr; }

  // Печата компактно в buf (с терминатор). false, ако дървото е неуспешно или не се побира; *outLen = дължина.
  bool print(char *buf, size_t cap, size_t *outLen = nullptr);

 private:
  cJSON *cur() const { return depth_ > 0 ? stack_[depth_ - 1] : nullptr; }
  bool push(cJSON *c);
  void fail() { ok_ = false; }

  cJSON *root_;
  cJSON *stack_[JSON_MAX_DEPTH];
  uint8_t depth_;
  bool ok_;
};

#endif
