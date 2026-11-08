#include <LoRa.h>
#include "crypto_common.h"
#include "ceiling_counter.h"
#include "config_storage.h"
#include "cad.h"
#include "dedup.h"
#include "power_mgmt.h"
#include "radio_io.h"
#include "debug_log.h"
#include "rng_seed.h"

void setup() {
  wdt_arm_8s_continuous();   // реален watchdog (8 s) от първия ред
  DBG_BEGIN(115200);
  loadConfigFromEeprom();
  seedRandomUnique((const uint8_t*)REPEATER_ID, MODULE_ID_LEN, hbCounter.peek());   // уникален за устройството (ID + nonce брояч)

  radio_setup();
  enablePinChangeWake();

  DBG(F("Repeater modul готов, M_ID=")); DBG(REPEATER_ID);
  DBG(F(" RX_freq=")); DBG(LORA_FREQ_RX_HZ);
  DBG(F(" TX_freq=")); DBG(LORA_FREQ_TX_HZ);
  DBG(F(" sf=")); DBG(LORA_SF);
  DBG(F(" bw=")); DBGLN(LORA_BW_HZ);
  DBG_FLUSH();
}

void loop() {
  wdt_feed();   // "жив съм": ако цикълът се забие, следващият таймаут на watchdog-а рестартира чипа
  uint8_t packetSize = (uint8_t)LoRa.parsePacket();
  if (packetSize) receive_and_queue(packetSize);

  process_pending_forwards();
  heartbeat_tick();

  // Дълбок сън само ако няма нищо, чакащо конкретен millis() момент - иначе оставаме
  // будни (Timer0 работи нормално), за да следим backoff/jitter timing-а точно.
  if (!any_forward_pending() && !heartbeat_pending()) {
    deep_sleep_until_event();
  }
}
