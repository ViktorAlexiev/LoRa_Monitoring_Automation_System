#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H

#include <stdint.h>

#pragma pack(push, 1)
// Вътрешно представяне (опашки между задачите) - НЕ е wire форматът. M_ID не е нужен: декриптиран downlink
// пакет вече е потвърден "за мен" (чиста проверка на target преди decrypt).
struct CommandPacket {
  char C_ID[4];
  uint8_t com;
  uint8_t status;
};
#pragma pack(pop)

// Заявка към радио задачата: потвърди тази команда, не по-рано от sendAt (пауза, докато gateway-ят излезе от TX)
struct AckReq {
  CommandPacket pkt;
  uint32_t sendAt;
};

#endif
