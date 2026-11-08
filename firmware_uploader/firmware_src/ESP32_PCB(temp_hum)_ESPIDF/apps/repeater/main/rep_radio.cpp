#include "rep_radio.h"
#include <stdint.h>
#include <inttypes.h>

#include <string.h>
#include "board_pins.h"
#include "agro_radio.h"
#include "ceiling_counter.h"
#include "channel_access.h"
#include "crypto_common.h"
#include "dedup.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "agro_hal.h"
#include "board_power.h"
#include "protocol.h"
#include "radio_timing.h"

static const char *TAG = "rep_radio";

static const RepConfig *s_cfg = nullptr;
static QueueHandle_t s_fwdQ = nullptr;
static SemaphoreHandle_t s_mutex = nullptr;
static TaskHandle_t s_rxTask = nullptr;
static DedupRing<DEDUP_BUFFER_SIZE, DEDUP_MAX_LEN> s_dedup;
static CeilingCounter s_hbCounter;            // само от задачата rep_fwd
static bool s_counterReady = false;
static uint32_t s_dedupHits = 0;
static esp_timer_handle_t s_hbTimer = nullptr;

static inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

void rep_radio_reset(const RepConfig *cfg, QueueHandle_t fwdQ, SemaphoreHandle_t radioMutex) {
  s_cfg = cfg;
  s_fwdQ = fwdQ;
  s_mutex = radioMutex;
  s_rxTask = nullptr;
  s_dedup = DedupRing<DEDUP_BUFFER_SIZE, DEDUP_MAX_LEN>();
  s_counterReady = false;
  s_dedupHits = 0;
  s_hbTimer = nullptr;
}

uint32_t rep_dedup_hits(void) { return s_dedupHits; }

void rep_enter_rx_mode(void) {
  radio_set_frequency(s_cfg->freqRxHz);   // честотата се сменя в standby
  radio_listen();
}

bool rep_radio_begin(void) {
  RadioParams p;
  p.freqHz = s_cfg->freqRxHz;
  p.sf = s_cfg->sf;
  p.bwHz = s_cfg->bwHz;
  p.txPowerDbm = s_cfg->txPowerDbm;
  if (!radio_setup(&p, 0)) return false;
  rep_enter_rx_mode();
  return true;
}

// ---------------- Приемане ----------------
// buf: прочетеното (до DEDUP_MAX_LEN байта), len: истинската дължина на пакета
static void rxHandle(const uint8_t *buf, int32_t len) {
  if (len <= 0) return;
  // По-дълъг пакет не минава през repeater-а (сензорните са 22 B, heartbeat-ите 14 B): отрязано копие би се препратило
  // като повреден кадър (приемникът го отхвърля по MAC) и би хабило ефирно време. Затова се изхвърля цял.
  if (len > DEDUP_MAX_LEN) {
    ESP_LOGW(TAG, "[RX] пакет от %" PRId32 " B е над границата %" PRId32 " B - не се препраща", len, (int32_t)DEDUP_MAX_LEN);
    return;
  }
  int32_t i = len;

  // Вече сме препратили точно тези байтове наскоро (дублиран прием, напр. чут и от двама repeater-и на
  // предишното ниво) - пропусни. Пакетът не се променя при препращане, затова дубликатите са байт по байт еднакви.
  if (s_dedup.seen(buf, (uint8_t)i)) {
    s_dedupHits++;
    ESP_LOGI(TAG, "[RX] дублиран пакет (dedup), игнориран");
    return;
  }
  s_dedup.add(buf, (uint8_t)i);

  FwdItem it;
  memset(&it, 0, sizeof(it));
  it.kind = FWD_KIND_FRAME;
  it.len = (uint8_t)i;
  memcpy(it.buf, buf, (size_t)i);
  it.sendAt = hal_millis() + hal_random(FORWARD_BACKOFF_MAX_MS);
  if (xQueueSend(s_fwdQ, &it, 0) == pdTRUE) {
    ESP_LOGI(TAG, "[RX] пакет приет, len=%" PRId32 " -> опашка за препращане", i);
  } else {
    // опашката е пълна (много рядко - 4 едновременни forward-а) - пакетът се губи
    ESP_LOGE(TAG, "[ERR] forward опашка пълна, пакет изгубен");
  }
}

void rep_rx_service(void) {
  uint8_t buf[DEDUP_MAX_LEN];
  for (int32_t k = 0; k < 16; k++) {
    int32_t n = radio_read_packet(buf, sizeof(buf));
    if (n == RADIO_RX_NONE) break;
    if (n == RADIO_RX_CRC_ERROR) continue;       // повреден пакет не се препраща
    rxHandle(buf, n);
  }
  hal_irq_rearm(LORA_DIO1);                      // DIO1 пада, когато флаговете са изчистени
}

// ---------------- Препращане и heartbeat ----------------
// CAD се прави на TX честотата (там ще предаваме); помним дали е бил изпълнен на този ход, за да върнем
// радиото на RX честотата само тогава.
static bool s_cadRan = false;
static bool cadOnTxFreq() {
  s_cadRan = true;
  radio_set_frequency(s_cfg->freqTxHz);
  return channelActive();
}

static bool transmitNow(const uint8_t *buf, uint8_t len) {
  radio_set_frequency(s_cfg->freqTxHz);    // изрично: радиото да е на TX честотата в момента на предаването
  bool ok = radio_transmit(buf, len);
  rep_enter_rx_mode();
  return ok;
}

void rep_fwd_process(const FwdItem &it) {
  // Изчакване до планираното време (backoff/jitter) - mutex-ът НЕ се държи, rx продължава да приема
  uint32_t now = hal_millis();
  if (!reached(now, it.sendAt)) hal_delay_ms((uint32_t)(it.sendAt - now));

  // Батерията се мери ПРЕДИ mutex-а на радиото (ADC отнема ~20-30 ms, а rx не бива да чака за това)
  int16_t batMv = BATTERY_RAW_NODATA;
  if (it.kind == FWD_KIND_HB && s_cfg->hasBattery) batMv = board_battery_mv();

  // FORWARD (PERSIST): изчаква паузата след чуждо предаване (напр. друг repeater препраща същия кадър - иначе се
  // предава върху него и кадърът се губи), а ако каналът е запушен цял срок - предава въпреки това (измерването е
  // по-ценно от чистия ефир); HB - при зает канал се пропуска до следващия
  ChannelAccess ca;
  channelAccessStart(&ca, it.kind == FWD_KIND_HB ? CH_POLICY_TELEMETRY_SKIP : CH_POLICY_TELEMETRY_PERSIST);

  for (;;) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (radio_rx_pending()) {
      // в чипа чака непрочетен пакет: CAD/смяната на честотата би го унищожила. rx задачата (по-висок приоритет) вече е
      // събудена от DIO1 и чака mutex-а - щом го освободим, тя го чете.
      xSemaphoreGive(s_mutex);
      hal_delay_ms(1);
      continue;
    }
    s_cadRan = false;
    uint8_t r = channelAccessPoll(&ca, hal_millis(), cadOnTxFreq, channelRandom);
    if (r == CH_WAIT) {
      if (s_cadRan) rep_enter_rx_mode();         // между проверките слушаме на RX честотата
      uint32_t w = channelAccessNextMs(&ca, hal_millis());
      xSemaphoreGive(s_mutex);
      hal_delay_ms(w > 0 ? w : 1);
      continue;
    }

    if (it.kind == FWD_KIND_FRAME) {
      bool ok = transmitNow(it.buf, it.len);
      if (ok) ESP_LOGI(TAG, "[TX FORWARD] len=%" PRIu32, (uint32_t)it.len);
      else ESP_LOGE(TAG, "[TX FORWARD] ПРОВАЛ len=%" PRIu32, (uint32_t)it.len);
    } else if (r == CH_CLEAR) {
      if (!s_counterReady) { s_hbCounter.begin("ceil"); s_counterReady = true; }
      uint32_t ctr;
      uint8_t wire[CRYPTO_OVERHEAD + HB_BAT_PT_LEN];
      if (s_hbCounter.next(&ctr)) {
        // С батерия: heartbeat носи mV (int16 LE) -> 16 B; без: 14 B като досега
        uint8_t pt[HB_BAT_PT_LEN] = {(uint8_t)((uint16_t)batMv & 0xFF), (uint8_t)((uint16_t)batMv >> 8)};
        uint8_t ptLen = s_cfg->hasBattery ? HB_BAT_PT_LEN : 0;
        uint8_t n = cryptoBuildWirePacket(wire, s_cfg->key, CRYPTO_TYPE_REPEATER_HB, (const uint8_t *)s_cfg->id, ctr,
                                          ptLen ? pt : NULL, ptLen);
        if (n && transmitNow(wire, n)) ESP_LOGI(TAG, "[TX HB] M_ID=%s", s_cfg->id);
        else rep_enter_rx_mode();
      } else {
        rep_enter_rx_mode();
      }
    } else {
      rep_enter_rx_mode();
      ESP_LOGW(TAG, "[HB] канала зает, пропускам този heartbeat");
    }
    xSemaphoreGive(s_mutex);
    return;
  }
}

void rep_hb_due(void) {
  FwdItem it;
  memset(&it, 0, sizeof(it));
  it.kind = FWD_KIND_HB;
  it.sendAt = hal_millis() + hal_random(HB_JITTER_MAX_MS);     // jitter разминава heartbeat-и от различни repeater-и
  if (xQueueSend(s_fwdQ, &it, 0) != pdTRUE) ESP_LOGW(TAG, "[HB] опашката е пълна - пропускам този heartbeat");
}

// ---------------- Задачите ----------------
// Еднократен таймер, който сам се пререгистрира: първият срок е на случайна фаза (виж HB_FIRST_MIN_MS в protocol.h)
static void hbTimerCb(void *) {
  rep_hb_due();
  if (s_hbTimer) esp_timer_start_once(s_hbTimer, (uint64_t)HB_INTERVAL_MS * UINT64_C(1000));
}

void rep_rx_task(void *) {
  s_rxTask = xTaskGetCurrentTaskHandle();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  rep_radio_begin();
  xSemaphoreGive(s_mutex);
  if (!hal_irq_attach(LORA_DIO1, s_rxTask)) hal_fatal_restart("прекъсването на DIO1 не се закачи");

  esp_timer_create_args_t ta = {};
  ta.callback = hbTimerCb;
  ta.name = "rep_hb";
  if (esp_timer_create(&ta, &s_hbTimer) == ESP_OK) {
    esp_timer_start_once(s_hbTimer, (uint64_t)hal_random_range(HB_FIRST_MIN_MS, HB_INTERVAL_MS + 1) * UINT64_C(1000));
  }

  for (;;) {
    HAL_STACK_PROBE();
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);     // само DIO1 - никакво опресняване; чипът спи между пакетите
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    rep_rx_service();
    xSemaphoreGive(s_mutex);
  }
}

void rep_fwd_task(void *) {
  FwdItem it;
  for (;;) {
    HAL_STACK_PROBE();
    if (xQueueReceive(s_fwdQ, &it, portMAX_DELAY) == pdTRUE) rep_fwd_process(it);
  }
}
