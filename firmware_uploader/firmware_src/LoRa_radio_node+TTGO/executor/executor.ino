#include "packets.h"
#include "crypto_common.h"
#include "ceiling_counter.h"
#include "config_storage.h"
#include "consumers.h"
#include "queues.h"
#include "power_mgmt.h"
#include "cad.h"
#include "radio_io.h"
#include "debug_log.h"
#include "rng_seed.h"

void setup() {
  wdt_arm_8s_continuous();   // реален watchdog (8 s) от първия ред
  loadConfigFromEeprom();
  DBG_BEGIN(115200);
  seedRandomUnique((const uint8_t*)MY_M_ID, MODULE_ID_LEN, txCounter.peek());   // уникален за устройството (ID + nonce брояч)

  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    pinMode(consumerPins[i], OUTPUT);
    digitalWrite(consumerPins[i], LOW);
  }

  radio_setup();
  enablePinChangeWake();
  DBG(F("Executor modul готов, M_ID=")); DBG(MY_M_ID);
  DBG(F(" freq=")); DBG(LORA_FREQ_HZ);
  DBG(F(" sf=")); DBG(LORA_SF);
  DBG(F(" bw=")); DBGLN(LORA_BW_HZ);

  // Специален state response веднага след boot - маркира началото на живота на този
  // firmware инстанс (консуматорите бяха принудително изключени по-горе). Gateway трябва
  // да го потвърди с ACK; ако не отговори до 3 опита, Executor се отказва мълчаливо.
  sendRestartStateResponse();
}

void loop() {
  wdt_feed();   // "жив съм": ако цикълът се забие, следващият таймаут на watchdog-а рестартира чипа
  radioReceivePoll();
  processRx();
  ackManager();
  heartbeat_tick();
  restartRespManager();
  radioTxTick();   // неблокиращ достъп до канала + изпращане на чакащия пакет

  if (ackCount == 0 && rxCount == 0 && !heartbeat_pending() && !restart_response_pending() &&
      !radio_tx_busy()) {
    deep_sleep_until_event();
  }
}
