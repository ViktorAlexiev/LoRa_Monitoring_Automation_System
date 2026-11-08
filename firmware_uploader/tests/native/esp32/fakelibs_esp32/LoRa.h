// Заместител на LoRa.h за интеграционните тестове: прихваща изпратените пакети (g_lora.tx) и
// "приема" пакети от опашка (g_lora.rx). readRegister/writeRegister са public (както в патчнатата
// библиотека, виж cad.h); CAD симулира зает/свободен канал през g_lora.channelBusy.
#ifndef FAKE_LORA_ESP32_H
#define FAKE_LORA_ESP32_H

#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>

struct LoRaMock {
  std::deque<std::vector<uint8_t>> rx;     // чакащи входящи пакети
  std::vector<uint8_t> cur;                // пакетът, който се чете в момента
  size_t pos = 0;
  std::vector<std::vector<uint8_t>> tx;    // всичко, изпратено през endPacket()
  std::vector<uint8_t> txbuf;
  int rssi = -50;
  float snr = 7.5f;
  bool channelBusy = false;                // какво връща CAD
  int cadCount = 0;                        // колко пъти е стартиран CAD
  long freq = 0;
  std::vector<long> txFreq;                // честотата, на която е изпратен всеки пакет
  void reset() { *this = LoRaMock(); }
};
extern LoRaMock g_lora;

class LoRaClass {
public:
  void setPins(int, int, int) {}
  int begin(long f) { g_lora.freq = f; return 1; }
  void end() {}
  int beginPacket(int = 0) { g_lora.txbuf.clear(); return 1; }
  int endPacket(bool = false) { g_lora.tx.push_back(g_lora.txbuf); g_lora.txFreq.push_back(g_lora.freq); return 1; }
  int parsePacket(int = 0) {
    if (g_lora.rx.empty()) return 0;
    g_lora.cur = g_lora.rx.front();
    g_lora.rx.pop_front();
    g_lora.pos = 0;
    return (int)g_lora.cur.size();
  }
  int packetRssi() { return g_lora.rssi; }
  float packetSnr() { return g_lora.snr; }
  size_t write(uint8_t b) { g_lora.txbuf.push_back(b); return 1; }
  size_t write(const uint8_t *p, size_t n) { g_lora.txbuf.insert(g_lora.txbuf.end(), p, p + n); return n; }
  int available() { return (int)(g_lora.cur.size() - g_lora.pos); }
  int read() { return g_lora.pos < g_lora.cur.size() ? g_lora.cur[g_lora.pos++] : -1; }
  size_t readBytes(uint8_t *buf, size_t n) {
    size_t i = 0;
    while (i < n && g_lora.pos < g_lora.cur.size()) buf[i++] = g_lora.cur[g_lora.pos++];
    return i;
  }
  void receive(int = 0) {}
  void idle() {}
  void sleep() {}
  void setTxPower(int, int = 1) {}
  void setFrequency(long f) { g_lora.freq = f; }
  void setSpreadingFactor(int) {}
  void setSignalBandwidth(long) {}
  void setCodingRate4(int) {}
  void setPreambleLength(long) {}
  void setSyncWord(int) {}
  void enableCrc() {}
  // SX127x регистри - само каквото ползва cad.cpp
  uint8_t readRegister(uint8_t reg) {
    if (reg == 0x12) return (uint8_t)(0x04 | (g_lora.channelBusy ? 0x01 : 0x00));   // CAD done (+ detected)
    return 0;
  }
  void writeRegister(uint8_t reg, uint8_t val) {
    if (reg == 0x01 && val == 0x87) g_lora.cadCount++;   // OP_MODE_LORA_CAD
  }
};
extern LoRaClass LoRa;

#endif
