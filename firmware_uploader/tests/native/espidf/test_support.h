// Общи помощници за тестовете на ESP-IDF прошивките.
#ifndef ESPIDF_TEST_SUPPORT_H
#define ESPIDF_TEST_SUPPORT_H

#include <cmath>
#include <functional>
#include <cstring>
#include <string>
#include <vector>

#include "../test_framework.h"
#include "cJSON.h"
#include "sim.h"
#include "sim_net.h"
#include "sim_periph.h"

// Броене на заделянията на cJSON и инжектиране на липса на памет (cJSON_InitHooks).
//   CjHooks(failAt)      - проваля заделянето с пореден номер failAt (от 0)
//   CjHooks(0, true)     - проваля ВСЯКО заделяне на cJSON
struct CjStats { int allocs = 0, frees = 0, allocNo = 0, failAt = -1; bool failAll = false; };
inline CjStats &cjStats() { static CjStats s; return s; }
inline void *cjMalloc(size_t n) {
  CjStats &s = cjStats();
  if (s.failAll || (s.failAt >= 0 && s.allocNo++ == s.failAt)) return nullptr;
  s.allocs++;
  return malloc(n);
}
inline void cjFree(void *p) { if (p) cjStats().frees++; free(p); }
struct CjHooks {
  explicit CjHooks(int failAt = -1, bool failAll = false) {
    cjStats() = CjStats();
    cjStats().failAt = failAt;
    cjStats().failAll = failAll;
    cJSON_Hooks h = {cjMalloc, cjFree};
    cJSON_InitHooks(&h);
  }
  ~CjHooks() { cJSON_InitHooks(nullptr); cjStats() = CjStats(); }
  bool balanced() const { return cjStats().allocs == cjStats().frees; }
};

// Отстраняване на грешки: TEST_FILTER=<част от името> пуска само съвпадащите тестове; изходът не се буферира
// (при abort() на симулатора не се губят вече отпечатаните редове).
inline int runAllTestsFiltered() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char *f = getenv("TEST_FILTER");
  if (f && *f) {
    std::vector<TestCase> keep;
    for (auto &t : testRegistry()) if (t.name.find(f) != std::string::npos) keep.push_back(t);
    testRegistry() = keep;
  }
  return (runAllTests)();
}
#define runAllTests runAllTestsFiltered

// Тест със свежа симулация (нулира времето, NVS, периферията, задачите)
#define STEST(name) \
  static void body_##name(); \
  static void test_##name##_wrap() { sim::reset(); body_##name(); sim::reset(); } \
  static TestRegistrar reg_##name(#name, test_##name##_wrap); \
  static void body_##name()

#define ASSERT_STREQ(a, b) do { \
  g_asserts++; \
  std::string _a = (a), _b = (b); \
  if (_a != _b) { \
    g_failures++; \
    printf("  [FAIL] %s:%d: ASSERT_STREQ(%s, %s) -> got \"%s\", expected \"%s\"\n", g_currentTest, __LINE__, #a, #b, _a.c_str(), _b.c_str()); \
  } \
} while (0)

#define ASSERT_NEAR(a, b, tol) do { \
  g_asserts++; \
  double _a = (double)(a), _b = (double)(b); \
  if (std::fabs(_a - _b) > (double)(tol)) { \
    g_failures++; \
    printf("  [FAIL] %s:%d: ASSERT_NEAR(%s, %s, %s) -> got %g, expected %g\n", g_currentTest, __LINE__, #a, #b, #tol, _a, _b); \
  } \
} while (0)

#define ASSERT_GE(a, b) do { \
  g_asserts++; \
  auto _a = (a); auto _b = (b); \
  if (!(_a >= _b)) { \
    g_failures++; \
    printf("  [FAIL] %s:%d: ASSERT_GE(%s, %s) -> got %ld, expected >= %ld\n", g_currentTest, __LINE__, #a, #b, (long)_a, (long)_b); \
  } \
} while (0)

#define ASSERT_LE(a, b) do { \
  g_asserts++; \
  auto _a = (a); auto _b = (b); \
  if (!(_a <= _b)) { \
    g_failures++; \
    printf("  [FAIL] %s:%d: ASSERT_LE(%s, %s) -> got %ld, expected <= %ld\n", g_currentTest, __LINE__, #a, #b, (long)_a, (long)_b); \
  } \
} while (0)

// Изпълнява f като FreeRTOS задача (за код, който блокира: delay, опашки) и върти симулацията ms милисекунди.
inline std::function<void()> &jobSlot() { static std::function<void()> f; return f; }
inline void jobTrampoline() { jobSlot()(); }
inline void runAsTask(std::function<void()> f, uint32_t ms = 60000) {
  jobSlot() = f;
  sim::boot(jobTrampoline);
  sim::run_for_ms(ms);
}

// Суров текст на стойността на ключ от най-горното ниво на JSON обект (число/низ/булево/масив) - умишлено
// независим от cJSON и json_util (проверява ТОЧНО какъв текст е излязъл, напр. "50.00", не само числото).
inline std::string jraw(const std::string &json, const char *key) {
  size_t i = 0, n = json.size();
  int depth = 0;
  while (i < n) {
    char c = json[i];
    if (c == '"') {
      size_t st = ++i;
      while (i < n && json[i] != '"') { if (json[i] == '\\') i++; i++; }
      std::string tok = json.substr(st, i - st);
      i++;
      size_t j = i;
      while (j < n && (json[j] == ' ' || json[j] == '\t')) j++;
      if (depth == 1 && j < n && json[j] == ':' && tok == key) {
        j++;
        while (j < n && json[j] == ' ') j++;
        size_t vs = j;
        int d = 0;
        bool inStr = false;
        while (j < n) {
          char v = json[j];
          if (inStr) { if (v == '\\') j++; else if (v == '"') inStr = false; }
          else if (v == '"') inStr = true;
          else if (v == '{' || v == '[') d++;
          else if (v == '}' || v == ']') { if (d == 0) break; d--; }
          else if (v == ',' && d == 0) break;
          j++;
        }
        return json.substr(vs, j - vs);
      }
      continue;
    }
    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') depth--;
    i++;
  }
  return "<няма>";
}

// CAD заема радиото: докато трае, то не приема. Тези помощници доставят кадър в момент, в който радиото слуша, и мерят
// какъв дял от времето слуша.
inline bool deliverWhenListening(const std::vector<uint8_t> &f, long freq, uint32_t afterMs, uint32_t maxWaitMs = 500) {
  sim::run_for_ms(afterMs);
  for (uint32_t i = 0; i < maxWaitMs; i++) {
    if (sim::radio.listening()) return sim::radio.deliver(f, freq, -70, 6.25f);
    sim::run_for_ms(1);
  }
  return false;
}
inline int listeningPercent(uint32_t ms) {
  uint32_t on = 0;
  for (uint32_t i = 0; i < ms; i++) { sim::run_for_ms(1); if (sim::radio.listening()) on++; }
  return (int)(100 * on / ms);
}

inline std::string toHex(const std::vector<uint8_t> &v) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
  return s;
}
inline std::vector<uint8_t> fromHex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

static const uint8_t TEST_KEY[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                     0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

// Записва стандартна конфигурация в симулирания NVS (както би направил config стъпката)
inline void nvsRadioConfig(const char *id, uint32_t freq = 433000000, uint8_t sf = 7, uint32_t bw = 125000) {
  sim::nvs.setStr("id", id);
  sim::nvs.setBlob("key", TEST_KEY, 16);
  sim::nvs.setU32("freq", freq);
  sim::nvs.setU8("sf", sf);
  sim::nvs.setU32("bw", bw);
}

#endif
