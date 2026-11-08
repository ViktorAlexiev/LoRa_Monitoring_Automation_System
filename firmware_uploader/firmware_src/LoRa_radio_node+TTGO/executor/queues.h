#ifndef QUEUES_H
#define QUEUES_H

#include <Arduino.h>
#include "packets.h"

// Gateway държи ЕДНА команда в полет наведнъж, така че 6 места са с голям запас (преди 8: ~32 B RAM повече).
#define RXBUFFSIZE 6
#define ACKBUFFSIZE 6

extern uint8_t rxCount;
extern uint8_t ackCount;

bool rxPush(const CommandPacket &p);
bool rxPop(CommandPacket *out);

bool ackEnqueue(const CommandPacket &p, uint32_t sendAt);
// Достъп до опашката за ackManager() - декларирани extern, за да не дублираме push/pop API
// само за едно място на употреба.
extern CommandPacket ackQueue[ACKBUFFSIZE];
extern uint32_t ackSendAt[ACKBUFFSIZE];
void ackRemoveAt(uint8_t index);

#endif
