#include "cad.h"
#include <LoRa.h>

bool channelActive() {
  LoRa.writeRegister(REG_OP_MODE, OP_MODE_LORA_CAD);
  unsigned long start = millis();
  while (!(LoRa.readRegister(REG_IRQ_FLAGS) & IRQ_CAD_DONE)) {
    if (millis() - start > CAD_TIMEOUT_MS) break;
  }
  bool detected = LoRa.readRegister(REG_IRQ_FLAGS) & IRQ_CAD_DETECTED;
  LoRa.writeRegister(REG_IRQ_FLAGS, 0xFF);   // clear IRQ флаговете
  return detected;
}
