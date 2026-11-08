#include "queues.h"
#include "debug_log.h"

static CommandPacket rxBuffer[RXBUFFSIZE];
static uint8_t rxHead = 0;
static uint8_t rxTail = 0;
uint8_t rxCount = 0;

bool rxPush(const CommandPacket &p) {
  if (rxCount >= RXBUFFSIZE) { DBGLN(F("[ERR] RX buffer full")); return false; }
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
uint32_t ackSendAt[ACKBUFFSIZE] = {0};
uint8_t ackCount = 0;

bool ackEnqueue(const CommandPacket &p, uint32_t sendAt) {
  if (ackCount >= ACKBUFFSIZE) { DBGLN(F("[ERR] ACK queue full")); return false; }
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
