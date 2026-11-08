#ifndef SENSOR_TX_H
#define SENSOR_TX_H

#include "sensor_config.h"
#include "sensors_io.h"

// Стартира радиото на честотата от NVS. false = чипът не отговори (не се зацикля: батерия).
bool sensor_radio_init(const SensorConfig *c);

// Строи криптирания пакет (S_ID в чисто, 4-те стойности като int16 x100 криптирани = 22 B), праща го по LoRa
// (jitter + listen-before-talk), после приспива радиото. false = нищо не е излъчено (няма ключ/id, провал на
// NVS ceiling или TX). Броячът на nonce е в RTC паметта и оцелява при дълбок сън.
bool sensor_send_packet(const SensorConfig *c, const SensorSample *s);

// Номерът на последния излъчен пакет е в ceiling_counter; за тестове:
void sensor_tx_reset(void);

#endif
