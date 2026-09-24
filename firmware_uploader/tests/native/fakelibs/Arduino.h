// Минимален заместител на Arduino.h - само каквото реално се ползва от тестваните
// .cpp файлове (crypto_common, ceiling_counter, queues, consumers, config_storage,
// dedup, sensors_io). НЕ е пълна Arduino съвместимост - разширява се при нужда.
#ifndef FAKE_ARDUINO_H
#define FAKE_ARDUINO_H

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
using std::isnan; // Arduino.h дефинира isnan() глобално (макрос/функция); std::isnan е еквивалентно

#define F(x) x
#define PROGMEM

// ---------------- Serial (пише в конзолата на теста, за видимост при дебъг) ----------------
class FakeSerial {
public:
  void begin(long) {}
  void print(const char *s) { fputs(s, stdout); }
  void print(int v, int base = 10) { if (base == 16) printf("%x", v); else printf("%d", v); }
  void print(unsigned int v, int base = 10) { if (base == 16) printf("%x", v); else printf("%u", v); }
  void print(long v) { printf("%ld", v); }
  void print(unsigned long v) { printf("%lu", v); }
  void print(float v) { printf("%f", v); }
  void println() { printf("\n"); }
  void println(const char *s) { fputs(s, stdout); printf("\n"); }
  void println(int v, int base = 10) { print(v, base); printf("\n"); }
  void println(unsigned long v) { print(v); printf("\n"); }
  void flush() {}
};
extern FakeSerial Serial;

// ---------------- Timing / random - контролируеми от теста ----------------
extern unsigned long g_fakeMillis;
inline unsigned long millis() { return g_fakeMillis; }
inline void delay(unsigned long) {}
inline long random(long minV, long maxV) { return minV; } // детерминистично за тестове
inline long random(long maxV) { return 0; }
inline void randomSeed(unsigned long) {}

// ---------------- digital I/O - фиктивни, само за да компилира ----------------
#define OUTPUT 1
#define INPUT 0
#define HIGH 1
#define LOW 0
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return LOW; }
#define A0 100 // достатъчно голямо, за да не се сблъска с цифровите пинове 0-13

#endif
