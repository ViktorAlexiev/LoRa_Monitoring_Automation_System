#ifndef PACKETS_H
#define PACKETS_H

#pragma pack(push, 1)
// Вътрешно RAM представяне (rxBuffer/ackQueue) - НЕ е директно wire формата.
// M_ID не е нужен тук - decrypt-нат downlink пакет вече е потвърден "за мен"
// (cleartext target проверка) преди да стигне тук, implicit adresat = MY_M_ID.
struct CommandPacket {
  char    C_ID[4];
  uint8_t com;
  uint8_t status;
};
#pragma pack(pop)

#endif
