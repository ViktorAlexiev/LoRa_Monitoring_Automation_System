#include "gw_radio.h"
#include <stdint.h>
#include <inttypes.h>

#include <string.h>
#include "board_pins.h"
#include "agro_radio.h"
#include "channel_access.h"
#include "crypto_common.h"
#include "dedup.h"
#include "esp_log.h"
#include "agro_hal.h"
#include "protocol.h"
#include "radio_timing.h"
#include "esp_task_wdt.h"

static const char *TAG = "gw_radio";

#define GW_WAIT_FOREVER UINT32_C(0xFFFFFFFF)
#define MAX_RX_PER_SERVICE 16
#define MAX_SERVICE_PASSES 8

static const GwConfig *s_cfg = nullptr;
static QueueHandle_t s_gwInQ = nullptr, s_radioInQ = nullptr;
static TaskHandle_t s_task = nullptr;

// ---------- Dedup буфер на sensor пакети (пази от дублиране при 2+ repeater-и) ----------
// Пази суровите radio байтове (различни при всяка трансмисия благодарение на nonce-а). Директният и препратеният
// от Repeater пакет са еднакви, затова дубликатът се познава със сравнение на цялото съдържание.
static DedupRing<GW_DEDUP_BUFFER_SIZE, GW_DEDUP_MAX_LEN> s_dedup;
static uint32_t s_dedupHits = 0;

// ---------- TX слот ----------
static TxReq s_cur;
static bool s_txActive = false;
static ChannelAccess s_ca;
static bool s_cadRan = false;

void gw_radio_reset(const GwConfig *cfg, QueueHandle_t gwInQ, QueueHandle_t radioInQ) {
  s_cfg = cfg;
  s_gwInQ = gwInQ;
  s_radioInQ = radioInQ;
  s_task = nullptr;
  s_dedup = DedupRing<GW_DEDUP_BUFFER_SIZE, GW_DEDUP_MAX_LEN>();
  s_dedupHits = 0;
  s_txActive = false;
}
void gw_radio_set_task(TaskHandle_t t) { s_task = t; }
uint32_t gw_radio_dedup_hits(void) { return s_dedupHits; }
bool gw_radio_tx_active(void) { return s_txActive; }

static inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static void post(GwMsg &m) {
  if (xQueueSend(s_gwInQ, &m, 0) != pdTRUE) ESP_LOGE(TAG, "[ERR] опашката към логиката е пълна, съобщение тип %" PRId32 " е изгубено", (int32_t)m.type);
}

static void idFrom(char *dst, const uint8_t *senderId) {
  memset(dst, 0, CRYPTO_ID_LEN + 1);
  memcpy(dst, senderId, CRYPTO_ID_LEN);
}

// ---------- Приемане: пакет -> (валидиран) GwMsg ----------
// buf: прочетеното (до GW_RX_BUF_MAX байта), len: истинската дължина на пакета
static void handleIncoming(const uint8_t *buf, int32_t len) {
  const uint8_t *key = s_cfg->key;

  GwMsg m;
  memset(&m, 0, sizeof(m));
  m.rssi = (int16_t)radio.getRSSI();
  m.snrX100 = (int16_t)(radio.getSNR() * 100.0f);
  m.tsSec = hal_millis() / 1000;

  uint8_t senderId[CRYPTO_ID_LEN];
  uint8_t plaintext[CRYPTO_MAX_PT];
  uint8_t ptLen;

  m.batMv = BATTERY_RAW_NODATA;            // старите пакети (без батерия) оставят "няма измерване"

  // Сензорни пакети (директни или препратени от Repeater - без маркер): 22 B (стар формат, без батерия) или 24 B (с батерия).
  // 24 B е и дължината на state response от 2 консуматора: ако MAC-ът не излезе като sensor, продължава към state response.
  if (len == SENSOR_WIRE_LEN || len == SENSOR_BAT_WIRE_LEN) {
    const bool withBat = (len == SENSOR_BAT_WIRE_LEN);
    if (s_dedup.seen(buf, (uint8_t)len)) {
      s_dedupHits++;
      ESP_LOGI(TAG, "  -> дублиран sensor пакет (вече видян), игнориран");
      return;
    }
    if (cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_SENSOR, senderId, plaintext, &ptLen) &&
        ptLen == (withBat ? SENSOR_BAT_PT_LEN : SENSOR_PT_LEN)) {
      s_dedup.add(buf, (uint8_t)len);
      m.type = GM_SENSOR;
      idFrom(m.id, senderId);
      for (int32_t i = 0; i < 4; i++) m.v[i] = (int16_t)((uint16_t)plaintext[2 * i] | ((uint16_t)plaintext[2 * i + 1] << 8));
      if (withBat) m.batMv = (int16_t)((uint16_t)plaintext[8] | ((uint16_t)plaintext[9] << 8));
      post(m);
      return;
    }
    if (!withBat) {
      ESP_LOGW(TAG, "  -> sensor пакет: невалиден MAC, отхвърлен");
      return;
    }
    // 24 B, но не е sensor: може да е state response от 2 консуматора - проверява се по-долу
  }

  if (len == ACK_WIRE_LEN) {
    if (!cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_EXEC_ACK, senderId, plaintext, &ptLen) || ptLen != ACK_PT_LEN) {
      ESP_LOGW(TAG, "  -> ACK: невалиден MAC, отхвърлен");
      return;
    }
    m.type = GM_ACK;
    idFrom(m.id, senderId);
    memcpy(m.cId, plaintext, 4);
    m.com = plaintext[4];
    m.status = plaintext[5];
    post(m);
  } else if (len == HB_WIRE_LEN) {
    // HB от repeater, HB от executor и state response с 0 консуматора имат еднаква wire дължина (14, без данни) -
    // "типа" не пътува по въздуха, пробваме поред.
    if (cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_REPEATER_HB, senderId, plaintext, &ptLen)) {
      m.type = GM_HB_REPEATER;
      idFrom(m.id, senderId);
      post(m);
    } else if (cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_EXEC_HB, senderId, plaintext, &ptLen)) {
      m.type = GM_HB_EXEC;
      idFrom(m.id, senderId);
      post(m);
    } else if (cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_STATE_RESP, senderId, plaintext, &ptLen)) {
      m.type = GM_STATE; m.restart = 0; m.stateLen = ptLen; idFrom(m.id, senderId); post(m);
    } else if (cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_STATE_RESP_RESTART, senderId, plaintext, &ptLen)) {
      m.type = GM_STATE; m.restart = 1; m.stateLen = ptLen; idFrom(m.id, senderId); post(m);
    } else {
      ESP_LOGW(TAG, "  -> HB/STATE(0) пакет: невалиден MAC, отхвърлен");
    }
  } else if (len == HB_BAT_WIRE_LEN) {
    // Heartbeat с батерия (ESP32 PCB): repeater или executor ("типа" не пътува по въздуха - пробваме поред); plaintext = mV
    if (cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_REPEATER_HB, senderId, plaintext, &ptLen) && ptLen == HB_BAT_PT_LEN) {
      m.type = GM_HB_REPEATER;
    } else if (cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_EXEC_HB, senderId, plaintext, &ptLen) && ptLen == HB_BAT_PT_LEN) {
      m.type = GM_HB_EXEC;
    } else {
      ESP_LOGW(TAG, "  -> HB с батерия: невалиден MAC, отхвърлен");
      return;
    }
    idFrom(m.id, senderId);
    m.batMv = (int16_t)((uint16_t)plaintext[0] | ((uint16_t)plaintext[1] << 8));
    post(m);
  } else if (len > CRYPTO_OVERHEAD && (len - CRYPTO_OVERHEAD) % STATE_ENTRY_LEN == 0 &&
             (len - CRYPTO_OVERHEAD) <= MAX_CONSUMERS * STATE_ENTRY_LEN) {
    // State response с 1+ консуматор - разпознава се по (len - CRYPTO_OVERHEAD) кратно на STATE_ENTRY_LEN(5).
    // Не съвпада числено с нито един друг тип (ACK ptLen=6, SENSOR ptLen=8 - нито едно не е кратно на 5).
    bool restart;
    if (cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_STATE_RESP, senderId, plaintext, &ptLen)) restart = false;
    else if (cryptoParseWirePacket(buf, (uint8_t)len, key, CRYPTO_TYPE_STATE_RESP_RESTART, senderId, plaintext, &ptLen)) restart = true;
    else { ESP_LOGW(TAG, "  -> STATE_RESP: невалиден MAC, отхвърлен"); return; }
    m.type = GM_STATE;
    m.restart = restart ? 1 : 0;
    m.stateLen = ptLen;
    idFrom(m.id, senderId);
    memcpy(m.state, plaintext, ptLen);
    post(m);
  } else {
    ESP_LOGI(TAG, "  -> UNKNOWN length %" PRId32 ", ignored", len);
    return;
  }
}

static void rxDrain() {
  uint8_t buf[GW_RX_BUF_MAX];
  for (int32_t i = 0; i < MAX_RX_PER_SERVICE; i++) {
    int32_t n = radio_read_packet(buf, sizeof(buf));
    if (n == RADIO_RX_NONE) break;
    if (n == RADIO_RX_CRC_ERROR) { ESP_LOGD(TAG, "[RX] пакет с лош CRC, изхвърлен"); continue; }
    handleIncoming(buf, n);
  }
  hal_irq_rearm(LORA_DIO1);                      // DIO1 пада, когато флаговете са изчистени
}

// ---------- Предаване: командният път (команда, state request, state-resp ACK) ----------
// CAD оставя радиото в standby - връщаме го на слушане само след реално изпълнен CAD
static bool cadProbe() {
  if (radio_rx_pending()) return true;   // чака непрочетен пакет: не пипаме радиото (CAD би го унищожил); каналът е зает
  s_cadRan = true;
  return channelActive();
}

static bool txTick() {
  if (!s_txActive) {
    if (xQueueReceive(s_radioInQ, &s_cur, 0) != pdTRUE) return false;
    channelAccessStart(&s_ca, CH_POLICY_COMMAND);
    s_txActive = true;
  }
  s_cadRan = false;
  uint8_t r = channelAccessPoll(&s_ca, hal_millis(), cadProbe, channelRandom);
  if (r == CH_WAIT) {
    if (s_cadRan) radio_listen();                // между проверките слушаме
    return false;
  }
  if (r == CH_CLEAR) {
    bool ok = radio_transmit(s_cur.buf, s_cur.len);
    radio_listen();
    if (ok) ESP_LOGI(TAG, "[TX] изпратен пакет, len=%" PRIu32, (uint32_t)s_cur.len);
    else ESP_LOGE(TAG, "[TX] предаването се провали, len=%" PRIu32, (uint32_t)s_cur.len);
  } else {
    // срокът за сондиране изтече - каналът остана зает. Пакетът се отхвърля; надеждността е на по-горния
    // слой (retry на командата/state request, retry на Executor за restart).
    radio_listen();
    ESP_LOGW(TAG, "[TX] канала остана зает, отказвам се от този пакет (retry на по-горния слой)");
  }
  s_txActive = false;
  return true;
}

uint32_t gw_radio_service(void) {
  bool progress = false;
  for (int32_t pass = 0; pass < MAX_SERVICE_PASSES; pass++) {
    rxDrain();
    bool p = txTick();
    progress = p;
    if (!p) break;
  }
  esp_task_wdt_reset();                          // признак на живот към Task Watchdog (не е грешка, ако задачата не е абонирана)
  uint32_t w = GW_WAIT_FOREVER;
  if (s_txActive) w = channelAccessNextMs(&s_ca, hal_millis());
  if (w == 0 && !progress) w = 1;
  return w;
}

void gw_radio_task(void *) {
  s_task = xTaskGetCurrentTaskHandle();
  esp_task_wdt_add(nullptr);                     // от тук нататък задачата трябва да "храни" watchdog-а (в т.ч. при мъртъв радио чип -> рестарт)
  RadioParams p;
  p.freqHz = s_cfg->freqHz;
  p.sf = s_cfg->sf;
  p.bwHz = s_cfg->bwHz;
  p.txPowerDbm = s_cfg->txPowerDbm;
  radio_setup(&p, 0);                            // докато успее (постоянно захранено устройство)
  radio_listen();                                // непрекъснат прием (DIO1 = RxDone)
  if (!hal_irq_attach(LORA_DIO1, s_task)) hal_fatal_restart("прекъсването на DIO1 не се закачи");

  for (;;) {
    HAL_STACK_PROBE();
    uint32_t w = gw_radio_service();
    if (w > GW_RADIO_MAX_WAIT_MS) w = GW_RADIO_MAX_WAIT_MS;   // Task Watchdog: поне веднъж на 5 s
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(w));
  }
}
