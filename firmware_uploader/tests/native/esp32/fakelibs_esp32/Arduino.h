// Заместител на Arduino.h за ESP32 тестовете (нативно, g++). По-пълен от ../../fakelibs/Arduino.h:
// има String клас (config_esp32.ino го ползва), Serial с прихващане на изхода, симулирани пинове
// (digitalWrite/digitalRead си спомнят стойността), контролируемо време (millis/delay), ESP.restart().
// НЕ е пълна Arduino съвместимост - само каквото реално ползват ESP32_PCB(temp_hum) скечовете.
#ifndef FAKE_ARDUINO_ESP32_H
#define FAKE_ARDUINO_ESP32_H

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <type_traits>
using std::isnan;

#define F(x) x
#define RTC_DATA_ATTR           // в теста е обикновена статична променлива
typedef uint8_t byte;
typedef bool boolean;
#define HEX 16
#define DEC 10
#define PROGMEM

// ---------------- String (минимален) ----------------
class String {
public:
  String() {}
  String(const char *c) : s_(c ? c : "") {}
  String(const std::string &x) : s_(x) {}
  const char *c_str() const { return s_.c_str(); }
  unsigned int length() const { return (unsigned int)s_.size(); }
  String substring(unsigned int from) const { return String(from <= s_.size() ? s_.substr(from) : std::string()); }
  String substring(unsigned int from, unsigned int to) const {
    if (from > s_.size()) return String();
    if (to > s_.size()) to = (unsigned int)s_.size();
    if (to < from) return String();
    return String(s_.substr(from, to - from));
  }
  int indexOf(const String &x, unsigned int from = 0) const {
    size_t p = s_.find(x.s_, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  int indexOf(char c, unsigned int from = 0) const {
    size_t p = s_.find(c, from);
    return p == std::string::npos ? -1 : (int)p;
  }
  bool startsWith(const String &x) const { return s_.compare(0, x.s_.size(), x.s_) == 0; }
  void trim() {
    size_t a = 0, b = s_.size();
    while (a < b && isspace((unsigned char)s_[a])) a++;
    while (b > a && isspace((unsigned char)s_[b - 1])) b--;
    s_ = s_.substr(a, b - a);
  }
  long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
  void toCharArray(char *buf, unsigned int size, unsigned int index = 0) const {
    (void)index;
    if (!size) return;
    strncpy(buf, s_.c_str(), size - 1);
    buf[size - 1] = 0;
  }
  char operator[](unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
  bool operator==(const String &o) const { return s_ == o.s_; }
  bool operator==(const char *o) const { return s_ == (o ? o : ""); }
  bool operator!=(const String &o) const { return s_ != o.s_; }
  bool operator!=(const char *o) const { return s_ != (o ? o : ""); }
  String operator+(const String &o) const { return String(s_ + o.s_); }
  String operator+(const char *o) const { return String(s_ + (o ? o : "")); }
  std::string s_;
};
inline String operator+(const char *a, const String &b) { return String(std::string(a ? a : "") + b.s_); }

// ---------------- Serial - прихваща изхода; входът се зарежда от теста ----------------
extern std::string g_serialOut;
extern std::string g_serialIn;
extern bool g_serialEcho;
void serialEmit(const char *s);

class FakeSerial {
public:
  void begin(long) {}
  void flush() {}
  void print(const char *s) { serialEmit(s ? s : ""); }
  void print(const String &s) { serialEmit(s.c_str()); }
  void print(char c) { char b[2] = {c, 0}; serialEmit(b); }
  template <class T, typename std::enable_if<std::is_arithmetic<T>::value, int>::type = 0>
  void print(T v, int base = 10) {
    char b[48];
    if (std::is_floating_point<T>::value) snprintf(b, sizeof(b), "%.2f", (double)v);
    else if (base == 16) snprintf(b, sizeof(b), "%llx", (unsigned long long)v);
    else if (std::is_signed<T>::value) snprintf(b, sizeof(b), "%lld", (long long)v);
    else snprintf(b, sizeof(b), "%llu", (unsigned long long)v);
    serialEmit(b);
  }
  void println() { serialEmit("\n"); }
  template <class T> void println(T v) { print(v); serialEmit("\n"); }
  template <class T> void println(T v, int base) { print(v, base); serialEmit("\n"); }
  int available() { return (int)g_serialIn.size(); }
  String readStringUntil(char term) {
    size_t p = g_serialIn.find(term);
    std::string line;
    if (p == std::string::npos) { line = g_serialIn; g_serialIn.clear(); }
    else { line = g_serialIn.substr(0, p); g_serialIn.erase(0, p + 1); }
    return String(line);
  }
};
extern FakeSerial Serial;

// ---------------- Време - контролира се от теста ----------------
extern unsigned long g_fakeMillis;
inline unsigned long millis() { return g_fakeMillis; }
inline unsigned long micros() { return g_fakeMillis * 1000UL; }
inline void delay(unsigned long ms) { g_fakeMillis += ms; }   // delay() напредва времето (цикли "while(...) delay(1)")
inline long random(long minV, long maxV) { (void)maxV; return minV; }   // детерминистично
inline long random(long maxV) { (void)maxV; return 0; }
inline void randomSeed(unsigned long) {}

// ---------------- Симулирани GPIO ----------------
#define OUTPUT 1
#define INPUT 0
#define HIGH 1
#define LOW 0
extern uint8_t g_pinState[64];
extern int g_pinMode[64];
inline void pinMode(int pin, int mode) { if (pin >= 0 && pin < 64) g_pinMode[pin] = mode; }
inline void digitalWrite(int pin, int v) { if (pin >= 0 && pin < 64) g_pinState[pin] = v ? 1 : 0; }
inline int digitalRead(int pin) { return (pin >= 0 && pin < 64) ? g_pinState[pin] : 0; }

// ---------------- ESP ----------------
struct EspClass { void restart(); };
extern EspClass ESP;
extern bool g_espRestarted;

#endif
