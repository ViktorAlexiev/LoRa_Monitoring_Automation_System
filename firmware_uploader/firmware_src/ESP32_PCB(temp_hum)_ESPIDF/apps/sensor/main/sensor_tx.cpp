#include "sensor_tx.h"
#include <inttypes.h>

#include <stddef.h>
#include <string.h>
#include "agro_radio.h"
#include "ceiling_counter.h"
#include "channel_access.h"
#include "crypto_common.h"
#include "esp_log.h"
#include "agro_hal.h"
#include "radio_timing.h"

static const char *TAG = "sensor_tx";

#define SENSOR_RADIO_TRIES 5     // опита за radio.begin() на събуждане (после сънят продължава до следващия цикъл)

static CeilingCounter s_txCounter;
static bool s_counterReady = false;

void sensor_tx_reset(void) { s_counterReady = false; }

bool sensor_radio_init(const SensorConfig *c) {
  RadioParams p;
  p.freqHz = c->freqHz;
  p.sf = c->sf;
  p.bwHz = c->bwHz;
  p.txPowerDbm = c->txPowerDbm;
  return radio_setup(&p, SENSOR_RADIO_TRIES);
}

// Компактен формат: 4 x int16 little-endian са plaintext-ът, който се криптира; S_ID НЕ е част от него -
// пътува в чисто (wire формат) като sender identity, нужна на gateway-я преди decrypt.
// Опционално 5-то поле BAT (mV): при устройство БЕЗ батерия се криптират само първите SENSOR_PT_LEN (8) байта - точно
// старият формат (22 B по въздуха); с батерия - 10 байта (24 B по въздуха).
struct __attribute__((packed)) SensorPacketData {
  int16_t S_T, S_H, A_T, A_H;
  int16_t BAT;
};
static_assert(sizeof(SensorPacketData) == SENSOR_BAT_PT_LEN, "sensor plaintext с батерия трябва да е 10 B");
static_assert(offsetof(SensorPacketData, BAT) == SENSOR_PT_LEN, "старият формат е префикс на новия (8 B)");

bool sensor_send_packet(const SensorConfig *c, const SensorSample *s) {
  if (!c->hasId || !c->hasKey) {
    ESP_LOGE(TAG, "няма id/ключ - не предавам (устройството не е конфигурирано)");
    return false;
  }

  if (!s_counterReady) { s_txCounter.begin("ceil"); s_counterReady = true; }

  SensorPacketData pkt;
  pkt.S_T = s->s_t;
  pkt.S_H = s->s_h;
  pkt.A_T = s->a_t;
  pkt.A_H = s->a_h;
  pkt.BAT = s->bat_mv;
  const uint8_t ptLen = c->hasBattery ? SENSOR_BAT_PT_LEN : SENSOR_PT_LEN;   // без батерия: старият формат, 22 B

  hal_delay_ms(hal_random(JITTER_MAX_MS));   // jitter - разминава TX-а с други sensor-и на честотата

  ESP_LOGI(TAG, "TX S_ID=%s S_T=%d S_H=%d A_T=%d A_H=%d BAT=%d mV (x0.01; -32768 = няма данни) (wire %" PRId32 " B)",
           c->id, s->s_t, s->s_h, s->a_t, s->a_h, c->hasBattery ? s->bat_mv : BATTERY_RAW_NODATA, (int32_t)(CRYPTO_OVERHEAD + ptLen));

  uint8_t senderId[CRYPTO_ID_LEN] = {0};
  memcpy(senderId, c->id, strnlen(c->id, CRYPTO_ID_LEN));

  uint32_t counter;
  if (!s_txCounter.next(&counter)) return false;

  uint8_t wire[CRYPTO_OVERHEAD + SENSOR_BAT_PT_LEN];
  uint8_t wireLen = cryptoBuildWirePacket(wire, c->key, CRYPTO_TYPE_SENSOR, senderId, counter,
                                          (const uint8_t *)&pkt, ptLen);
  if (wireLen == 0) { ESP_LOGE(TAG, "криптирането се провали"); return false; }

  // Listen-before-talk (телеметрия, PERSIST): един CAD; ако е зает - изчаква пауза (до срока = най-дългият възможен
  // пакет), случаен слот и последен CAD; ако каналът е запушен целия срок, предава въпреки това (едно измерване е
  // по-ценно от чистия ефир). Sensor няма друга работа, затова тук блокираме (и после заспива).
  ChannelAccess ca;
  channelAccessStart(&ca, CH_POLICY_TELEMETRY_PERSIST);
  uint8_t r;
  while ((r = channelAccessPoll(&ca, hal_millis(), channelActive, channelRandom)) == CH_WAIT) {
    uint32_t w = channelAccessNextMs(&ca, hal_millis());
    hal_delay_ms(w > 0 ? w : 1);
  }

  bool ok = radio_transmit(wire, wireLen);
  if (!ok) ESP_LOGE(TAG, "предаването се провали");
  radio.sleep(false);                         // най-малък ток; при следващото събуждане begin() прави пълна инициализация
  return ok;
}
