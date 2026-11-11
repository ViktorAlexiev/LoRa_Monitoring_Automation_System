#include "exec_radio.h"
#include <stdint.h>
#include <inttypes.h>

#include <atomic>
#include <string.h>
#include "board_pins.h"
#include "agro_radio.h"
#include "ceiling_counter.h"
#include "channel_access.h"
#include "crypto_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "power.h"
#include "consumers.h"
#include "agro_hal.h"
#include "board_power.h"
#include "protocol.h"
#include "radio_timing.h"

static const char *TAG = "exec_radio";

// ---------------- Общ TX слот с неблокиращ достъп до канала ----------------
// Един пакет наведнъж. Пакетът се строи (и получава counter) при заявката; достъпът до канала (CAD + сондиране
// + backoff) тече на стъпки в radioTxTick() и НЕ блокира приемането на входящи пакети. Команден път (ACK/NACK,
// state response, restart response): политика COMMAND. Heartbeat: телеметрия, при зает канал се пропуска.
#define TXK_ACK           1
#define TXK_STATE_RESP    2
#define TXK_RESTART_RESP  3
#define TXK_HB            4

#define ACK_LIST_SIZE        8
#define STATE_RESP_MAX_RETRIES 2        // общо до 3 опита (1 + 2 retry-я)
#define MAX_RX_PER_SERVICE   16
#define EXEC_RX_BUF          64        // най-големият пакет, който executor-ът разбира, е 26 B; по-дълги се броят само по дължина
#define MAX_SERVICE_PASSES    8

struct State {
  const ExecConfig *cfg = nullptr;
  QueueHandle_t cmdQ = nullptr, ackInQ = nullptr;
  TaskHandle_t task = nullptr;
  CeilingCounter txCounter;
  bool counterReady = false;

  AckReq acks[ACK_LIST_SIZE];
  uint8_t ackCount = 0;

  bool txPending = false;
  uint8_t txKind = 0;
  uint8_t txBuf[CRYPTO_OVERHEAD + MAX_CONSUMERS * STATE_ENTRY_LEN];
  uint8_t txLen = 0;
  char txTag[CONSUMER_ID_LEN + 1];
  uint8_t txStatus = 0;
  uint32_t txCreatedAt = 0;
  ChannelAccess txAccess;
  bool cadRan = false;

  bool restartPending = false;
  uint32_t restartSentAt = 0;
  uint8_t restartRetries = 0;

  bool hbPending = false;
  uint32_t hbSendAt = 0;
};

static State s;
static std::atomic<bool> s_hbDue(false);
static esp_timer_handle_t s_hbTimer = nullptr;

static inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

void exec_radio_reset(const ExecConfig *cfg, QueueHandle_t cmdQ, QueueHandle_t ackInQ) {
  s = State();
  s.cfg = cfg;
  s.cmdQ = cmdQ;
  s.ackInQ = ackInQ;
  s_hbDue.store(false);
  s_hbTimer = nullptr;
}

void exec_radio_set_task(TaskHandle_t t) { s.task = t; }
bool exec_radio_tx_busy(void) { return s.txPending; }
bool exec_radio_restart_pending(void) { return s.restartPending; }
int32_t exec_radio_ack_pending(void) { return s.ackCount; }

static bool nextCounter(uint32_t *c) {
  if (!s.counterReady) { s.txCounter.begin("ceil"); s.counterReady = true; }
  return s.txCounter.next(c);
}

static bool txRequest(uint8_t kind, const uint8_t *buf, uint8_t len, uint8_t policy) {
  if (s.txPending) return false;
  memcpy(s.txBuf, buf, len);
  s.txLen = len;
  s.txKind = kind;
  s.txCreatedAt = hal_millis();
  channelAccessStart(&s.txAccess, policy);
  s.txPending = true;
  return true;
}

// CAD оставя радиото в standby - помни дали на този ход е бил изпълнен, за да върнем радиото на слушане само
// тогава (без излишни записи в регистрите)
static bool cadProbe() {
  if (radio_rx_pending()) return true;   // чака непрочетен пакет: не пипаме радиото (CAD би го унищожил); каналът е зает
  s.cadRan = true;
  return channelActive();
}

static bool radioTxTick() {
  if (!s.txPending) return false;

  s.cadRan = false;
  uint8_t r = channelAccessPoll(&s.txAccess, hal_millis(), cadProbe, channelRandom);
  if (r == CH_WAIT) {
    if (s.cadRan) radio_listen();   // между проверките слушаме
    return false;
  }

  if (r == CH_CLEAR) {
    bool ok = radio_transmit(s.txBuf, s.txLen);
    radio_listen();
    if (!ok) {
      ESP_LOGE(TAG, "[TX] предаването се провали (вид=%" PRId32 ")", (int32_t)s.txKind);
    } else {
      switch (s.txKind) {
        case TXK_ACK:          ESP_LOGI(TAG, "[TX %s] %s", s.txStatus == STATUS_ACK ? "ACK" : "NACK", s.txTag); break;
        case TXK_STATE_RESP:   ESP_LOGI(TAG, "[TX STATE_RESP] изпратен"); break;
        case TXK_RESTART_RESP: ESP_LOGI(TAG, "[TX STATE_RESP RESTART] изпратен"); break;
        case TXK_HB:           ESP_LOGI(TAG, "[TX HB] M_ID=%s", s.cfg->id); break;
      }
    }
    s.txPending = false;
    return true;
  }

  // CH_GIVEUP - срокът за сондиране изтече, каналът остана зает
  radio_listen();
  if (s.txKind == TXK_ACK && (uint32_t)(hal_millis() - s.txCreatedAt) < radioAckTimeoutCmdMs()) {
    // ACK-ът още има смисъл (Gateway още чака) - започни нов цикъл на достъп
    ESP_LOGW(TAG, "[ACK/NACK] канала зает, опитвам пак, C_ID=%s", s.txTag);
    channelAccessStart(&s.txAccess, CH_POLICY_COMMAND);
    return false;
  }
  ESP_LOGW(TAG, "[TX] канала остана зает, отказвам се (вид=%" PRId32 ")", (int32_t)s.txKind);
  s.txPending = false;
  return true;
}

// ---------------- Строи и заявява криптиран state response (нормален или RESTART) ----------------
static bool sendStateResponseWire(uint8_t typeId) {
  if (s.txPending) {
    ESP_LOGW(TAG, "[STATE_RESP] TX слотът е зает, пропускам");
    return false;
  }
  const ExecConfig *cfg = s.cfg;
  uint8_t plaintext[MAX_CONSUMERS * STATE_ENTRY_LEN];
  for (uint8_t i = 0; i < cfg->consumers.count; i++) {
    uint8_t off = (uint8_t)(i * STATE_ENTRY_LEN);
    memcpy(plaintext + off, cfg->consumers.id[i], 4);
    plaintext[off + 4] = consumers_state(&cfg->consumers, i) ? 1 : 0;
  }
  uint8_t ptLen = (uint8_t)(cfg->consumers.count * STATE_ENTRY_LEN);

  uint32_t ctr;
  if (!nextCounter(&ctr)) return false;
  uint8_t wire[CRYPTO_OVERHEAD + MAX_CONSUMERS * STATE_ENTRY_LEN];
  uint8_t wireLen = cryptoBuildWirePacket(wire, cfg->key, typeId, (const uint8_t *)cfg->id, ctr, plaintext, ptLen);
  if (wireLen == 0) return false;

  uint8_t kind = (typeId == CRYPTO_TYPE_STATE_RESP_RESTART) ? TXK_RESTART_RESP : TXK_STATE_RESP;
  if (!txRequest(kind, wire, wireLen, CH_POLICY_COMMAND)) return false;

  ESP_LOGI(TAG, "[STATE_RESP%s] заявен, M_ID=%s consumers=%" PRIu32, kind == TXK_RESTART_RESP ? " RESTART" : "", cfg->id,
           (uint32_t)cfg->consumers.count);
  for (uint8_t i = 0; i < cfg->consumers.count; i++) {
    ESP_LOGI(TAG, "   %s = %s", cfg->consumers.id[i], consumers_state(&cfg->consumers, i) ? "ON" : "OFF");
  }
  return true;
}

// ---------------- Restart state response - ACK/retry state machine ----------------
// Timeout-ът на ACK-а се изчислява от SF/BW (radioAckTimeoutRestartMs), не е константа.
void exec_radio_request_restart_response(void) {
  s.restartPending = true;
  s.restartRetries = 0;
  s.restartSentAt = hal_millis();
  sendStateResponseWire(CRYPTO_TYPE_STATE_RESP_RESTART);
}

static bool restartRespManager() {
  if (!s.restartPending) return false;
  if ((uint32_t)(hal_millis() - s.restartSentAt) < radioAckTimeoutRestartMs()) return false;

  if (s.restartRetries < STATE_RESP_MAX_RETRIES) {
    // ако TX слотът е зает, не броим опита - пробваме пак след края на предаването
    if (sendStateResponseWire(CRYPTO_TYPE_STATE_RESP_RESTART)) {
      s.restartRetries++;
      s.restartSentAt = hal_millis();
      ESP_LOGW(TAG, "[RETRY STATE_RESP RESTART] опит #%" PRIu32, (uint32_t)s.restartRetries);
      return true;
    }
    return false;
  }
  ESP_LOGW(TAG, "[STATE_RESP RESTART] няма ACK след 3 опита, отказвам се, продължавам нормално");
  s.restartPending = false;
  return true;
}

// ---------------- Ниска батерия ----------------
void exec_low_battery_shutdown(const ExecConfig *cfg) {
  ESP_LOGW(TAG, "ниска батерия (под %" PRId32 " mV): консуматорите се изключват, дълбок сън за %" PRId32 " s", (int32_t)BATT_CUTOFF_MV, (int32_t)BATT_LOW_SLEEP_S);
  consumers_hold_for_deep_sleep(&cfg->consumers);   // консуматорите LOW и заключени за съня
  board_iface_power(false);                          // интерфейсите се изключват
  power_deep_sleep_us(BATT_LOW_SLEEP_US);            // заключва и двата ключа; при събуждане app_main пак мери батерията
}

// ---------------- Heartbeat (криптиран, без данни - само идентичност+counter+MAC) ----------------
void exec_radio_hb_due(void) {
  s_hbDue.store(true);
  if (s.task) xTaskNotifyGive(s.task);
}

static bool heartbeatTick() {
  bool progress = false;
  if (s_hbDue.exchange(false) && !s.hbPending) {
    s.hbPending = true;
    s.hbSendAt = hal_millis() + hal_random(JITTER_MAX_MS);   // jitter разминава колизии между устройства
    progress = true;
  }
  if (s.hbPending && !s.txPending && reached(hal_millis(), s.hbSendAt)) {
    uint32_t ctr;
    if (!nextCounter(&ctr)) { s.hbPending = false; return true; }   // този HB се пропуска (NVS провал)
    // С батерия: heartbeat носи mV (int16 LE, BATTERY_RAW_NODATA = няма измерване) -> 16 B; без: 14 B като досега
    uint8_t wire[CRYPTO_OVERHEAD + HB_BAT_PT_LEN];
    uint8_t pt[HB_BAT_PT_LEN];
    uint8_t ptLen = 0;
    if (s.cfg->hasBattery) {
      int16_t mv = board_battery_mv();        // блокира ~20-30 ms (радиото е в покой)
      if (board_battery_is_low(mv)) exec_low_battery_shutdown(s.cfg);   // под прага: не връща
      pt[0] = (uint8_t)((uint16_t)mv & 0xFF);
      pt[1] = (uint8_t)((uint16_t)mv >> 8);
      ptLen = HB_BAT_PT_LEN;
    }
    uint8_t n = cryptoBuildWirePacket(wire, s.cfg->key, CRYPTO_TYPE_EXEC_HB, (const uint8_t *)s.cfg->id, ctr,
                                      ptLen ? pt : NULL, ptLen);
    // телеметрия: при зает канал (два CAD-а) heartbeat-ът се пропуска - следващият идва по график
    if (n && txRequest(TXK_HB, wire, n, CH_POLICY_TELEMETRY_SKIP)) {
      s.hbPending = false;
      progress = true;
    }
  }
  return progress;
}

// ---------------- ACK/NACK на командите ----------------
static void drainAckIn() {
  AckReq req;
  while (s.ackCount < ACK_LIST_SIZE && xQueueReceive(s.ackInQ, &req, 0) == pdTRUE) s.acks[s.ackCount++] = req;
}

static void ackRemoveAt(uint8_t index) {
  for (uint8_t j = index; j + 1 < s.ackCount; j++) s.acks[j] = s.acks[j + 1];
  s.ackCount--;
}

static bool ackManager() {
  if (s.txPending) return false;   // един пакет наведнъж; следващият ACK изчаква реда си
  uint32_t now = hal_millis();
  for (uint8_t i = 0; i < s.ackCount; i++) {
    if (!reached(now, s.acks[i].sendAt)) continue;

    uint8_t plaintext[CMD_PLAINTEXT_LEN];
    memcpy(plaintext, s.acks[i].pkt.C_ID, 4);
    plaintext[4] = s.acks[i].pkt.com;
    plaintext[5] = s.acks[i].pkt.status;

    uint32_t ctr;
    if (!nextCounter(&ctr)) { ackRemoveAt(i); return true; }          // не излъчваме с непотвърден nonce
    uint8_t wire[CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN];
    uint8_t n = cryptoBuildWirePacket(wire, s.cfg->key, CRYPTO_TYPE_EXEC_ACK, (const uint8_t *)s.cfg->id, ctr,
                                      plaintext, sizeof(plaintext));
    memcpy(s.txTag, s.acks[i].pkt.C_ID, 4);
    s.txTag[4] = 0;
    s.txStatus = s.acks[i].pkt.status;
    if (n && txRequest(TXK_ACK, wire, n, CH_POLICY_COMMAND)) {
      ackRemoveAt(i);   // от тук нататък пакетът е собственост на TX слота
      return true;
    }
    return false;
  }
  return false;
}

// ---------------- Приемане ----------------
// buf: прочетеното от радиото (до EXEC_RX_BUF байта), packetSize: истинската дължина на пакета
static void rxHandle(const uint8_t *buf, int32_t packetSize) {
  const ExecConfig *cfg = s.cfg;

  if (packetSize == CMD_WIRE_LEN) {
    // target проверка ПРЕДИ decrypt - евтино, отхвърля пакети за други executor-и веднага
    if (memcmp(buf, cfg->id, MODULE_ID_LEN) != 0) {
      ESP_LOGI(TAG, "[RX CMD] не е за мен, игнориран");
      return;
    }
    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[CMD_PLAINTEXT_LEN];
    uint8_t ptLen;
    if (!cryptoParseWirePacket(buf + MODULE_ID_LEN, CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN, cfg->key, CRYPTO_TYPE_GW_CMD,
                               senderId, plaintext, &ptLen)) {
      ESP_LOGW(TAG, "[RX CMD] невалиден MAC, отхвърлен");
      return;
    }
    CommandPacket p;
    memcpy(p.C_ID, plaintext, 4);
    p.com = plaintext[4];
    p.status = plaintext[5];
    char c_id[5] = {0};
    memcpy(c_id, p.C_ID, 4);
    ESP_LOGI(TAG, "[RX CMD] C_ID=%s com=0x%02" PRIX32, c_id, (uint32_t)p.com);
    if (xQueueSend(s.cmdQ, &p, 0) != pdTRUE) ESP_LOGE(TAG, "[ERR] RX опашката е пълна, командата е изгубена");
  } else if (packetSize == STATE_REQ_WIRE_LEN) {
    // target проверка ПРЕДИ decrypt - общ формат за state request и state-resp ACK
    if (memcmp(buf, cfg->id, MODULE_ID_LEN) != 0) {
      ESP_LOGI(TAG, "[RX STATE] не е за мен, игнориран");
      return;
    }
    uint8_t senderId[CRYPTO_ID_LEN];
    uint8_t plaintext[1];
    uint8_t ptLen;
    if (cryptoParseWirePacket(buf + MODULE_ID_LEN, CRYPTO_OVERHEAD, cfg->key, CRYPTO_TYPE_STATE_REQ, senderId, plaintext, &ptLen)) {
      ESP_LOGI(TAG, "[RX STATE_REQ] за мен, изпращам състояния");
      sendStateResponseWire(CRYPTO_TYPE_STATE_RESP);
    } else if (cryptoParseWirePacket(buf + MODULE_ID_LEN, CRYPTO_OVERHEAD, cfg->key, CRYPTO_TYPE_STATE_RESP_ACK, senderId,
                                     plaintext, &ptLen)) {
      if (s.restartPending) {
        ESP_LOGI(TAG, "[RX STATE_RESP_ACK] потвърдено от Gateway, спирам retry-тата");
        s.restartPending = false;
      }
    } else {
      ESP_LOGW(TAG, "[RX STATE] невалиден MAC, отхвърлен");
    }
  } else {
    ESP_LOGI(TAG, "[RX] непознат размер=%" PRId32, packetSize);
  }
}

static void rxDrain() {
  uint8_t buf[EXEC_RX_BUF];
  for (int32_t i = 0; i < MAX_RX_PER_SERVICE; i++) {
    int32_t n = radio_read_packet(buf, sizeof(buf));
    if (n == RADIO_RX_NONE) break;
    if (n == RADIO_RX_CRC_ERROR) { ESP_LOGD(TAG, "[RX] пакет с лош CRC, изхвърлен"); continue; }
    rxHandle(buf, n);
  }
  // DIO1 пада, когато IRQ флаговете са изчистени (radio_read_packet) - сега прекъсването може да се върне
  hal_irq_rearm(LORA_DIO1);
}

// ---------------- Една обиколка и срок до следващата ----------------
static uint32_t nextWaitMs() {
  uint32_t now = hal_millis();
  uint32_t best = EXEC_WAIT_FOREVER;
  auto consider = [&](uint32_t at) {
    uint32_t d = reached(now, at) ? 0 : (uint32_t)(at - now);
    if (d < best) best = d;
  };
  if (s.txPending) {
    consider(now + channelAccessNextMs(&s.txAccess, now));      // следваща проверка на канала; след TX всичко друго се връща в играта
  } else {
    for (uint8_t i = 0; i < s.ackCount; i++) consider(s.acks[i].sendAt);
    if (s.hbPending) consider(s.hbSendAt);
    if (s.restartPending) consider(s.restartSentAt + radioAckTimeoutRestartMs());
  }
  return best;
}

uint32_t exec_radio_service(void) {
  bool progress = false;
  for (int32_t pass = 0; pass < MAX_SERVICE_PASSES; pass++) {
    rxDrain();
    drainAckIn();
    bool p = false;
    p |= restartRespManager();
    p |= ackManager();
    p |= heartbeatTick();
    p |= radioTxTick();
    progress = p;
    if (!p) break;          // нищо не се е променило - спокойно състояние
  }
  uint32_t w = nextWaitMs();
  if (w == 0 && !progress) w = 1;       // срок, който не може да се изпълни сега (напр. чака слота) - не въртим в празно
  return w;
}

// ---------------- Задачата ----------------
// Еднократен таймер, който сам се пререгистрира: първият срок е на случайна фаза (виж HB_FIRST_MIN_MS в protocol.h)
static void hbTimerCb(void *) {
  exec_radio_hb_due();
  if (s_hbTimer) esp_timer_start_once(s_hbTimer, (uint64_t)HB_INTERVAL_MS * UINT64_C(1000));
}

void exec_radio_task(void *) {
  s.task = xTaskGetCurrentTaskHandle();

  esp_task_wdt_add(nullptr);   // от тук задачата трябва да "храни" watchdog-а (мъртъв радио чип -> рестарт след 30 s)
  RadioParams p;
  p.freqHz = s.cfg->freqHz;
  p.sf = s.cfg->sf;
  p.bwHz = s.cfg->bwHz;
  p.txPowerDbm = s.cfg->txPowerDbm;
  radio_setup(&p, 0);          // докато успее (постоянно захранено устройство)
  radio_listen();
  if (!hal_irq_attach(LORA_DIO1, s.task)) hal_fatal_restart("прекъсването на DIO1 не се закачи");

  esp_timer_create_args_t ta = {};
  ta.callback = hbTimerCb;
  ta.name = "exec_hb";
  if (esp_timer_create(&ta, &s_hbTimer) == ESP_OK) {
    esp_timer_start_once(s_hbTimer, (uint64_t)hal_random_range(HB_FIRST_MIN_MS, HB_INTERVAL_MS + 1) * UINT64_C(1000));
  }

  // Специален state response веднага след boot - маркира началото на живота на този firmware инстанс
  // (консуматорите бяха принудително изключени). Gateway трябва да го потвърди с ACK; ако не отговори до
  // 3 опита, Executor се отказва мълчаливо.
  exec_radio_request_restart_response();

  for (;;) {
    HAL_STACK_PROBE();
    uint32_t w = exec_radio_service();
    ESP_LOGD(TAG, "следващо събуждане след %" PRIu32 " ms", (uint32_t)w);
    if (w > AGRO_TASK_WAKE_MAX_MS) w = AGRO_TASK_WAKE_MAX_MS;   // task watchdog: събужда се поне веднъж на 5 s
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(w));
    esp_task_wdt_reset();
  }
}
