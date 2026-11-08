/*
  sensor (ESP-IDF) - измерва два SHT31 (почва и въздух, на две I2C шини), праща 22-байтов криптиран пакет по LoRa и
  заспива в ДЪЛБОК СЪН (~15 минути). При събуждане чипът стартира от app_main() наново.

  Една линейна последователност - няма задачи: след дълбокия сън чипът се рестартира всеки цикъл и
  няма какво да се върши паралелно.
*/
#include "esp_log.h"
#include <stdint.h>
#include <inttypes.h>
#include "agro_hal.h"
#include "board_power.h"
#include "power.h"
#include "sensor_config.h"
#include "sensor_tx.h"
#include "sensors_io.h"

static const char *TAG = "sensor";

// ~15 минути между измерванията: 113 x 8 s = 904 s (същата стойност като в Arduino версията)
// Колко се изчаква след включване на интерфейсите, преди да се мери: сензорите се събуждат и стабилизират
#define SENSOR_POWER_SETTLE_MS  1000

#define SLEEP_CYCLES  113
#define SLEEP_US      ((uint64_t)SLEEP_CYCLES * UINT64_C(8) * UINT64_C(1000000))

extern "C" void app_main(void) {
  sensor_tx_reset();
  hal_nvs_init();

  SensorConfig cfg;
  sensor_config_load(&cfg);
  ESP_LOGI(TAG, "Sensor node (2x SHT31) id=%s freq=%" PRIu32 " sf=%" PRIu32 " bw=%" PRIu32, cfg.id, (uint32_t)cfg.freqHz,
           (uint32_t)cfg.sf, (uint32_t)cfg.bwHz);

  board_power_init();                     // интерфейсите и делителят са ИЗКЛЮЧЕНИ (и след дълбок сън: пиновете се освобождават)
  board_iface_power(true);                // сензорите се захранват само за измерването
  hal_delay_ms(SENSOR_POWER_SETTLE_MS);   // ~1 s: сензорите се събуждат; измерването е по команда и е свежо
  sensors_init();
  SensorSample sample;
  sensors_read(&sample);
  sensors_deinit();                       // шините не са нужни повече; радиото и сънят следват
  board_iface_power(false);               // сензорите се изключват веднага след измерването
  if (cfg.hasBattery) sample.bat_mv = board_battery_mv();   // радиото е в покой: ADC не се бърка с предаване

  if (sensor_radio_init(&cfg)) {
    sensor_send_packet(&cfg, &sample);
  } else {
    ESP_LOGE(TAG, "радиото не стартира - пропускам този цикъл");
  }

  power_deep_sleep_us(SLEEP_US);
}
