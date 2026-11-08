#include "sensors_io.h"
#include <inttypes.h>

#include <string.h>
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "agro_hal.h"
#include "protocol.h"

static const char *TAG = "sensors";

// Физически обхват на SHT31 по datasheet: -40..125 °C, 0..100 %RH (суровите стойности дават -45..130 и 0..100).
// 1 °C запас от двете страни: на самата граница квантуването (0.0027 °C) не бива да превръща реална стойност в "няма данни".
#define SHT31_TEMP_MIN_C  -41.0f
#define SHT31_TEMP_MAX_C  126.0f

#define I2C_TIMEOUT_MS    100
#define SHT31_MEASURE_MS  20        // еднократно измерване, високо повторение: до 15 ms

// Един канал = един сензор на собствена шина (при мултиплексор: същата таблица с друг адрес/мултиплексиране).
struct ShtChannel {
  const char *name;
  uint8_t port;
  uint8_t sda;
  uint8_t scl;
  uint8_t addr;
  i2c_master_bus_handle_t bus;
  i2c_master_dev_handle_t dev;
  bool ok;
};

static ShtChannel s_ch[SENSOR_CHANNELS] = {
    {"почва", I2C_SOIL_PORT, I2C_SOIL_SDA, I2C_SOIL_SCL, SHT31_SOIL_ADDR, nullptr, nullptr, false},
    {"въздух", I2C_AIR_PORT, I2C_AIR_SDA, I2C_AIR_SCL, SHT31_AIR_ADDR, nullptr, nullptr, false},
};

// Sensirion CRC-8 (полином 0x31, начална стойност 0xFF за SHT31)
static uint8_t crc8(const uint8_t *d, size_t n) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int32_t b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

bool sht31_decode(const uint8_t b[6], float *tempC, float *humPct) {
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return false;
  uint16_t rt = (uint16_t)((b[0] << 8) | b[1]);
  uint16_t rh = (uint16_t)((b[3] << 8) | b[4]);
  float t = -45.0f + 175.0f * ((float)rt / 65535.0f);
  float h = 100.0f * ((float)rh / 65535.0f);
  if (t < SHT31_TEMP_MIN_C || t > SHT31_TEMP_MAX_C) return false;      // извън обхвата на сензора
  *tempC = t;
  *humPct = h;
  return true;
}

static void channelClose(ShtChannel &c) {
  if (c.dev) i2c_master_bus_rm_device(c.dev);
  if (c.bus) i2c_del_master_bus(c.bus);
  c.dev = nullptr;
  c.bus = nullptr;
  c.ok = false;
}

static bool channelOpen(ShtChannel &c) {
  c.ok = false;
  i2c_master_bus_config_t bc;
  memset(&bc, 0, sizeof(bc));
  bc.i2c_port = (i2c_port_num_t)c.port;
  bc.sda_io_num = (gpio_num_t)c.sda;
  bc.scl_io_num = (gpio_num_t)c.scl;
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = true;     // слаб pull-up; платката трябва да има външни
  if (i2c_new_master_bus(&bc, &c.bus) != ESP_OK) {
    ESP_LOGE(TAG, "I2C шина за %s (порт %" PRIu32 ") не се вдигна", c.name, (uint32_t)c.port);
    c.bus = nullptr;
    return false;
  }
  i2c_device_config_t dc;
  memset(&dc, 0, sizeof(dc));
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = c.addr;
  dc.scl_speed_hz = I2C_HZ;
  if (i2c_master_bus_add_device(c.bus, &dc, &c.dev) != ESP_OK) {
    ESP_LOGE(TAG, "SHT31 (%s) не се добави на шината", c.name);
    c.dev = nullptr;
    channelClose(c);
    return false;
  }
  c.ok = true;
  const uint8_t reset[2] = {0x30, 0xA2};      // soft reset (както Adafruit begin())
  i2c_master_transmit(c.dev, reset, sizeof(reset), I2C_TIMEOUT_MS);   // липсващ сензор не е грешка тук
  hal_delay_ms(2);
  return true;
}

static bool channelRead(const ShtChannel &c, float *t, float *h) {
  if (!c.ok) return false;
  const uint8_t cmd[2] = {0x24, 0x00};        // еднократно измерване, високо повторение, без clock stretching
  if (i2c_master_transmit(c.dev, cmd, sizeof(cmd), I2C_TIMEOUT_MS) != ESP_OK) return false;
  hal_delay_ms(SHT31_MEASURE_MS);
  uint8_t b[6];
  if (i2c_master_receive(c.dev, b, sizeof(b), I2C_TIMEOUT_MS) != ESP_OK) return false;
  return sht31_decode(b, t, h);
}

bool sensors_init(void) {
  bool any = false;
  for (uint8_t i = 0; i < SENSOR_CHANNELS; i++) {
    channelClose(s_ch[i]);                    // безопасно повторно извикване
    any |= channelOpen(s_ch[i]);
  }
  return any;
}

void sensors_deinit(void) {
  for (uint8_t i = 0; i < SENSOR_CHANNELS; i++) channelClose(s_ch[i]);
}

void sensors_read(SensorSample *out) {
  out->s_t = out->s_h = out->a_t = out->a_h = SENSOR_RAW_NODATA;
  out->bat_mv = BATTERY_RAW_NODATA;           // попълва се от app_main, ако има батерия

  // Двете стойности на един SHT31 идват от едно измерване, затова са валидни само заедно
  float t, h;
  if (channelRead(s_ch[SENSOR_CH_SOIL], &t, &h)) {
    out->s_t = sensorEncodeCompact(t);
    out->s_h = sensorEncodeCompact(h);
  } else {
    ESP_LOGW(TAG, "SHT31 (почва) - няма данни (нерабоща шина, грешка при четене, лоша CRC или извън обхват)");
  }
  if (channelRead(s_ch[SENSOR_CH_AIR], &t, &h)) {
    out->a_t = sensorEncodeCompact(t);
    out->a_h = sensorEncodeCompact(h);
  } else {
    ESP_LOGW(TAG, "SHT31 (въздух) - няма данни (нерабоща шина, грешка при четене, лоша CRC или извън обхват)");
  }
}
