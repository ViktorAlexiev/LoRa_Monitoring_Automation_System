from engine import Diagram


def build():
    D = Diagram("gateway", 3040)
    L0x, L0w = 30, 400
    L1x, L1w = 570, 760
    L2x, L2w = 1400, 250
    L3x, L3w = 1720, 780
    L4x, L4w = 2580, 430
    D.lane(L0x, L0w, "Услуги и хардуер")
    D.lane(L1x, L1w, "Задача gw_radio (ядро 1, приоритет 10): единственият собственик на радиото")
    D.lane(L2x, L2w, "Опашки")
    D.lane(L3x, L3w, "Задача gw_cmd (ядро 0, приоритет 5): логика, JSON, команди в полет")
    D.lane(L4x, L4w, "Мрежа (ядро 0, системни задачи на IDF)")

    D.stack(L1x, L1w, 70, [
        ("G0", "app_main(): старт на приложението", [
            "1  hal_nvs_init();  gw_config_load(): id, wssid, wpass, mqip, mqport (низ), mquser, mqpass, freq, sf, bw, txp, key",
            "2  няма id, ключ или Wi-Fi+MQTT → return (не стартира)",
            "3  configureTaskWdt(): 30 s, panic = true, idle задачите на двете ядра (reconfigure → при INVALID_STATE init)",
            "4  gwInQ = xQueueCreate(16, GwMsg ≈ 280 B);  radioInQ = xQueueCreate(4, TxReq ≤ 26 B)",
            "5  gw_radio_reset() и gw_logic_reset(): конфигурация и опашки",
            "6  xTaskCreatePinnedToCore(gw_radio_task, 5120 B, приоритет 10, ядро 1)",
            "7  gw_radio_set_task() и gw_logic_set_radio_task(): handle за известия",
            "8  xTaskCreatePinnedToCore(gw_logic_task 'gw_cmd', 6144 B, приоритет 5, ядро 0)",
            "9  mqtt_link_init(): host, порт, потребител; абонаменти commands и module_states_requests (QoS 1); onMessage = gw_mqtt_on_message",
            "10 wifi_link_start(ssid, парола, onGotIp → mqtt_link_network_up)",
        ], "init"),
        ("G1", "gw_radio_task(): старт на задачата", [
            "esp_task_wdt_add(nullptr): от тук нататък трябва да 'храни' watchdog-а",
            "radio_setup(&p, 0): до успех. Мъртъв чип → задачата е абонирана → watchdog рестарт след 30 s",
            "radio_listen(): непрекъснат прием;   hal_irq_attach(DIO1, задачата)",
        ], "fn"),
        ("G2", "Главен цикъл на gw_radio_task", [
            "for (;;) {",
            "    w = gw_radio_service();   if (w > 5000) w = 5000;",
            "    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(w));",
            "}",
            "Събуждат я: DIO1 (ISR), xTaskNotifyGive от gw_cmd (нов TxReq), изтичане (най-много 5 s, за да храни watchdog-а).",
        ], "fn"),
        ("G3", "gw_radio_service(): една обиколка", [
            "for (pass = 0; pass < 8; pass++) {",
            "    ① rxDrain();   ② progress = txTick();   if (!progress) break;",
            "}",
            "esp_task_wdt_reset();                      // 'жива съм'",
            "w = 0xFFFFFFFF;  if (s_txActive) w = channelAccessNextMs(&s_ca, now);",
            "if (w == 0 && !progress) w = 1;   return w;",
        ], "fn"),
        ("G4", "rxDrain()", [
            "до 16 пъти: n = radio_read_packet(buf[220], 220)",
            "RX_NONE → break;   CRC_ERROR → продължава;   иначе → handleIncoming(buf, n)",
            "накрая hal_irq_rearm(DIO1): DIO1 е ниско, прекъсването може да се върне",
        ], "fn", "①"),
        ("G5", "handleIncoming(buf, len): разпознава пакета по дължина", [
            "m.rssi = radio.getRSSI();  m.snrX100 = radio.getSNR() × 100;  m.tsSec = hal_millis() / 1000",
            "len == 22 (СЕНЗОР):",
            "    DedupRing.seen(buf)? → дубликат, игнорира",
            "    Parse(SENSOR) ✓ и ptLen == 8 → DedupRing.add(buf)",
            "    v[i] = int16 little-endian × 4 → GM_SENSOR",
            "len == 20 (ACK): Parse(EXEC_ACK) → cId[4], com, status → GM_ACK",
            "len == 14 (heartbeat или state без консуматори), пробва поред:",
            "    REPEATER_HB → GM_HB_REPEATER;  EXEC_HB → GM_HB_EXEC;",
            "    STATE_RESP → GM_STATE;  STATE_RESP_RESTART → GM_STATE (restart = 1)",
            "len = 14 + 5k, k ≤ 10: Parse(STATE_RESP | STATE_RESP_RESTART) → GM_STATE + state[]",
            "друга дължина → 'UNKNOWN length';   всеки лош MAC → отхвърля",
            "накрая post(m): xQueueSend(gwInQ, 0), пълна → лог, не блокира",
        ], "fn"),
        ("G6", "txTick(): достъп до канала и предаване на команда", [
            "!s_txActive: xQueueReceive(radioInQ, &s_cur, 0)?  → channelAccessStart(&s_ca, COMMAND); s_txActive = true",
            "s_cadRan = false;  r = channelAccessPoll(&s_ca, now, cadProbe, channelRandom)",
            "CH_WAIT: ако е правен CAD → radio_listen();  return false",
            "CH_CLEAR: radio_transmit(s_cur) → radio_listen() → лог;  s_txActive = false;  return true",
            "CH_GIVEUP: radio_listen();  'канала остана зает' (повторение е работа на gw_cmd);  s_txActive = false",
        ], "fsm", "②"),
    ])

    b = D.boxes
    # ---------- lane 0
    D.box("HW", L0x, b["G1"].y, L0w, "SX1262 радио (SPI)", [
        "DIO1 → HIGH при събитие (RxDone, TxDone, CadDone)",
        "BUSY, RST, NSS; SPI2",
        "слуша непрекъснато; gateway е на мрежово захранване, без сън",
    ], "hw")
    D.box("ISR", L0x, b["G2"].y, L0w, "irqIsr(): ISR в IRAM", [
        "gpio_intr_disable(DIO1);  vTaskNotifyGiveFromISR(gw_radio)",
        "portYIELD_FROM_ISR(woken)",
        "ниво HIGH: събитие не се губи;  rearm СЛЕД четене",
    ], "isr")
    D.box("WDT", L0x, b["G3"].y, L0w, "Task Watchdog (ESP-IDF)", [
        "30 s, panic = true → рестарт",
        "абонирани: gw_radio и gw_cmd (esp_task_wdt_add)",
        "всяка обиколка вика esp_task_wdt_reset()",
        "двете задачи се будят поне веднъж на 5 s",
    ], "isr")
    D.box("RAD", L0x, b["G4"].y, L0w, "agro_radio (RadioLib + EspHal)", [
        "radio_setup / radio_listen / radio_read_packet",
        "radio_transmit: standby → startTransmit → чака ToA → finishTransmit",
        "channelActive: CAD 2 символа, poll DIO1",
    ], "svc")
    D.box("DED", L0x, b["G5"].y, L0w, "DedupRing<8, 64>", [
        "кръгов буфер на последните 8 сензорни пакета",
        "сравнява цялото съдържание (байт по байт)",
        "препратен от repeater = идентичен → дубликат",
    ], "svc")
    D.box("CRY", L0x, b["G5"].y + D.boxes["DED"].h + 26, L0w, "crypto_common (mbedTLS)", [
        "Parse: ПЪРВО MAC (constant-time), после AES-128-CTR",
        "Build: nonce[тип, ID, брояч] → CTR → CMAC → 4 B",
    ], "svc")
    D.box("CHA", L0x, b["G6"].y, L0w, "channel_access (автомат)", [
        "FIRST_CAD → SENSING → BACKOFF → CH_CLEAR",
        "COMMAND: срок ToA(64 B), накрая GIVEUP",
        "NextMs = по-ранното от nextAt и deadline",
    ], "svc")

    # ---------- lane 2: опашки
    D.box("QI", L2x, b["G5"].y + 60, L2w, "gwInQ  (16 × GwMsg)", [
        "радио + MQTT → логика",
        "тип: GM_SENSOR, GM_ACK, GM_HB_REPEATER,",
        "GM_HB_EXEC, GM_STATE, GM_MQTT_CMD,",
        "GM_MQTT_STATE_REQ",
        "GwMsg ≈ 280 B (копира се)",
    ], "queue")
    D.box("QR", L2x, b["G6"].y, L2w, "radioInQ  (4 × TxReq)", [
        "логика → радио",
        "{len, buf[26]}",
    ], "queue")

    # ---------- lane 3: gw_cmd
    y3 = 70
    y3 = D.stack(L3x, L3w, y3, [
        ("L1", "gw_logic_task() 'gw_cmd'", [
            "esp_task_wdt_add(nullptr)",
            "for (;;) {",
            "    w = gw_logic_next_wait_ms();",
            "    if (xQueueReceive(gwInQ, &m, ms(w))) gw_logic_handle(m);",
            "    gw_logic_tick();   esp_task_wdt_reset();",
            "}",
            "спи, когато няма работа (най-много до най-близкия срок, ≤ 5 s)",
        ], "fn"),
        ("L2", "gw_logic_handle(m): разпределя по тип", [
            "GM_SENSOR → handleSensor     GM_ACK → handleAck     GM_HB_REPEATER / GM_HB_EXEC → handleHeartbeat",
            "GM_STATE → handleState     GM_MQTT_CMD → handleMqttCommand     GM_MQTT_STATE_REQ → handleMqttStateReq",
        ], "fn"),
        ("L3", "handleSensor(m)", [
            "JsonOut: {id, soil_t, soil_h, air_t, air_h, rssi, snr, ts}",
            "putRaw: −32768 → 'няма данни' като число;  иначе fixed2 = стойност ÷ 100 (два знака)",
            "publish('sensors', QoS 0): загубено показание не е проблем, закъснялото е по-лошо",
        ], "fn"),
        ("L4", "handleAck(m)", [
            "приема САМО ако: cmdPending && id == s_pM && cId == s_pC && com == s_pCom",
            "status = (m.status == 0) ? ACK : NACK → publishCommandStatus() → cmdPending = false",
            "иначе (закъснял или чужд ACK) → игнорира",
        ], "fn"),
        ("L5", "handleHeartbeat(m)", [
            "{id[, rssi, snr за repeater], ts} → publish('heartbeat', QoS 0)",
        ], "fn"),
        ("L6", "handleState(m)", [
            "states[] от m.state: ID(4) + байт → 'ON' / 'OFF';  restart → 'restart': true",
            "publish('module_states_response', QoS 1)",
            "m.restart → sendStateRespAck(id)   (ACK към executor-а, дори MQTT да е пропаднал)",
            "иначе, ако stateReqPending за същия модул → stateReqPending = false",
        ], "fn"),
        ("L7", "handleMqttCommand(m)", [
            "cmdPending → 'нова команда игнорирана' (БЕЗ статус към сървъра)",
            "JsonDoc(payload): M_ID, C_ID, com (jsonGetString; твърде дълго = грешка, не се реже)",
            "startCommand(M_ID, C_ID, strtol(com, 16))",
        ], "fn"),
        ("L8", "handleMqttStateReq(m)", [
            "stateReqPending → игнорира;  JsonDoc: M_ID → startStateRequest(M_ID)",
        ], "fn"),
        ("L9", "startCommand → sendCommandPacket → enqueueTx", [
            "startCommand: копира M_ID, C_ID, com;  sentAt = now;  retries = 0;  cmdPending = true",
            "sendCommandPacket: plaintext = C_ID(4) + com + 0xFF;  ctr = nextCounter('cmdceil')",
            "    wire = [целеви M_ID, 6 B чисто] + Build(GW_CMD, ID на gateway-а, ctr, plaintext) = 26 B",
            "enqueueTx: xQueueSend(radioInQ, TxReq, 0) + xTaskNotifyGive(gw_radio)",
            "sendStateRequestPacket и sendStateRespAck: същото без данни (STATE_REQ / STATE_RESP_ACK) = 20 B",
        ], "fn"),
        ("L10", "gw_logic_tick(): таймаути, повторения, heartbeat", [
            "cmdPending && now − sentAt ≥ radioAckTimeoutCmdMs():",
            "    retries < 2 → retries++, sentAt = now, sendCommandPacket()  (НОВ брояч)",
            "    иначе → publishCommandStatus(STATUS_TIMEOUT);  cmdPending = false",
            "stateReqPending && … ≥ radioStateReqTimeoutMs(): същото; накрая {id, status: 'timeout'}",
            "now − lastHb ≥ 30 s && mqtt_link_connected() → publish('heartbeat', {id, ts})",
        ], "fsm"),
        ("L11", "gw_logic_next_wait_ms()", [
            "най-близкият от: срок на командата, срок на заявката, lastHb + 30 s;  най-много 5000 ms",
        ], "fn"),
    ])
    D.box("L12", L3x, y3, L3w, "Автомат: команда в полет", [
        "ПРАЗНО → (MQTT 'commands') → ИЗПРАТЕНА (retries = 0)",
        "ИЗПРАТЕНА → (ACK/NACK, съвпада) → публикува статус → ПРАЗНО",
        "ИЗПРАТЕНА → (изтече срок, retries < 2) → ПОВТОРЕНИЕ с нов брояч → ИЗПРАТЕНА",
        "ИЗПРАТЕНА → (изтече срок, retries = 2) → публикува TIMEOUT → ПРАЗНО",
        "Докато е ИЗПРАТЕНА, нова команда се игнорира. Същият автомат има и заявката за състояние.",
    ], "fsm")
    y3 = D.boxes["L12"].y + D.boxes["L12"].h + 26
    D.box("L13", L3x, y3, L3w, "publish() и publishCommandStatus()", [
        "publish(topic, JsonOut, buf, cap, qos): w.print() ✗ (памет / не се побира) → нищо не се публикува, никога отрязан JSON",
        "mqtt_link_publish(topic, buf, len, qos, retain = false) ✗ → лог;  функцията е void",
        "publishCommandStatus: {M_ID, C_ID, com 'A1' (%02X), status 0 / 1 / 2}, QoS 1",
    ], "fn")

    # ---------- lane 4: мрежа
    y4 = 70
    y4 = D.stack(L4x, L4w, y4, [
        ("N1", "wifi_link (esp_wifi + esp_netif + esp_event)", [
            "wifi_link_start: netif_init, event loop, default sta, wifi_init, обработчици, STA режим, wifi_start, PS изключен",
            "STA_START → esp_wifi_connect()",
            "STA_DISCONNECTED → s_connected = false, запомня началото, таймер 5 s → connect",
            "GOT_IP → s_connected = true → onGotIp → mqtt_link_network_up()",
            "надзор на 1 s: няма Wi-Fi 2 минути → hal_restart()",
        ], "net"),
        ("N2", "mqtt_link (esp-mqtt, собствена задача)", [
            "mqtt_link_init: конфигурация на клиента, clean session, keepalive 30 s, буфер 1024 B",
            "mqtt_link_network_up(): еднократен esp_mqtt_client_start",
            "CONNECTED → абонира се наново за всяка тема (QoS 1)",
            "DISCONNECTED → s_connected = false",
            "надзор на 1 s: Wi-Fi има, MQTT няма 2 минути → hal_restart()",
            "mqtt_link_publish: няма връзка → false (нищо не се трупа)",
        ], "net"),
        ("N3", "gw_mqtt_on_message(topic, data, len)", [
            "изпълнява се в MQTT задачата, затова е кратка",
            "commands → GM_MQTT_CMD;  module_states_requests → GM_MQTT_STATE_REQ;  друго → игнорира",
            "len > 159 → отхвърля;  копира payload;  xQueueSend(gwInQ, 0)",
        ], "net"),
    ])
    D.box("N4", L4x, y4, L4w, "MQTT брокер и сървър", [
        "подписва се: commands, module_states_requests",
        "получава: sensors (QoS 0), heartbeat (QoS 0), commands_status (QoS 1), module_states_response (QoS 1)",
        "нищо не е retained",
    ], "hw")

    # ---------- стрелки lane 1
    for a, c in [("G0", "G1"), ("G1", "G2"), ("G2", "G3"), ("G3", "G4"), ("G4", "G5"), ("G5", "G6")]:
        D.straight(a, "b", c, "t")

    D.straight("HW", "b", "ISR", "t", label="DIO1", lab_dx=26, lab_dy=0, anchor="start")
    D.straight("ISR", "r", "G2", "l", label="notify", dashed=True, color="#B45309")
    D.straight("WDT", "r", "G3", "l", label="reset", dashed=True, color="#B45309", lab_dy=-6)

    xg = L0x + L0w
    D.h_elbow("G1", "l", "RAD", "r", xg + 22, ta=0.5, tb=0.10, label="radio_setup", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("G4", "l", "RAD", "r", xg + 46, ta=0.5, tb=0.50, label="read_packet", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("G6", "l", "RAD", "r", xg + 70, ta=0.15, tb=0.9, label="transmit / CAD", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("G6", "l", "CHA", "r", xg + 96, ta=0.8, tb=0.5, label="Poll / NextMs", lab_at=2, lab_dx=-4, lab_dy=-5, anchor="end")
    D.h_elbow("G5", "l", "DED", "r", xg + 22, ta=0.12, tb=0.5, label="seen / add", lab_at=2, lab_dx=-4, lab_dy=-5, anchor="end")
    D.h_elbow("G5", "l", "CRY", "r", xg + 46, ta=0.3, tb=0.5, label="Parse", lab_at=2, lab_dx=-4, lab_dy=-5, anchor="end")

    # lane 1 -> lane 2 -> lane 3
    D.straight("G5", "r", "QI", "l", ta=0.5, tb=0.3, label="GwMsg", lab_dy=-6)
    D.straight("QI", "r", "L1", "l", label="xQueueReceive", lab_dy=-6) if False else None
    D.h_elbow("QI", "r", "L1", "l", L2x + L2w + 22, ta=0.3, tb=0.5, label="xQueueReceive", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("L9", "l", "QR", "r", L2x + L2w + 42, ta=0.65, tb=0.5, label="TxReq", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.straight("QR", "l", "G6", "r", label="xQueueReceive", lab_dy=-6)
    D.edge([D.P("L9", "l", 0.9), (L2x + L2w + 62, D.P("L9", "l", 0.9)[1]), (L2x + L2w + 62, b["G2"].y + 40), D.P("G2", "r", 0.3)],
           label="xTaskNotifyGive", dashed=True, color="#B45309", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")

    # lane 3 вътре
    D.straight("L1", "b", "L2", "t")
    # lane 3 -> lane 4
    qi = D.P("QI", "t", 0.5)
    n3 = D.P("N3", "t", 0.5)
    D.edge([n3, (n3[0], 56), (qi[0], 56), qi])
    D.note(qi[0] + 8, 82, "xQueueSend", 11, "#374151")
    D.note(qi[0] + 8, 96, "(gw_mqtt_on_message)", 11, "#374151")
    D.straight("N1", "b", "N2", "t")
    D.straight("N2", "b", "N3", "t", label="MQTT_EVENT_DATA", lab_dx=8, lab_dy=0, anchor="start")
    D.straight("N3", "b", "N4", "t") if False else None
    D.h_elbow("L13", "r", "N4", "l", L3x + L3w + 40, ta=0.5, tb=0.5, label="mqtt_link_publish", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.straight("N2", "r", "N2", "r") if False else None
    return D
