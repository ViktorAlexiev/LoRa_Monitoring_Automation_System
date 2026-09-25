// Заместител на LoRa.h - САМО за проверка на синтаксис/типове (-fsyntax-only) на радио-ниво
// файловете, които не могат да се изпълняват нативно. readRegister/writeRegister са public,
// както изисква патчнатата библиотека (виж cad.h).
#ifndef FAKE_LORA_H
#define FAKE_LORA_H
#include <cstdint>
#include <cstddef>
class LoRaClass {
public:
  void setPins(int, int, int) {}
  int  begin(long) { return 1; }
  void end() {}
  int  beginPacket(int = 0) { return 1; }
  int  endPacket(bool = false) { return 1; }
  int  parsePacket(int = 0) { return 0; }
  int  packetRssi() { return 0; }
  float packetSnr() { return 0; }
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t*, size_t n) { return n; }
  int  available() { return 0; }
  int  read() { return -1; }
  size_t readBytes(uint8_t*, size_t n) { return n; }
  void receive(int = 0) {}
  void idle() {}
  void sleep() {}
  void setTxPower(int, int = 1) {}
  void setFrequency(long) {}
  void setSpreadingFactor(int) {}
  void setSignalBandwidth(long) {}
  void setCodingRate4(int) {}
  void setPreambleLength(long) {}
  void setSyncWord(int) {}
  void enableCrc() {}
  uint8_t readRegister(uint8_t) { return 0; }
  void writeRegister(uint8_t, uint8_t) {}
};
extern LoRaClass LoRa;
#endif
