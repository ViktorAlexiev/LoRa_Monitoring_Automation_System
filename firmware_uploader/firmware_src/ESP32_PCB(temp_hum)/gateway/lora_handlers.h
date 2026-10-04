#ifndef LORA_HANDLERS_H
#define LORA_HANDLERS_H

#include <Arduino.h>

// ---------- LoRa пинове (виж board_pins.h) ----------
#include "board_pins.h"

// ---------- LoRa радио параметри ----------
// SF и BW НЕ са константи - идват от NVS (LORA_SF/LORA_BW_HZ в config_storage.h)
#define LORA_CR_DENOM       5
#define LORA_TX_POWER_DBM   17
#define LORA_PREAMBLE_LEN   8
#define LORA_SYNC_WORD      0x12

// ---------- Команден протокол ----------
#define CMD_ON  0xA1
#define CMD_OFF 0xB2

#define STATUS_ACK     0
#define STATUS_TIMEOUT 1
#define STATUS_NACK    2

// ACK timeout-ите не са константи - изчисляват се от SF/BW (radioAckTimeoutCmdMs() и др. в radio_timing.h)
#define MAX_RETRIES     2

#define LORA_BEGIN_RETRY_MSG_MS  10000UL   // интервал между диагностични съобщения при неуспешен LoRa.begin()

void lora_radio_setup();   // SPI.begin + LoRa.setPins/begin/params

// MQTT->LoRa изходящи заявки (държат "в полет" състоянието вътрешно)
void lora_send_command(const char* m_id, const char* c_id, uint8_t com);
void lora_send_state_request(const char* m_id);

// Обработка на входящ LoRa пакет + периодични ACK/retry мениджъри
void lora_handle_incoming(int packetSize);
void lora_managers_tick();

#endif
