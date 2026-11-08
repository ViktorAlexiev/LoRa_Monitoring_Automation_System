#include "cad.h"
#include <LoRa.h>
#include "radio_timing.h"
#include "power_mgmt.h"

bool radioRxPending() {
  return (LoRa.readRegister(REG_IRQ_FLAGS) & IRQ_RX_DONE) != 0;
}

bool channelActive() {
  // Непрочетен пакет: CAD изкарва радиото от приемане, а чистенето на флаговете би го изтрило. Каналът е очевидно зает.
  if (radioRxPending()) return true;

  LoRa.writeRegister(REG_OP_MODE, OP_MODE_LORA_CAD);
  uint32_t start = millis();
  uint32_t timeoutMs = radioCadTimeoutMs();
  while (!(LoRa.readRegister(REG_IRQ_FLAGS) & IRQ_CAD_DONE)) {
    if (millis() - start > timeoutMs) break;
  }
  bool detected = (LoRa.readRegister(REG_IRQ_FLAGS) & IRQ_CAD_DETECTED) != 0;
  LoRa.writeRegister(REG_IRQ_FLAGS, IRQ_CAD_DONE | IRQ_CAD_DETECTED);   // чистим само флаговете на CAD
  return detected;
}

bool loraTransmit(const uint8_t *buf, uint8_t len) {
  if (!LoRa.beginPacket()) return false;      // 0 = радиото още предава
  LoRa.write(buf, len);
  LoRa.endPacket(true);                        // async: връща веднага след старта на предаването

  uint32_t toaMs = radioToaMs(len);
  uint32_t deadlineMs = toaMs + toaMs / 4 + 100;
  uint32_t start = millis();
  bool ok = false;
  while (millis() - start <= deadlineMs) {
    if (LoRa.readRegister(REG_IRQ_FLAGS) & IRQ_TX_DONE) { ok = true; break; }
    delay(1);
    wdt_feed();           // предаването може да трае секунди (SF12): без feed watchdog-ът би рестартирал чипа
  }
  LoRa.writeRegister(REG_IRQ_FLAGS, IRQ_TX_DONE);
  if (!ok) LoRa.idle();                        // прекъсва зациклило предаване
  return ok;
}

void radioSetLowDataRateOptimize(bool on) {
  uint8_t c3 = LoRa.readRegister(REG_MODEM_CONFIG_3);
  if (on) c3 |= MODEM_CONFIG_3_LDRO; else c3 &= (uint8_t)~MODEM_CONFIG_3_LDRO;
  LoRa.writeRegister(REG_MODEM_CONFIG_3, c3);
}

void radioApplyModemSettings() {
  LoRa.setSpreadingFactor(radioSf());
  LoRa.setSignalBandwidth((int32_t)radioBw());
  LoRa.setCodingRate4(radioCrDenom());
  LoRa.setPreambleLength(radioPreambleLen());
  // ПОСЛЕДНО и изрично: setSpreadingFactor/setSignalBandwidth на библиотеката смятат LDRO
  // с целочислено деление и на SF11/BW125 (Ts=16.384 ms) я оставят изключена.
  radioSetLowDataRateOptimize(radioLdroNeeded());
}
