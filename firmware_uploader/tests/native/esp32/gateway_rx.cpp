// Драйвер за e2e теста: пуска РЕАЛНИЯ gateway.ino (setup/loop), lora_handlers.cpp, wifi_mqtt.cpp и
// config_storage.cpp с фалшиви LoRa/WiFi/PubSubClient/NVS и РЕАЛНАТА ArduinoJson. Python тестът
// подава команди през stdin и чете резултата (публикувани MQTT съобщения, изпратени LoRa пакети).
//
//   gateway_rx <GW_ID> <KEY_HEX32> <FREQ_HZ> <SF> <BW_HZ>
//   RX <hex>                              входящ LoRa пакет (напр. от sensor_capture)
//   MKACK <M_ID> <C_ID> <comHex> <status> <ctr>      ACK/NACK от executor (build с реалния crypto)
//   MKSTATE <M_ID> <restart:0|1> <ctr> [C_ID:0|1 ...] state response от executor
//   MKHB <M_ID> <EXEC|REPEATER> <ctr>     heartbeat
//   MQTT <topic> <json>                   съобщение от брокера (както би го доставил PubSubClient)
//   TICK <ms>                             напредва времето и пуска loop()
//   RUN <n> <step_ms>                     n пъти: време += step; loop()
//   BUSY <0|1>                            зает/свободен канал (CAD)
//   SETFAIL <mqtt_publish_ok:0|1>         успех на MQTT publish
// Всяка команда печата ">> <команда>", после редове:
//   PUB @<ms> <topic> <retained> <payload>   публикувано в MQTT (с момента на симулираното време)
//   TX @<ms> <hex>                        изпратен LoRa пакет
//   TXDEC <CMD|STATE_REQ|STATE_RESP_ACK> target=<id> sender=<id> pt=<hex>   разшифровка на TX
#include "esp32_test_helpers.h"
#include <iostream>
#include <sstream>

#include "gateway/gateway.ino"   // -I <папката с firmware> (виж run_esp32.sh)

static std::string hex(const uint8_t *b, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[b[i] >> 4]; s += d[b[i] & 15]; }
  return s;
}
static std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}
static void pad(uint8_t *dst, const std::string &s, size_t n) {
  memset(dst, 0, n);
  memcpy(dst, s.c_str(), s.size() < n ? s.size() : n);
}
static std::string idStr(const uint8_t *b, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && b[i]; i++) s += (char)b[i];
  return s;
}

static size_t txPrinted = 0;
static size_t pubPrinted = 0;

static void flushOutputs() {
  for (; pubPrinted < g_mqtt.published.size(); pubPrinted++) {
    MqttPublished &p = g_mqtt.published[pubPrinted];
    printf("PUB @%lu %s %d %s\n", g_fakeMillis, p.topic.c_str(), p.retained ? 1 : 0, p.payload.c_str());
  }
  for (; txPrinted < g_lora.tx.size(); txPrinted++) {
    std::vector<uint8_t> &f = g_lora.tx[txPrinted];
    printf("TX @%lu %s\n", g_fakeMillis, hex(f.data(), f.size()).c_str());
    if (f.size() > 6) {
      const uint8_t *wire = f.data() + 6;
      uint8_t wlen = (uint8_t)(f.size() - 6);
      struct { const char *name; uint8_t type; } types[3] = {
        {"CMD", CRYPTO_TYPE_GW_CMD}, {"STATE_REQ", CRYPTO_TYPE_STATE_REQ}, {"STATE_RESP_ACK", CRYPTO_TYPE_STATE_RESP_ACK}};
      for (auto &t : types) {
        uint8_t sender[CRYPTO_ID_LEN], pt[CRYPTO_MAX_PT], ptLen = 0;
        if (cryptoParseWirePacket(wire, wlen, NETWORK_KEY, t.type, sender, pt, &ptLen)) {
          printf("TXDEC %s target=%s sender=%s pt=%s\n", t.name, idStr(f.data(), 6).c_str(),
                 idStr(sender, CRYPTO_ID_LEN).c_str(), hex(pt, ptLen).c_str());
          break;
        }
      }
    }
  }
  fflush(stdout);
}

static void drain(int maxLoops = 20) {
  for (int i = 0; i < maxLoops; i++) {
    bool more = !g_lora.rx.empty();
    g_fakeMillis += 1;
    loop();
    if (!more) break;
  }
}

int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "употреба: gateway_rx GW_ID KEY_HEX FREQ SF BW\n"); return 2; }
  resetFakes();
  std::vector<uint8_t> key = unhex(argv[2]);
  nvsSetStr("id", argv[1]);
  nvsSetBytes("key", key.data(), key.size());
  nvsSetU32("freq", (uint32_t)strtoul(argv[3], nullptr, 10));
  nvsSetU8("sf", (uint8_t)atoi(argv[4]));
  nvsSetU32("bw", (uint32_t)strtoul(argv[5], nullptr, 10));
  nvsSetStr("wssid", "FarmNet");
  nvsSetStr("wpass", "pw");
  nvsSetStr("mqip", "192.168.1.10");
  nvsSetStr("mqport", "1883");
  g_fakeMillis = 10000;
  setup();
  g_fakeMillis += 6000;   // над RECONNECT_ATTEMPT_MS -> свързва се с MQTT
  loop();
  printf("READY subs=%d\n", (int)g_mqtt.subs.size());
  for (auto &s : g_mqtt.subs) printf("SUB %s %d\n", s.topic.c_str(), s.qos);
  flushOutputs();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    printf(">> %s\n", line.c_str());
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "RX") {
      std::string h; in >> h;
      g_lora.rx.push_back(unhex(h));
      drain();
    } else if (cmd == "MKACK") {
      std::string m, c, com; int status; unsigned ctr;
      in >> m >> c >> com >> status >> ctr;
      uint8_t sender[6], pt[6], wire[CRYPTO_OVERHEAD + 6];
      pad(sender, m, 6);
      pad(pt, c, 4);
      pt[4] = (uint8_t)strtol(com.c_str(), nullptr, 16);
      pt[5] = (uint8_t)status;
      uint8_t n = cryptoBuildWirePacket(wire, NETWORK_KEY, CRYPTO_TYPE_EXEC_ACK, sender, ctr, pt, 6);
      g_lora.rx.push_back(std::vector<uint8_t>(wire, wire + n));
      drain();
    } else if (cmd == "MKSTATE") {
      std::string m; int restart; unsigned ctr;
      in >> m >> restart >> ctr;
      std::vector<uint8_t> pt;
      std::string ent;
      while (in >> ent) {
        size_t colon = ent.find(':');
        uint8_t c4[4];
        pad(c4, ent.substr(0, colon), 4);
        pt.insert(pt.end(), c4, c4 + 4);
        pt.push_back((uint8_t)atoi(ent.substr(colon + 1).c_str()));
      }
      uint8_t sender[6];
      pad(sender, m, 6);
      uint8_t wire[CRYPTO_OVERHEAD + CRYPTO_MAX_PT];
      uint8_t n = cryptoBuildWirePacket(wire, NETWORK_KEY, restart ? CRYPTO_TYPE_STATE_RESP_RESTART : CRYPTO_TYPE_STATE_RESP,
                                        sender, ctr, pt.data(), (uint8_t)pt.size());
      g_lora.rx.push_back(std::vector<uint8_t>(wire, wire + n));
      drain();
    } else if (cmd == "MKHB") {
      std::string m, kind; unsigned ctr;
      in >> m >> kind >> ctr;
      uint8_t sender[6], wire[CRYPTO_OVERHEAD];
      pad(sender, m, 6);
      uint8_t n = cryptoBuildWirePacket(wire, NETWORK_KEY, kind == "REPEATER" ? CRYPTO_TYPE_REPEATER_HB : CRYPTO_TYPE_EXEC_HB,
                                        sender, ctr, NULL, 0);
      g_lora.rx.push_back(std::vector<uint8_t>(wire, wire + n));
      drain();
    } else if (cmd == "MQTT") {
      std::string topic; in >> topic;
      std::string payload;
      std::getline(in, payload);
      if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
      mqttInject(topic, payload);
      drain(3);
    } else if (cmd == "TICK") {
      unsigned long ms; in >> ms;
      g_fakeMillis += ms;
      loop();
    } else if (cmd == "RUN") {
      int n; unsigned long step; in >> n >> step;
      for (int i = 0; i < n; i++) { g_fakeMillis += step; loop(); flushOutputs(); }
    } else if (cmd == "BUSY") {
      int b; in >> b;
      g_lora.channelBusy = b != 0;
    } else if (cmd == "SETFAIL") {
      int ok; in >> ok;
      g_mqtt.publishResult = ok != 0;
    } else if (cmd == "MQTTDOWN") {
      g_mqtt.connected = false;
    }
    flushOutputs();
  }
  return 0;
}
