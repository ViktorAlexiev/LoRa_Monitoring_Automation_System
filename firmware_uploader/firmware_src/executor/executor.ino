#include "packets.h"
#include "crypto_common.h"
#include "ceiling_counter.h"
#include "config_storage.h"
#include "consumers.h"
#include "queues.h"
#include "power_mgmt.h"
#include "cad.h"
#include "radio_io.h"

void setup() {
  loadConfigFromEeprom();
  Serial.begin(115200);
  randomSeed(micros());   // seed без зависимост от пин - консуматорски пинове може да заемат всеки аналогов

  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    pinMode(consumerPins[i], OUTPUT);
    digitalWrite(consumerPins[i], LOW);
  }

  radio_setup();
  enablePinChangeWake();
  wdt_arm_8s_continuous();
  Serial.print(F("Executor modul готов, M_ID=")); Serial.print(MY_M_ID);
  Serial.print(F(" freq=")); Serial.print(LORA_FREQ_HZ);
  Serial.print(F(" sf=")); Serial.print(LORA_SF);
  Serial.print(F(" bw=")); Serial.println(LORA_BW_HZ);

  // Специален state response веднага след boot - маркира началото на живота на този
  // firmware инстанс (консуматорите бяха принудително изключени по-горе). Gateway трябва
  // да го потвърди с ACK; ако не отговори до 3 опита, Executor се отказва мълчаливо.
  sendRestartStateResponse();
}

void loop() {
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
