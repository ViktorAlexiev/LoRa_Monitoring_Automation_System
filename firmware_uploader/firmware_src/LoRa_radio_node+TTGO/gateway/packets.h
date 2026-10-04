#ifndef PACKETS_H
#define PACKETS_H

// SensorPacket / HeartbeatPacket(repeater) / HeartbeatExecPacket / state request/response
// вече не се пращат като сурови struct-ове - всички са криптирани (виж crypto_common.h),
// gateway.ino/lora_handlers.cpp ги обработва директно от raw байтовете.
#pragma pack(push, 1)
struct CommandPacket {
  char    M_ID[6];
  char    C_ID[4];
  uint8_t com;
  uint8_t status;
};
#pragma pack(pop)

#endif
