#ifndef SENSORS_IO_H
#define SENSORS_IO_H

#include <stdint.h>
#include <stdbool.h>

// Два SHT31 на ДВЕ различни I2C шини (нов I2C драйвер на IDF: driver/i2c_master.h). Пиновете и адресите
// са в board_pins.h. Бъдещ мултиплексор: сменя се само таблицата в sensors_io.cpp.
#define SENSOR_CH_SOIL   0     // почвен сензор  -> S_T, S_H
#define SENSOR_CH_AIR    1     // въздушен сензор -> A_T, A_H
#define SENSOR_CHANNELS  2

// Стойност в стотни (22.57 °C -> 2257); SENSOR_RAW_NODATA (-32768) = няма данни
struct SensorSample {
  int16_t s_t, s_h, a_t, a_h;   // почва: температура, влажност; въздух: температура, влажност
  int16_t bat_mv;               // батерия в mV (само при опция "има батерия"); BATTERY_RAW_NODATA = няма измерване
};

// Вдига всяка от двете I2C шини (самостоятелно: неуспех на едната не пречи на другата) и рестартира SHT31.
// true = поне един канал е готов. При false sensors_read връща "няма данни" за всичко, но пакетът пак се праща
// (бекендът вижда живо устройство).
bool sensors_init(void);

// Чете двата сензора. Грешка/извън диапазон/лоша CRC на конкретен сензор -> SENSOR_RAW_NODATA за неговите две
// стойности; другият сензор не се засяга. Не се прави bus-recovery/ескалация (виж README).
void sensors_read(SensorSample *out);

// Освобождава устройствата и шините (преди дълбокия сън / за повторно стартиране).
void sensors_deinit(void);

// Чиста функция (тества се без шина): CRC на двете двойки + физически обхват на SHT31
// (температура -40..125 °C с 1 °C запас, влажност 0..100 %; извън него стойността е боклук, не измерване).
bool sht31_decode(const uint8_t b[6], float *tempC, float *humPct);

#endif
