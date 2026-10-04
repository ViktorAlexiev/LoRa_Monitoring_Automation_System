#ifndef SENSORS_IO_H
#define SENSORS_IO_H

#include <Arduino.h>

#define SHT31_ADDR   0x44   // почвен сензор
#define SHT21_ADDR   0x40   // въздушен сензор (raw I2C, без extra либа)

void sensors_init();

// Чете двата сензора; при грешка съответното поле остава 255.0f (не се прави
// bus-recovery/reset ескалация - виж README за причината).
void sensors_read(float &s_t, float &s_h, float &a_t, float &a_h);

#endif
