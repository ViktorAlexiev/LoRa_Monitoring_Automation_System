#include "board_power.h"

#include <inttypes.h>
#include "agro_hal.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "board_power";

static bool s_ifaceOn = false;

// ---------------- Ключове ----------------
static void pinOutputLow(gpio_num_t pin) {
  // След дълбок сън пинът още е заключен (hold). Документацията на ESP-IDF: ако заключването се свали, докато
  // регистрите са по подразбиране, пинът става вход и нивото пада (импулс). Затова ПЪРВО се задават ниво LOW и
  // режим изход (докато е заключен, пинът не се променя), и чак тогава се сваля заключването.
  gpio_set_level(pin, 0);
  gpio_config_t io = {};
  io.pin_bit_mask = UINT64_C(1) << (uint8_t)pin;
  io.mode = GPIO_MODE_OUTPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  gpio_hold_dis(pin);
}

void board_power_init(void) {
  gpio_deep_sleep_hold_dis();
  pinOutputLow((gpio_num_t)IFACE_PWR_EN_GPIO);
  pinOutputLow((gpio_num_t)BATT_DIV_EN_GPIO);
  s_ifaceOn = false;
}

void board_iface_power(bool on) {
  gpio_set_level((gpio_num_t)IFACE_PWR_EN_GPIO, on ? 1 : 0);
  s_ifaceOn = on;
}

bool board_iface_is_on(void) { return s_ifaceOn; }

void board_power_hold_for_deep_sleep(void) {
  gpio_hold_en((gpio_num_t)IFACE_PWR_EN_GPIO);
  gpio_hold_en((gpio_num_t)BATT_DIV_EN_GPIO);
  gpio_deep_sleep_hold_en();
}

// ---------------- Батерия ----------------
// Делител 1:1 (два еднакви резистора): на входа на АЦП е половината от напрежението на батерията (4.2 V -> 2.1 V).
// Затова атенюация 12 dB (обхват до ~3.1 V). АЦП на ESP32-S3 е нелинеен: ползва се фабричната калибровка (eFuse).
#define BATT_ADC_ATTEN       ADC_ATTEN_DB_12
#define BATT_SAMPLES         16            // усредняване: шумът от Wi-Fi/радио и от самия АЦП
#define BATT_VALID_MIN_MV    2000          // под това няма батерия (плаващ вход) или е мъртва - не е измерване
#define BATT_VALID_MAX_MV    4600          // над това (литиева 4.2 V + запас) е грешка

static adc_cali_handle_t caliCreate(adc_unit_t unit, adc_channel_t chan) {
  adc_cali_handle_t h = nullptr;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t c = {};
  c.unit_id = unit;
  c.chan = chan;
  c.atten = BATT_ADC_ATTEN;
  c.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_cali_create_scheme_curve_fitting(&c, &h) != ESP_OK) h = nullptr;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  adc_cali_line_fitting_config_t c = {};
  c.unit_id = unit;
  c.atten = BATT_ADC_ATTEN;
  c.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_cali_create_scheme_line_fitting(&c, &h) != ESP_OK) h = nullptr;
#else
  (void)unit;
  (void)chan;
#endif
  return h;
}

static void caliDelete(adc_cali_handle_t h) {
  if (h == nullptr) return;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_delete_scheme_curve_fitting(h);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  adc_cali_delete_scheme_line_fitting(h);
#endif
}

int16_t board_battery_mv(void) {
  adc_unit_t unit;
  adc_channel_t chan;
  if (adc_oneshot_io_to_channel(BATT_ADC_GPIO, &unit, &chan) != ESP_OK) {
    ESP_LOGE(TAG, "GPIO %" PRIu32 " не е вход на АЦП", (uint32_t)BATT_ADC_GPIO);
    return BOARD_BATTERY_NODATA;
  }

  adc_oneshot_unit_handle_t adc = nullptr;
  adc_oneshot_unit_init_cfg_t uc = {};
  uc.unit_id = unit;
  if (adc_oneshot_new_unit(&uc, &adc) != ESP_OK) {
    ESP_LOGE(TAG, "АЦП не се инициализира");
    return BOARD_BATTERY_NODATA;
  }
  adc_oneshot_chan_cfg_t cc = {};
  cc.atten = BATT_ADC_ATTEN;
  cc.bitwidth = ADC_BITWIDTH_DEFAULT;
  int16_t result = BOARD_BATTERY_NODATA;
  adc_cali_handle_t cali = nullptr;

  if (adc_oneshot_config_channel(adc, chan, &cc) == ESP_OK) {
    cali = caliCreate(unit, chan);
    gpio_set_level((gpio_num_t)BATT_DIV_EN_GPIO, 1);          // делителят се включва САМО за измерването
    hal_delay_ms(BATT_SETTLE_MS);                              // RC на делителя + входния кондензатор се стабилизира

    int32_t rawSum = 0;
    int32_t n = 0;
    for (int32_t i = 0; i < BATT_SAMPLES; i++) {
      int raw = 0;
      if (adc_oneshot_read(adc, chan, &raw) == ESP_OK) { rawSum += raw; n++; }
    }
    gpio_set_level((gpio_num_t)BATT_DIV_EN_GPIO, 0);

    if (n > 0) {
      int32_t rawAvg = rawSum / n;
      int adcMv = 0;
      if (cali != nullptr && adc_cali_raw_to_voltage(cali, (int)rawAvg, &adcMv) == ESP_OK) {
        int32_t vbat = (int32_t)adcMv * BATT_DIV_RATIO_NUM / BATT_DIV_RATIO_DEN;   // обратно през делителя
        if (vbat >= BATT_VALID_MIN_MV && vbat <= BATT_VALID_MAX_MV) result = (int16_t)vbat;
        else ESP_LOGW(TAG, "батерия: %" PRId32 " mV е извън 2000..4600 - няма измерване", vbat);
      } else {
        ESP_LOGW(TAG, "АЦП калибровка липсва/неуспешна - няма измерване (суровата стойност без калибровка не е точна)");
      }
    }
  }
  caliDelete(cali);
  adc_oneshot_del_unit(adc);
  return result;
}
