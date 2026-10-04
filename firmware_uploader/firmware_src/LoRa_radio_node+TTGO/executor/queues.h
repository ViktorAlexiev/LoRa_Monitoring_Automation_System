#ifndef QUEUES_H
#define QUEUES_H

#include <Arduino.h>
#include "packets.h"

#define RXBUFFSIZE 8
#define ACKBUFFSIZE 8

extern uint8_t rxCount;
extern uint8_t ackCount;

bool rxPush(CommandPacket p);
bool rxPop(CommandPacket *out);

bool ackEnqueue(CommandPacket p, unsigned long sendAt);
// Достъп до опашката за ackManager() - декларирани extern, за да не дублираме push/pop API
// само за едно място на употреба.
extern CommandPacket ackQueue[ACKBUFFSIZE];
extern unsigned long ackSendAt[ACKBUFFSIZE];
void ackRemoveAt(uint8_t index);

#endif
