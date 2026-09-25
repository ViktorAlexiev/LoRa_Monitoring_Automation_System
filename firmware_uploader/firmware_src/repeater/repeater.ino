#include <LoRa.h>
#include "crypto_common.h"
#include "ceiling_counter.h"
#include "config_storage.h"
#include "cad.h"
#include "dedup.h"
#include "power_mgmt.h"
#include "radio_io.h"

void setup() {
  Serial.begin(115200);   // преди липсваше - сериен дебъг на Repeater беше невъзможен
  loadConfigFromEeprom();
  randomSeed(micros());   // seed за jitter/backoff

  radio_setup();
  enablePinChangeWake();
  wdt_arm_8s_continuous();

  Serial.print(F("Repeater modul готов, M_ID=")); Serial.print(REPEATER_ID);
  Serial.print(F(" RX_freq=")); Serial.print(LORA_FREQ_RX_HZ);
  Serial.print(F(" TX_freq=")); Serial.print(LORA_FREQ_TX_HZ);
  Serial.print(F(" sf=")); Serial.print(LORA_SF);
  Serial.print(F(" bw=")); Serial.println(LORA_BW_HZ);
  Serial.flush();
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) receive_and_queue(packetSize);

  process_pending_forwards();
  heartbeat_tick();

  // Дълбок сън само ако няма нищо, чакащо конкретен millis() момент - иначе оставаме
  // будни (Timer0 работи нормално), за да следим backoff/jitter timing-а точно.
  if (!any_forward_pending() && !heartbeat_pending()) {
    deep_sleep_until_event();
  }
}
