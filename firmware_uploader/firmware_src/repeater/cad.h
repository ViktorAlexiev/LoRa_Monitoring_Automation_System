#ifndef CAD_H
#define CAD_H

#include <Arduino.h>

// ---------------- CAD (Channel Activity Detection) - SX127x register-level ----------------
// Изисква readRegister/writeRegister да са public в LoRa.h (патчнато ръчно).
#define REG_OP_MODE       0x01
#define REG_IRQ_FLAGS     0x12
#define OP_MODE_LORA_CAD  0x87   // LongRangeMode бит + CAD mode (0b111)
#define IRQ_CAD_DONE      0x04
#define IRQ_CAD_DETECTED  0x01
#define CAD_TIMEOUT_MS    10     // CAD трае само няколко ms на SF7 - safety горна граница
                                  // (при по-висок SF трябва да се вдигне - виж README)

// true = каналът е зает точно сега (не предавай)
bool channelActive();

#endif
