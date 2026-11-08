#ifndef GW_TYPES_H
#define GW_TYPES_H

#include <stdint.h>
#include "protocol.h"

// ---- съобщения към задачата gw_cmd (радио -> логика, MQTT -> логика) ----
#define GW_MQTT_PAYLOAD_MAX  159          // команда/заявка са < 100 B

enum GwMsgType {
  GM_SENSOR = 1,        // декриптиран sensor пакет
  GM_HB_REPEATER,
  GM_HB_EXEC,
  GM_ACK,               // ACK/NACK от executor
  GM_STATE,             // state response (нормален или restart)
  GM_MQTT_CMD,          // команда от бекенда (тема "commands")
  GM_MQTT_STATE_REQ     // заявка за състояния (тема "module_states_requests")
};

struct GwMsg {
  uint8_t type;
  uint8_t restart;                       // GM_STATE: автоматичен отговор след boot
  uint8_t stateLen;                      // GM_STATE: байтове в state[]
  uint8_t com, status;                   // GM_ACK
  uint16_t payloadLen;                   // GM_MQTT_*
  int16_t rssi;
  int16_t snrX100;                       // SNR x 100 (стъпка 0.25 dB)
  uint32_t tsSec;                        // секунди от старта, към момента на приемане
  char id[CRYPTO_ID_LEN + 1];            // подател (sensor / repeater / executor)
  char cId[CONSUMER_ID_LEN + 1];         // GM_ACK
  int16_t v[4];                          // GM_SENSOR: сурови стойности x100 (SENSOR_RAW_NODATA = няма данни)
  int16_t batMv;                         // GM_SENSOR/GM_HB_*: батерия в mV; BATTERY_RAW_NODATA = пакетът е без батерия/няма измерване
  uint8_t state[CRYPTO_MAX_PT];          // GM_STATE
  char payload[GW_MQTT_PAYLOAD_MAX + 1]; // GM_MQTT_*
};

// ---- заявка за предаване (логика -> радио) ----
struct TxReq {
  uint8_t len;
  uint8_t buf[CMD_WIRE_LEN];             // най-дългият изходящ пакет (26 B)
};

#endif
