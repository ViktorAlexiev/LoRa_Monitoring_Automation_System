#include "crypto_common.h"
#include "ceiling_counter.h"
#include "config_storage.h"
#include "sensors_io.h"
#include "power_mgmt.h"
#include "radio_tx.h"

void setup() {
  loadConfigFromEeprom();
  Serial.begin(115200);
  randomSeed(micros());   // seed без зависимост от пин
  sensors_init();
  Serial.print(F("Sensor node ready (SHT31+SHT21), freq="));
  Serial.print(LORA_FREQ_HZ);
  Serial.print(F(" sf=")); Serial.print(LORA_SF);
  Serial.print(F(" bw=")); Serial.println(LORA_BW_HZ);
  Serial.flush();
}

void loop() {
  float s_t, s_h, a_t, a_h;
  sensors_read(s_t, s_h, a_t, a_h);

  lora_init();
  send_sensor_packet(s_t, s_h, a_t, a_h);

  deep_sleep(WDT_CYCLES_BETWEEN_SEND);
}
