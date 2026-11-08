# -*- coding: utf-8 -*-
"""Мутационна проверка на тестовете на ESP-IDF прошивките.

На ВРЕМЕННО копие на firmware_src/ESP32_PCB(temp_hum)_ESPIDF се въвежда по една умишлена грешка и
съответните тестове (run_espidf.sh) трябва да се ПРОВАЛЯТ. Ако някоя мутация не се улови, значи тестовете не
пазят това поведение. Реалните файлове не се пипат.

    python mutation_check.py [филтър-по-описание ...]
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def find_bash():
    """Git Bash, не WSL bash.exe от System32 (той няма дистрибуция и тихо 'провали' всяко пускане)"""
    env = os.environ.get("MUT_BASH")
    cands = [env] if env else []
    cands += [r"C:\Program Files\Git\bin\bash.exe", r"C:\Program Files\Git\usr\bin\bash.exe",
              r"C:\Program Files (x86)\Git\bin\bash.exe", "/usr/bin/bash", "/bin/bash"]
    for c in cands:
        if c and os.path.exists(c):
            return c
    return "bash"


BASH = find_bash()
FW = os.path.normpath(os.path.join(HERE, "..", "..", "..", "firmware_src", "ESP32_PCB(temp_hum)_ESPIDF"))

C = "components/"
A = "apps/"

# (описание, файл, старо, ново, тестове)
M = [
    # ------------------------------------------------------------------ крипто / протокол
    ("crypto: типът не влиза в nonce", C + "agro_core/crypto_common.cpp", "nonce16[0] = typeId;", "nonce16[0] = 0;", "core"),
    ("crypto: тагът се сравнява само по 3 байта", C + "agro_core/crypto_common.cpp", "for (int i = 0; i < CRYPTO_TAG_LEN; i++) diff", "for (int i = 0; i < CRYPTO_TAG_LEN - 1; i++) diff", "core"),
    ("crypto: nonce не е в MAC входа", C + "agro_core/crypto_common.cpp", "memcpy(macIn, nonce16, 16);", "memset(macIn, 0, 16);", "core"),
    ("crypto: counter в nonce не е big-endian", C + "agro_core/crypto_common.cpp", "nonce16[7]  = (uint8_t)(counter >> 24);", "nonce16[7]  = (uint8_t)(counter);", "core"),
    # ------------------------------------------------------------------ ceiling / nonce
    ("ceiling: watermark се пише на всяка стойност", C + "agro_core/ceiling_counter.cpp", "uint32_t newCeiling = current + CEILING_COMMIT_INTERVAL;", "uint32_t newCeiling = current + 1;", "core"),
    ("ceiling: RTC състоянието не се ползва", C + "agro_core/ceiling_counter.cpp", "rtcMagic == CEIL_RTC_MAGIC", "false", "core"),
    ("ceiling: провал на NVS не спира излъчването", C + "agro_core/ceiling_counter.cpp", "if (!s.open(NVS_NAMESPACE, false) || !s.setU32(key, newCeiling) || !s.commit()) {", "if (false) {", "core"),
    ("ceiling: стойността преди ceiling, не след", C + "agro_core/ceiling_counter.cpp", "if (current >= committed) {", "if (current > committed) {", "core"),
    # ------------------------------------------------------------------ JSON (cJSON + обвивка) / конфигурация
    ("json: твърде дълга стойност се реже тихо", C + "agro_core/json_util.cpp", "if (n >= cap) return JSON_FIELD_TOO_LONG;", "if (false) return JSON_FIELD_TOO_LONG;", "core"),
    ("json: остатък след документа се приема", C + "agro_core/json_util.cpp", "if (end != stop || !cJSON_IsObject(r)) {", "if (!cJSON_IsObject(r)) {", "core"),
    ("json: не-обект се приема за документ", C + "agro_core/json_util.cpp", "if (end != stop || !cJSON_IsObject(r)) {", "if (end != stop) {", "core"),
    ("json: вложеността не се ограничава", C + "agro_core/json_util.cpp", "if (!depthOk(text, len, JSON_MAX_DEPTH)) return;", "", "core gateway"),
    ("json: скоби в низ се броят за вложеност", C + "agro_core/json_util.cpp", "    if (c == '\"') inStr = true;\n", "", "core"),
    ("json: ключовете без различаване на малки/главни", C + "agro_core/json_util.cpp", "cJSON_GetObjectItemCaseSensitive(obj, key)", "cJSON_GetObjectItem(obj, key)", "core"),
    ("json: fixed2 без две десетични", C + "agro_core/json_util.cpp", "\"%s%ld.%02ld\"", "\"%s%ld.%ld\"", "core gateway"),
    ("json: препълване на буфера не се докладва", C + "agro_core/json_util.cpp", "if (!cJSON_PrintPreallocated(root_, buf, (int)cap, 0)) { buf[0] = 0; return false; }", "cJSON_PrintPreallocated(root_, buf, (int)cap, 0);", "core"),
    ("json: JsonOut не освобождава дървото", C + "agro_core/json_util.cpp", "JsonOut::~JsonOut() { cJSON_Delete(root_); }", "JsonOut::~JsonOut() {}", "core"),
    ("json: JsonDoc не освобождава дървото", C + "agro_core/json_util.cpp", "JsonDoc::~JsonDoc() { cJSON_Delete(root_); }", "JsonDoc::~JsonDoc() {}", "core"),
    ("json: провал на памет се игнорира при печат", C + "agro_core/json_util.cpp", "if (!ok() || cap < 3 || depth_ != 0) return false;", "if (cap < 3) return false;", "core gateway"),
    ("cfg: не се валидира целият JSON", C + "agro_cfg/cfg_parse.cpp", "if (!doc.valid()) {", "if (false) {", "core"),
    ("cfg: няма проверка на GPIO", C + "agro_cfg/cfg_parse.cpp", "if (!board_gpio_usable((int)pin)) {", "if (false) {", "core"),
    ("cfg: няма commit", C + "agro_cfg/cfg_parse.cpp", "ok &= s.commit();", "", "core"),
    ("cfg: неуспешен запис се потвърждава с ACK", C + "agro_cfg/cfg_parse.cpp", "if (!ok) { nack(reply, \"запис в NVS се провали\"); return; }", "", "core"),
    ("cfg: мощността не се проверява за граници", C + "agro_cfg/cfg_parse.cpp", "parseSInt(txpStr, LORA_TX_POWER_MIN_DBM, LORA_TX_POWER_MAX_DBM, &txp)", "parseSInt(txpStr, -127, 127, &txp)", "core"),
    ("cfg: мощността не се пази със знак", C + "agro_cfg/cfg_parse.cpp", "s.setU8(\"txp\", (uint8_t)(int8_t)txp)", "s.setU8(\"txp\", (uint8_t)(txp < 0 ? 0 : txp))", "core"),
    ("dedup: дължината не се сравнява", C + "agro_core/include/dedup.h", "if (len_[i] == len && memcmp", "if (memcmp", "core"),
    ("hal: random извън обхвата", C + "agro_hal/hal.cpp", "esp_random() % max_excl", "esp_random() % (max_excl + 1)", "core"),
    ("consumers: изходите стартират включени", C + "agro_core/consumers.cpp", "hal_gpio_output(cs->pin[i], false);", "hal_gpio_output(cs->pin[i], true);", "executor"),
    ("consumers: няма проверка на използваем пин", C + "agro_core/consumers.cpp", "if (!board_gpio_usable(cs->pin[i])) {", "if (false) {", "executor"),
    # ------------------------------------------------------------------ времена на радиото / достъп до канала
    ("radio_timing: прозорец за сондиране по малък пакет", C + "agro_core/radio_timing.cpp", "return radioToaMs(RADIO_MAX_PAYLOAD_LEN); }", "return radioToaMs(RADIO_TELEMETRY_LEN); }", "core"),
    ("radio_timing: LDRO не се включва на SF11/BW125", C + "agro_core/radio_timing.cpp", "return radioSymbolUs() > 16000UL;", "return radioSymbolUs() > 17000UL;", "radio"),
    ("radio_timing: CAD таймаутът без запас от символи", C + "agro_core/radio_timing.cpp", "(uint32_t)(RADIO_CAD_SYMBOLS + 3) * radioSymbolUs()", "(uint32_t)(RADIO_CAD_SYMBOLS) * radioSymbolUs()", "core radio"),
    ("radio_timing: CAD символите са 4, а чипът прави 2", C + "agro_core/include/radio_timing.h", "#define RADIO_CAD_SYMBOLS       2", "#define RADIO_CAD_SYMBOLS       4", "radio"),
    ("channel_access: SKIP политиката предава", C + "agro_core/channel_access.cpp", "return ca->policy == CH_POLICY_TELEMETRY_FORCE ? CH_CLEAR : CH_GIVEUP;", "return CH_CLEAR;", "core"),
    # ------------------------------------------------------------------ радио слой (RadioLib през agro_radio)
    ("radio: мощността не се ограничава до максимума", C + "agro_radio/agro_radio.cpp", "if (dbm > LORA_TX_POWER_MAX_DBM) return (int8_t)LORA_TX_POWER_MAX_DBM;", "", "radio"),
    ("radio: мощността не се подава на чипа", C + "agro_radio/agro_radio.cpp", "cfg.power = power;", "cfg.power = 10;", "radio"),
    ("radio: ограничението по ток остава 60 mA", C + "agro_radio/agro_radio.cpp", "if (radio.setCurrentLimit(LORA_CURRENT_LIMIT_MA) != RADIOLIB_ERR_NONE)", "if (false)", "radio"),
    ("radio: грешен sync word", C + "agro_radio/agro_radio.cpp", "cfg.syncWord = LORA_SYNC_WORD;", "cfg.syncWord = 0x34;", "radio"),
    ("radio: честотата не е в MHz", C + "agro_radio/agro_radio.cpp", "cfg.frequency = (float)p->freqHz / 1.0e6f;", "cfg.frequency = (float)p->freqHz / 1.0e3f;", "radio"),
    ("radio: грешна широчина на лентата", C + "agro_radio/agro_radio.cpp", "cfg.bandwidth = (float)radioBw() / 1000.0f;", "cfg.bandwidth = (float)radioBw() / 500.0f;", "radio"),
    ("radio: грешен спред фактор", C + "agro_radio/agro_radio.cpp", "cfg.spreadingFactor = radioSf();", "cfg.spreadingFactor = 9;", "radio"),
    ("radio: стандартният reset на RadioLib (до 10 s при мъртъв чип)", C + "agro_radio/agro_radio.cpp", "radio.resetOnStartup = false;", "radio.resetOnStartup = true;", "radio sensor"),
    ("radio: таймаутът на BUSY остава 1 s", C + "agro_radio/agro_radio.cpp", "s_module.spiConfig.timeout = RADIO_SPI_BUSY_TIMEOUT_MS;", "", "radio"),
    ("radio: повторният setup не рестартира чипа", C + "agro_radio/agro_radio.cpp", "    radio.reset(false);                        // RST импулс; без чакане за standby\n", "", "radio"),
    ("radio_listen: прекъсването не се връща", C + "agro_radio/agro_radio.cpp", "  hal_irq_rearm(LORA_DIO1);                    // startReceive изчиства", "  (void)0;                    // startReceive изчиства", "radio executor"),
    ("radio_read_packet: пакет с лош CRC се приема", C + "agro_radio/agro_radio.cpp", "if (st == RADIOLIB_ERR_CRC_MISMATCH) return RADIO_RX_CRC_ERROR;", "if (st == RADIOLIB_ERR_CRC_MISMATCH) return (int)len;", "radio gateway"),
    ("radio_read_packet: чете повече от буфера", C + "agro_radio/agro_radio.cpp", "radio.readData(buf, len < cap ? len : cap)", "radio.readData(buf, len)", "radio"),
    ("radio_read_packet: празен пакет оставя прекъсването", C + "agro_radio/agro_radio.cpp", "    radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);          // DIO1 трябва да падне, иначе прекъсването се повтаря\n    return RADIO_RX_NONE;", "    return RADIO_RX_NONE;", "radio"),
    ("radio_transmit: прекъсването на DIO1 не се изключва", C + "agro_radio/agro_radio.cpp", "  hal_irq_disable(LORA_DIO1);                  // TX_DONE вдига DIO1; края на предаването го следим сами\n", "", "radio"),
    ("radio_transmit: пакетът се пише без standby", C + "agro_radio/agro_radio.cpp", "  int st = radio.standby();\n  if (st == RADIOLIB_ERR_NONE) st = radio.startTransmit(buf, len);", "  int st = radio.startTransmit(buf, len);", "radio"),
    ("radio_transmit: няма срок за края на предаването", C + "agro_radio/agro_radio.cpp", "      if (hal_millis() - start > deadline) break;\n", "", "radio"),
    ("radio_transmit: няма finishTransmit", C + "agro_radio/agro_radio.cpp", "  int fin = radio.finishTransmit();", "  int fin = RADIOLIB_ERR_NONE;", "radio"),
    ("radio_transmit: дължина 256 се приема", C + "agro_radio/agro_radio.cpp", "if (buf == nullptr || len == 0 || len > 255) return false;", "if (buf == nullptr || len == 0) return false;", "radio"),
    ("channelActive: прекъсването на DIO1 не се изключва", C + "agro_radio/agro_radio.cpp", "  hal_irq_disable(LORA_DIO1);                  // CAD вдига DIO1", "  (void)0;                  // CAD вдига DIO1", "radio"),
    ("channelActive: флаговете не се чистят", C + "agro_radio/agro_radio.cpp", "  radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);\n  if (!done) {", "  if (!done) {", "radio"),
    ("channelActive: резултатът е обърнат", C + "agro_radio/agro_radio.cpp", "return r == RADIOLIB_LORA_DETECTED;", "return r != RADIOLIB_LORA_DETECTED;", "radio"),
    ("channelActive: няма срок за CAD", C + "agro_radio/agro_radio.cpp", "    if (hal_millis() - start > timeoutMs) break;\n", "", "radio"),
    ("channelActive: провал на CAD се смята за зает канал", C + "agro_radio/agro_radio.cpp", "считам канала за свободен\", st);\n    radio.standby();\n    return false;", "считам канала за свободен\", st);\n    radio.standby();\n    return true;", "radio"),
    ("radio_set_frequency: без standby", C + "agro_radio/agro_radio.cpp", "  int st = radio.standby();                    // честотата се сменя в standby\n  if (st == RADIOLIB_ERR_NONE) st = radio.setFrequency((float)hz / 1.0e6f);", "  int st = radio.setFrequency((float)hz / 1.0e6f);", "radio"),
    # ------------------------------------------------------------------ мрежа
    ("wifi: power save е включен", C + "agro_net/wifi_link.cpp", "esp_wifi_set_ps(WIFI_PS_NONE);", "esp_wifi_set_ps(WIFI_PS_MIN_MODEM);", "net"),
    ("mqtt: clean session е изключена", C + "agro_net/mqtt_link.cpp", "mc.session.disable_clean_session = false;", "mc.session.disable_clean_session = true;", "net"),
    ("mqtt: абонаментите не се подновяват", C + "agro_net/mqtt_link.cpp", "for (int i = 0; i < s_cfg.subCount; i++) esp_mqtt_client_subscribe", "for (int i = 0; i < 0; i++) esp_mqtt_client_subscribe", "net"),
    ("mqtt: publish без връзка отива в outbox", C + "agro_net/mqtt_link.cpp", "if (s_client == nullptr || !s_connected.load()) return false;", "if (s_client == nullptr) return false;", "net"),
    ("mqtt: темата не е NUL-терминирана", C + "agro_net/mqtt_link.cpp", "topic[ev->topic_len] = 0;", "", "net"),
    ("mqtt: няма рестарт при дълга липса на брокер", C + "agro_net/mqtt_link.cpp", "} else if ((uint32_t)(now - s_downSinceMs.load()) >= MQTT_RESTART_MS) {", "} else if (false) {", "net"),
    # ------------------------------------------------------------------ sensor
    ("sensor: сън 100 вместо 113 цикъла", A + "sensor/main/app_main.cpp", "#define SLEEP_CYCLES  113", "#define SLEEP_CYCLES  100", "sensor"),
    ("sensor: CRC на SHT31 не се проверява", A + "sensor/main/sensors_io.cpp", "if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return false;", "", "sensor"),
    ("sensor: няма диапазонна проверка на SHT31", A + "sensor/main/sensors_io.cpp", "if (t < SHT31_TEMP_MIN_C || t > SHT31_TEMP_MAX_C) return false;", "", "sensor"),
    ("sensor: и двата сензора на шината на почвата", A + "sensor/main/sensors_io.cpp", "{\"въздух\", I2C_AIR_PORT, I2C_AIR_SDA, I2C_AIR_SCL, SHT31_AIR_ADDR, nullptr, nullptr, false}", "{\"въздух\", I2C_SOIL_PORT, I2C_SOIL_SDA, I2C_SOIL_SCL, SHT31_AIR_ADDR, nullptr, nullptr, false}", "sensor"),
    ("sensor: шините не се освобождават", A + "sensor/main/app_main.cpp", "  sensors_deinit();                       // шините не са нужни повече; радиото и сънят следват\n", "", "sensor"),
    ("sensor: няма soft reset на SHT31", A + "sensor/main/sensors_io.cpp", "  i2c_master_transmit(c.dev, reset, sizeof(reset), I2C_TIMEOUT_MS);   // липсващ сензор не е грешка тук\n", "", "sensor"),
    ("sensor: измерването не се изчаква", A + "sensor/main/sensors_io.cpp", "  hal_delay_ms(SHT31_MEASURE_MS);\n", "", "sensor"),
    ("sensor: двете стойности се четат от един сензор", A + "sensor/main/sensors_io.cpp", "if (channelRead(s_ch[SENSOR_CH_AIR], &t, &h)) {", "if (channelRead(s_ch[SENSOR_CH_SOIL], &t, &h)) {", "sensor"),
    ("sensor: радиото не се приспива", A + "sensor/main/sensor_tx.cpp", "  radio.sleep(false);", "", "sensor"),
    ("sensor: предава без ключ", A + "sensor/main/sensor_config.cpp", "c->hasKey = s.getBlob(\"key\", c->key, CRYPTO_KEY_LEN) == CRYPTO_KEY_LEN;", "c->hasKey = true;", "sensor"),
    ("sensor: мощността не се чете от NVS", A + "sensor/main/sensor_config.cpp", "c->txPowerDbm = (int8_t)s.getU8(\"txp\", (uint8_t)LORA_TX_POWER_DBM);", "", "sensor"),
    ("sensor: безкраен опит за радиото", A + "sensor/main/sensor_tx.cpp", "#define SENSOR_RADIO_TRIES 5", "#define SENSOR_RADIO_TRIES 0", "sensor"),
    ("sensor: компресия x10", C + "agro_core/include/protocol.h", "(int16_t)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f))", "(int16_t)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f))", "sensor"),
    # ------------------------------------------------------------------ executor
    ("executor: 5 опита за restart отговора", A + "executor/main/exec_radio.cpp", "#define STATE_RESP_MAX_RETRIES 2", "#define STATE_RESP_MAX_RETRIES 4", "executor"),
    ("executor: приема команди за други модули", A + "executor/main/exec_radio.cpp", "if (memcmp(buf, cfg->id, MODULE_ID_LEN) != 0) {", "if (false) {", "executor"),
    ("executor: ACK с type на heartbeat", A + "executor/main/exec_radio.cpp", "CRYPTO_TYPE_EXEC_ACK, (const uint8_t *)s.cfg->id", "CRYPTO_TYPE_EXEC_HB, (const uint8_t *)s.cfg->id", "executor"),
    ("executor: ACK без пауза", A + "executor/main/exec_cmd.cpp", "r.sendAt = hal_millis() + RADIO_EXEC_ACK_DELAY_MS;", "r.sendAt = hal_millis();", "executor"),
    ("executor: отхвърлена команда се потвърждава с ACK", A + "executor/main/exec_cmd.cpp", "p.status = STATUS_NACK;", "p.status = STATUS_ACK;", "executor"),
    ("executor: състоянието не е от пина", A + "executor/main/exec_radio.cpp", "plaintext[off + 4] = consumers_state(&cfg->consumers, i) ? 1 : 0;", "plaintext[off + 4] = 0;", "executor"),
    ("executor: радиото не се връща на слушане след TX", A + "executor/main/exec_radio.cpp", "    bool ok = radio_transmit(s.txBuf, s.txLen);\n    radio_listen();", "    bool ok = radio_transmit(s.txBuf, s.txLen);", "executor"),
    ("executor: мощността не се подава на радиото", A + "executor/main/exec_radio.cpp", "  p.txPowerDbm = s.cfg->txPowerDbm;\n", "  p.txPowerDbm = LORA_TX_POWER_DBM;\n", "executor"),
    ("executor: прекъсването не се връща след четене", A + "executor/main/exec_radio.cpp", "  hal_irq_rearm(LORA_DIO1);\n}\n\n// ---------------- Една обиколка", "}\n\n// ---------------- Една обиколка", "executor"),
    ("executor: автоматичният лек сън не е включен", A + "executor/main/app_main.cpp", "power_enable_auto_light_sleep(LORA_DIO1);", "", "executor"),
    ("executor: радиото на ядро 0", A + "executor/main/app_main.cpp", "\"exec_radio\", 5120, nullptr, 6, &radioTask, 1)", "\"exec_radio\", 5120, nullptr, 6, &radioTask, 0)", "executor"),
    ("executor: опашката ACK не буди радиото", A + "executor/main/exec_cmd.cpp", "if (s_radioTask) xTaskNotifyGive(s_radioTask);", "", "executor"),
    # ------------------------------------------------------------------ repeater
    ("repeater: предава на RX честотата", A + "repeater/main/rep_radio.cpp", "  radio_set_frequency((long)s_cfg->freqTxHz);    // изрично", "  radio_set_frequency((long)s_cfg->freqRxHz);    // изрично", "repeater"),
    ("repeater: няма dedup", A + "repeater/main/rep_radio.cpp", "if (s_dedup.seen(buf, (uint8_t)i)) {", "if (false) {", "repeater"),
    ("repeater: HB с type на executor", A + "repeater/main/rep_radio.cpp", "CRYPTO_TYPE_REPEATER_HB", "CRYPTO_TYPE_EXEC_HB", "repeater"),
    ("repeater: няма rearm на прекъсването", A + "repeater/main/rep_radio.cpp", "  hal_irq_rearm(LORA_DIO1);                      // DIO1 пада", "  (void)0;                      // DIO1 пада", "repeater"),
    ("repeater: рх задачата не е с по-висок приоритет", A + "repeater/main/app_main.cpp", "\"rep_rx\", 4096, nullptr, 6, nullptr, 1)", "\"rep_rx\", 4096, nullptr, 4, nullptr, 1)", "repeater"),
    ("repeater: мощността не се чете от NVS", A + "repeater/main/rep_config.cpp", "c->txPowerDbm = (int8_t)s.getU8(\"txp\", (uint8_t)LORA_TX_POWER_DBM);", "", "repeater"),
    # ------------------------------------------------------------------ gateway
    ("gateway: няма dedup", A + "gateway/main/gw_radio.cpp", "if (s_dedup.seen(buf, (uint8_t)len)) {", "if (false) {", "gateway"),
    ("gateway: втора команда не се игнорира", A + "gateway/main/gw_logic.cpp", "  if (s_cmdPending) {\n    ESP_LOGW(TAG, \"[WARN] команда вече чака", "  if (false) {\n    ESP_LOGW(TAG, \"[WARN] команда вече чака", "gateway"),
    ("gateway: ACK се приема без проверка на C_ID", A + "gateway/main/gw_logic.cpp", "memcmp(m.cId, s_pC, 4) == 0 &&", "true &&", "gateway"),
    ("gateway: всичко се публикува retained", A + "gateway/main/gw_logic.cpp", "if (!mqtt_link_publish(topic, buf, len, qos, false)) {", "if (!mqtt_link_publish(topic, buf, len, qos, true)) {", "gateway"),
    ("gateway: няма ACK за restart state response", A + "gateway/main/gw_logic.cpp", "if (m.restart) sendStateRespAck(m.id);", "if (false) sendStateRespAck(m.id);", "gateway"),
    ("gateway: 5 повторения вместо 2", A + "gateway/main/gw_logic.h", "#define MAX_RETRIES              2", "#define MAX_RETRIES              5", "gateway"),
    ("gateway: 'няма данни' се дели на 100", A + "gateway/main/gw_logic.cpp", "if (raw == SENSOR_RAW_NODATA) w.num", "if (false) w.num", "gateway"),
    ("gateway: радиото на ядро 0", A + "gateway/main/app_main.cpp", "\"gw_radio\", 5120, nullptr, 10, &radioTask, 1)", "\"gw_radio\", 5120, nullptr, 10, &radioTask, 0)", "gateway"),
    ("gateway: радио приоритет под логиката", A + "gateway/main/app_main.cpp", "\"gw_radio\", 5120, nullptr, 10, &radioTask, 1)", "\"gw_radio\", 5120, nullptr, 4, &radioTask, 1)", "gateway"),
    ("gateway: абониране с QoS 0", A + "gateway/main/app_main.cpp", "mc.subQos = 1;", "mc.subQos = 0;", "gateway"),
    ("gateway: радио задачата не храни Task WDT", A + "gateway/main/gw_radio.cpp", "  esp_task_wdt_reset();                          // признак на живот към Task Watchdog (не е грешка, ако задачата не е абонирана)\n", "", "gateway"),
    ("gateway: радио задачата не е абонирана за Task WDT", A + "gateway/main/gw_radio.cpp", "  esp_task_wdt_add(nullptr);", "  (void)0;", "gateway"),
    ("gateway: логиката не храни Task WDT", A + "gateway/main/gw_logic.cpp", "    esp_task_wdt_reset();                        // признак на живот към Task Watchdog\n", "", "gateway"),
    ("gateway: Task WDT без рестарт (само предупреждение)", A + "gateway/main/app_main.cpp", "  cfg.trigger_panic = true;", "  cfg.trigger_panic = false;", "gateway"),
    ("gateway: Task WDT 300 s вместо 30 s", A + "gateway/main/app_main.cpp", "#define GW_TASK_WDT_TIMEOUT_MS 30000UL", "#define GW_TASK_WDT_TIMEOUT_MS 300000UL", "gateway"),
    ("gateway: Task WDT не се настройва", A + "gateway/main/app_main.cpp", "  configureTaskWdt();\n", "", "gateway"),
    ("gateway: общ счетчик не е за всички пакети", A + "gateway/main/gw_logic.cpp", "if (!s_counterReady) { s_cmdCounter.begin(\"cmdceil\"); s_counterReady = true; }", "s_cmdCounter.begin(\"cmdceil\");", "gateway"),
    ("gateway: мощността не се чете от NVS", A + "gateway/main/gw_config.cpp", "c->txPowerDbm = (int8_t)s.getU8(\"txp\", (uint8_t)LORA_TX_POWER_DBM);", "", "gateway"),
    ("gateway: ts е в милисекунди", A + "gateway/main/gw_radio.cpp", "m.tsSec = hal_millis() / 1000;", "m.tsSec = hal_millis();", "gateway"),
    ("gateway: state request се игнорира при чакаща команда", A + "gateway/main/gw_logic.cpp", "  if (s_stateReqPending) {\n    ESP_LOGW(TAG, \"[WARN] state request вече чака", "  if (false) {\n    ESP_LOGW(TAG, \"[WARN] state request вече чака", "gateway"),
    ("gateway: невалиден JSON от MQTT се изпълнява", A + "gateway/main/gw_logic.cpp", "  JsonDoc doc(m.payload, m.payloadLen);\n  bool okM", "  JsonDoc doc(\"{\\\"M_ID\\\":\\\"E007\\\",\\\"C_ID\\\":\\\"V01\\\",\\\"com\\\":\\\"A1\\\"}\", 38);\n  bool okM", "gateway"),
    # ------------------------------------------------------------------ executor_wifi
    ("executor_wifi: няма филтър по M_ID", A + "executor_wifi/main/ew_logic.cpp", "if (strcmp(m_id, s_cfg->id) != 0) return;", "", "executor_wifi"),
    ("executor_wifi: restart state response при всяко свързване", A + "executor_wifi/main/ew_logic.cpp", "if (!s_restartStateSent) {", "if (true) {", "executor_wifi"),
    ("executor_wifi: 'online' не е retained", A + "executor_wifi/main/ew_logic.cpp", "mqtt_link_publish(topic, msg, strlen(msg), 1, true);", "mqtt_link_publish(topic, msg, strlen(msg), 1, false);", "executor_wifi"),
    ("executor_wifi: абониране с QoS 0", A + "executor_wifi/main/app_main.cpp", "mc.subQos = 1;", "mc.subQos = 0;", "executor_wifi"),
    ("executor_wifi: заявката за състояние се третира като команда", A + "executor_wifi/main/ew_logic.cpp", "m.kind = (m.com == CMD_STATE_REQ) ? EW_MSG_STATE_REQ : EW_MSG_COMMAND;", "m.kind = EW_MSG_COMMAND;", "executor_wifi"),
    ("executor_wifi: състоянието не е от пина", A + "executor_wifi/main/ew_logic.cpp", "consumers_state(&s_cfg->consumers, i) ? \"ON\" : \"OFF\"", "\"OFF\"", "executor_wifi"),
    ("executor_wifi: изпълнението на ядро 0", A + "executor_wifi/main/app_main.cpp", "\"ew_exec\", 4096, nullptr, 5, nullptr, 1)", "\"ew_exec\", 4096, nullptr, 5, nullptr, 0)", "executor_wifi"),
    # ------------------------------------------------------------------ config
    ("config: READY не се праща", A + "config/main/config_app.cpp", "io->print(\"READY\");", "", "config"),
    ("config: NACK приключва прозореца", A + "config/main/config_app.cpp", "io->print(out);\n  }\n  io->print(\"NACK:Timeout", "io->print(out);\n    return CONFIG_TIMED_OUT;\n  }\n  io->print(\"NACK:Timeout", "config"),
    # ------------------------------------------------------------------ достъп до канала / heartbeat
    ("channel_access: PERSIST се отказва вместо да предаде", C + "agro_core/channel_access.cpp", "return ca->policy == CH_POLICY_TELEMETRY_PERSIST ? CH_CLEAR : CH_GIVEUP;", "return CH_GIVEUP;", "core sensor"),
    ("channel_access: PERSIST не сондира (като FORCE)", C + "agro_core/channel_access.cpp", "if (ca->policy == CH_POLICY_COMMAND || ca->policy == CH_POLICY_TELEMETRY_PERSIST) {", "if (ca->policy == CH_POLICY_COMMAND) {", "core"),
    ("sensor: FORCE вместо PERSIST", A + "sensor/main/sensor_tx.cpp", "channelAccessStart(&ca, CH_POLICY_TELEMETRY_PERSIST);", "channelAccessStart(&ca, CH_POLICY_TELEMETRY_FORCE);", "sensor"),
    ("repeater: FORCE вместо PERSIST при препращане", A + "repeater/main/rep_radio.cpp", "CH_POLICY_TELEMETRY_SKIP : CH_POLICY_TELEMETRY_PERSIST", "CH_POLICY_TELEMETRY_SKIP : CH_POLICY_TELEMETRY_FORCE", "repeater"),
    ("executor: heartbeat без случайна начална фаза", A + "executor/main/exec_radio.cpp", "hal_random_range(HB_FIRST_MIN_MS, HB_INTERVAL_MS + 1)", "HB_INTERVAL_MS", "executor"),
    ("repeater: heartbeat без случайна начална фаза", A + "repeater/main/rep_radio.cpp", "hal_random_range(HB_FIRST_MIN_MS, HB_INTERVAL_MS + 1)", "HB_INTERVAL_MS", "repeater"),
    ("executor: heartbeat таймерът не се пререгистрира", A + "executor/main/exec_radio.cpp", "  if (s_hbTimer) esp_timer_start_once(s_hbTimer, (uint64_t)HB_INTERVAL_MS * 1000ULL);\n}", "}", "executor"),
    ("radio_timing: слотът не е равен на стъпката", C + "agro_core/radio_timing.cpp", "uint32_t radioSlotMs()          { return radioCadStepMs(); }", "uint32_t radioSlotMs()          { return 1; }", "core"),
    ("radio_timing: ACK таймаутът без паузата на executor-а", C + "agro_core/radio_timing.cpp", "uint32_t base = radioToaMs(RADIO_CMD_WIRE_LEN) + RADIO_EXEC_ACK_DELAY_MS + radioToaMs(RADIO_ACK_WIRE_LEN);", "uint32_t base = radioToaMs(RADIO_CMD_WIRE_LEN) + radioToaMs(RADIO_ACK_WIRE_LEN);", "core"),
    ("channel_access: един слот за backoff", C + "agro_core/channel_access.cpp", "#define CH_BACKOFF_SLOTS 4", "#define CH_BACKOFF_SLOTS 1", "core"),
    ("executor_wifi: Last Will е с retain=0", C + "agro_net/mqtt_link.cpp", "mc.session.last_will.retain = 1;", "mc.session.last_will.retain = 0;", "executor_wifi net"),
]


def run_one(desc, rel, old, new, targets, tmp):
    src = os.path.join(FW, rel)
    text = io.open(src, encoding="utf8").read()
    if old not in text:
        return "НЕ СЕ ПРИЛОЖИ"
    work = os.path.join(tmp, "fw")
    if os.path.exists(work):
        shutil.rmtree(work)
    shutil.copytree(FW, work)
    wf = os.path.join(work, rel)
    io.open(wf, "w", encoding="utf8", newline="\n").write(text.replace(old, new, 1))
    env = dict(os.environ, FW=work.replace("\\", "/"), OUT=os.path.join(tmp, "out").replace("\\", "/"))
    p = subprocess.run([BASH, "run_espidf.sh"] + targets.split(), cwd=HERE, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=1500)
    out = p.stdout + p.stderr
    if p.returncode == 0:
        return "НЕ СЕ УЛАВЯ"
    if "КОМПИЛАЦИЯТА" in out:
        return "КОМПИЛАЦИОННА ГРЕШКА (невалидна мутация)"
    first = [l.strip() for l in out.splitlines() if "[FAIL]" in l or "livelock" in l or "SIM:" in l]
    crash = [m for m in ("Segmentation fault", "bad_alloc", "terminate called", "Aborted", "Access violation") if m in out]
    if not first and crash:
        return "УЛОВЕНА   (срив: %s)" % crash[0]
    if not first and "[PASS]" in out:
        return "УЛОВЕНА   (срив на теста без провален assert, код %d)" % p.returncode      # напр. повредена памет: Windows не печата нищо
    if not first:
        return "ИНФРАСТРУКТУРНА ГРЕШКА (няма провален тест): " + out.strip().splitlines()[-1][:80] if out.strip() else "ИНФРАСТРУКТУРНА ГРЕШКА (празен изход)"
    return "УЛОВЕНА   " + first[0][:110]


def main():
    filt = sys.argv[1:]
    shard = os.environ.get("MUT_SHARD", "0/1").split("/")      # напр. MUT_SHARD=1/4: само всяка 4-та мутация (паралелно пускане)
    sh_i, sh_n = int(shard[0]), int(shard[1])
    tmp = tempfile.mkdtemp(prefix="mut_")      # път БЕЗ интервали (run_espidf.sh не цитира пътищата)
    caught = missed = bad = 0
    try:
        for idx, (desc, rel, old, new, targets) in enumerate(M):
            if idx % sh_n != sh_i:
                continue
            if filt and not any(f in desc for f in filt):
                continue
            r = run_one(desc, rel, old, new, targets, tmp)
            print("%-62s %s" % (desc, r))
            sys.stdout.flush()
            if r.startswith("УЛОВЕНА"):
                caught += 1
            elif r.startswith("КОМПИЛАЦ") or r.startswith("НЕ СЕ ПРИЛОЖИ") or r.startswith("ИНФРАСТР"):
                bad += 1
            else:
                missed += 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\nУловени: %d   Неуловени: %d   Невалидни мутации: %d" % (caught, missed, bad))
    return 0 if missed == 0 and bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
