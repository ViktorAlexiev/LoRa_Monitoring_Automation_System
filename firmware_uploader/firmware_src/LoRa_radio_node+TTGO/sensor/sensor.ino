#include "crypto_common.h"
#include "ceiling_counter.h"
#include "config_storage.h"
#include "sensors_io.h"
#include "power_mgmt.h"
#include "radio_tx.h"
#include "debug_log.h"
#include "rng_seed.h"

void setup() {
  wdt_start();                        // реален watchdog (8 s) от първия ред
  loadConfigFromEeprom();
  DBG_BEGIN(115200);
  seedRandomUnique((const uint8_t*)SENSOR_ID, MODULE_ID_LEN, txCounter.peek());   // уникален за устройството (ID + nonce брояч)
  DBG(F("Sensor node ready (SHT31+SHT21), freq="));
  DBG(LORA_FREQ_HZ);
  DBG(F(" sf=")); DBG(LORA_SF);
  DBG(F(" bw=")); DBGLN(LORA_BW_HZ);
  DBG_FLUSH();
}

void loop() {
  // Jitter ПРЕДИ радиото да се събуди (то е в sleep от предишния цикъл): разминава TX-а с други sensor-и, без да тегли
  // ток на standby (~1.6 mA) по време на изчакването.
  delay(random(0, JITTER_MAX_MS));
  wdt_feed();

  float s_t, s_h, a_t, a_h;
  sensors_begin();
  sensors_read(s_t, s_h, a_t, a_h);
  sensors_end();                      // TWI изключен (и в PRR при съня)
  wdt_feed();

  if (lora_init()) {
    send_sensor_packet(s_t, s_h, a_t, a_h);
  }
  // при мъртво радио (lora_init() == false) цикълът се пропуска: сензорът заспива и опитва пак след ~15 минути

  deep_sleep(WDT_CYCLES_BETWEEN_SEND);
}
