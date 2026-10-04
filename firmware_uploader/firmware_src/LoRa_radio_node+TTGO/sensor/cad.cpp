#include "cad.h"
#include <LoRa.h>
#include "radio_timing.h"

bool channelActive() {
  LoRa.writeRegister(REG_OP_MODE, OP_MODE_LORA_CAD);
  unsigned long start = millis();
  unsigned long timeoutMs = radioCadTimeoutMs();
  while (!(LoRa.readRegister(REG_IRQ_FLAGS) & IRQ_CAD_DONE)) {
    if (millis() - start > timeoutMs) break;
  }
  bool detected = LoRa.readRegister(REG_IRQ_FLAGS) & IRQ_CAD_DETECTED;
  LoRa.writeRegister(REG_IRQ_FLAGS, 0xFF);   // clear IRQ флаговете
  return detected;
}

void radioSetLowDataRateOptimize(bool on) {
  uint8_t c3 = LoRa.readRegister(REG_MODEM_CONFIG_3);
  if (on) c3 |= MODEM_CONFIG_3_LDRO; else c3 &= (uint8_t)~MODEM_CONFIG_3_LDRO;
  LoRa.writeRegister(REG_MODEM_CONFIG_3, c3);
}

void radioApplyModemSettings() {
  LoRa.setSpreadingFactor(radioSf());
  LoRa.setSignalBandwidth((long)radioBw());
  LoRa.setCodingRate4(radioCrDenom());
  LoRa.setPreambleLength(radioPreambleLen());
  // ПОСЛЕДНО и изрично: setSpreadingFactor/setSignalBandwidth на библиотеката смятат LDRO
  // с целочислено деление и на SF11/BW125 (Ts=16.384 ms) я оставят изключена.
  radioSetLowDataRateOptimize(radioLdroNeeded());
}
