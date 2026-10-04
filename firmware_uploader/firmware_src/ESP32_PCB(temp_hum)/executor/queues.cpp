#include "queues.h"

static CommandPacket rxBuffer[RXBUFFSIZE];
static uint8_t rxHead = 0, rxTail = 0;
uint8_t rxCount = 0;

bool rxPush(CommandPacket p) {
  if (rxCount >= RXBUFFSIZE) { Serial.println("[ERR] RX buffer full"); return false; }
  rxBuffer[rxTail] = p;
  rxTail = (rxTail + 1) % RXBUFFSIZE;
  rxCount++;
  return true;
}
bool rxPop(CommandPacket *out) {
  if (rxCount == 0) return false;
  *out = rxBuffer[rxHead];
  rxHead = (rxHead + 1) % RXBUFFSIZE;
  rxCount--;
  return true;
}

CommandPacket ackQueue[ACKBUFFSIZE];
unsigned long ackSendAt[ACKBUFFSIZE] = {0};
uint8_t ackCount = 0;

bool ackEnqueue(CommandPacket p, unsigned long sendAt) {
  if (ackCount >= ACKBUFFSIZE) { Serial.println("[ERR] ACK queue full"); return false; }
  ackQueue[ackCount]  = p;
  ackSendAt[ackCount] = sendAt;
  ackCount++;
  return true;
}

void ackRemoveAt(uint8_t index) {
  for (uint8_t j = index; j < ackCount - 1; j++) {
    ackQueue[j]  = ackQueue[j + 1];
    ackSendAt[j] = ackSendAt[j + 1];
  }
  ackCount--;
}
