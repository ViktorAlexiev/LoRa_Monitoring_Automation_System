from engine import Diagram


def build():
    D = Diagram("executor", 2370)
    L0x, L0w = 30, 400
    L1x, L1w = 570, 780
    L2x, L2w = 1410, 250
    L3x, L3w = 1720, 620
    D.lane(L0x, L0w, "Услуги и хардуер")
    D.lane(L1x, L1w, "Задача exec_radio (ядро 1, приоритет 6): единственият собственик на радиото")
    D.lane(L2x, L2w, "Опашки")
    D.lane(L3x, L3w, "Задача exec_cmd (ядро 1, приоритет 5)")

    D.stack(L1x, L1w, 70, [
        ("F0", "app_main(): старт на приложението", [
            "1  hal_nvs_init()  (NO_FREE_PAGES / NEW_VERSION → изтрива NVS)",
            "2  exec_config_load(): id, nc, c<i>i, c<i>p, freq, sf, bw, txp, key; липсва ли → подразбиране",
            "3  consumers_init(): всички позволени пинове → изходи, ИЗКЛЮЧЕНИ (безопасно състояние)",
            "4  няма id или ключ → return (радиото не стартира, консуматорите остават OFF)",
            "5  cmdQ = xQueueCreate(8, CommandPacket);  ackInQ = xQueueCreate(8, AckReq)",
            "6  exec_radio_reset(): s = State()  (чисто начално състояние)",
            "7  xTaskCreatePinnedToCore(exec_radio_task, стек 5120 B, приоритет 6, ядро 1)",
            "8  exec_cmd_init() + xTaskCreatePinnedToCore(exec_cmd_task, 3072 B, приоритет 5, ядро 1)",
            "9  power_enable_auto_light_sleep(DIO1): честота 40–80 MHz, събуждане от GPIO (HIGH)",
        ], "init"),
        ("F1", "exec_radio_task(): старт на задачата", [
            "s.task = xTaskGetCurrentTaskHandle()",
            "radio_setup(&p, 0): времена от SF/BW → мощност ≤ максимума на модула → ConfigLoRa_t → reset + begin (повтаря до успех) → OCP 140 mA",
            "radio_listen(): startReceive (непрекъснат прием) + hal_irq_rearm(DIO1)",
            "hal_irq_attach(DIO1, задачата): прекъсване на ниво HIGH",
            "esp_timer exec_hb: първи срок на случайно време от 1 s до 304 s",
            "exec_radio_request_restart_response(): restartPending = true, retries = 0, sentAt = now → sendStateResponseWire(RESTART)",
        ], "fn"),
        ("F2", "Главен цикъл на exec_radio_task", [
            "for (;;) {",
            "    w = exec_radio_service();",
            "    ulTaskNotifyTake(pdTRUE, w == 0xFFFFFFFF ? portMAX_DELAY : ms(w));",
            "}",
            "Събуждат я: DIO1 (ISR), hbTimerCb, xTaskNotifyGive от exec_cmd, изтичане на w.",
            "Няма работа → блокира безкрайно → tickless idle → ЛЕК СЪН на чипа.",
            "Известията не се губят: това е брояч, а ulTaskNotifyTake(pdTRUE) го нулира.",
        ], "fn"),
        ("F3", "exec_radio_service(): една обиколка", [
            "for (pass = 0; pass < 8; pass++) {",
            "    ① rxDrain()      ② drainAckIn()      ③ restartRespManager()",
            "    ④ ackManager()   ⑤ heartbeatTick()   ⑥ radioTxTick()",
            "    p = ③ | ④ | ⑤ | ⑥   (| а не ||, затова се изпълняват всичките)",
            "    if (!p) break;        // нищо не се е променило → спокойно състояние",
            "}",
            "w = ⑦ nextWaitMs();   if (w == 0 && !progress) w = 1;   return w;",
        ], "fn"),
        ("F4", "rxDrain()", [
            "до 16 пъти: n = radio_read_packet(buf[64], 64)",
            "n == 0 (RX_NONE) → break;   n == −1 (лош CRC) → пропуска;   иначе → rxHandle(buf, n)",
            "накрая hal_irq_rearm(DIO1): DIO1 вече е ниско, прекъсването може да се върне",
        ], "fn", "①"),
        ("F5", "rxHandle(buf, len): разпознава пакета по дължина", [
            "len == 26 (КОМАНДА):",
            "    memcmp(buf, моя id, 6) ≠ 0 → 'не е за мен', край (преди дешифриране)",
            "    cryptoParseWirePacket(buf+6, 20, key, GW_CMD): MAC ✗ → отхвърля",
            "    CommandPacket{C_ID[4], com, status} → xQueueSend(cmdQ, 0)   (пълна → лог, команда изгубена)",
            "len == 20 (заявка за състояние или ACK за рестарт):",
            "    целта ≠ моя id → край",
            "    Parse(STATE_REQ) ✓ → sendStateResponseWire(STATE_RESP)",
            "    Parse(STATE_RESP_ACK) ✓ → restartPending = false",
            "друга дължина → 'непознат размер'",
        ], "fn"),
        ("F6", "drainAckIn()", [
            "while (ackCount < 8 && xQueueReceive(ackInQ, 0)) acks[ackCount++] = req",
            "пренася AckReq{pkt, sendAt} от опашката във вътрешния списък (по ред на пристигане)",
        ], "fn", "②"),
        ("F7", "restartRespManager(): автоматичен отговор след рестарт", [
            "!restartPending → край",
            "now − restartSentAt < radioAckTimeoutRestartMs() → още чака ACK",
            "retries < 2: sendStateResponseWire(RESTART) → retries++, sentAt = now",
            "    (слотът зает → опитът НЕ се брои, пробва пак по-късно)",
            "retries == 2: 'няма ACK след 3 опита' → restartPending = false (продължава нормално)",
            "Успешен край: Parse(STATE_RESP_ACK) в rxHandle",
        ], "fsm", "③"),
        ("F8", "ackManager(): ACK/NACK на командите", [
            "txPending → чака (един пакет наведнъж)",
            "първият ACK със sendAt ≤ now (пауза 500 ms след командата):",
            "    plaintext = C_ID(4) + com + status;   ctr = nextCounter()",
            "    Build(EXEC_ACK) → txRequest(TXK_ACK, COMMAND) → маха го от списъка",
            "    nextCounter ✗ → ACK се изхвърля (не излъчва с непотвърден брояч)",
        ], "fn", "④"),
        ("F9", "heartbeatTick(): heartbeat на ~5 минути", [
            "s_hbDue.exchange(false) && !hbPending → hbPending = true, hbSendAt = now + random(1000 ms)",
            "hbPending && !txPending && now ≥ hbSendAt:",
            "    ctr = nextCounter();   Build(EXEC_HB, без данни) = 14 B",
            "    txRequest(TXK_HB, TELEMETRY_SKIP): зает канал (2 CAD) → пропуска до следващия",
        ], "fn", "⑤"),
        ("F10", "radioTxTick(): достъп до канала и предаване (TX слот)", [
            "!txPending → край",
            "r = channelAccessPoll(&txAccess, now, cadProbe, channelRandom)",
            "CH_WAIT: ако е правен CAD → radio_listen() (радиото е било в standby); край на стъпката",
            "CH_CLEAR: radio_transmit(txBuf, txLen) → radio_listen() → лог по вид → txPending = false",
            "CH_GIVEUP: radio_listen();",
            "    ACK и възраст < radioAckTimeoutCmdMs() → нов цикъл на достъп (COMMAND)",
            "    иначе → 'канала остана зает', txPending = false",
        ], "fsm", "⑥"),
        ("F11", "txRequest(kind, buf, len, policy)", [
            "txPending → false (извикващият не губи пакета, пробва по-късно)",
            "копира пакета в txBuf (≤ 64 B), запомня txKind и txCreatedAt = now",
            "channelAccessStart(&txAccess, policy);  txPending = true",
            "пакетът вече е построен и подписан, броячът е взет",
        ], "fn"),
        ("F12", "sendStateResponseWire(typeId)", [
            "txPending → пропуска (ПРЕДИ да вземе брояч)",
            "за всеки консуматор: ID(4) + consumers_state()  ← РЕАЛНОТО ниво на пина (1/0)",
            "ctr = nextCounter();  Build(STATE_RESP | STATE_RESP_RESTART)",
            "txRequest(kind, COMMAND)",
            "0 консуматора → 14 B (като heartbeat; разпознава се по MAC)",
        ], "fn"),
        ("F13", "nextCounter()", [
            "първо извикване: CeilingCounter.begin('ceil')",
            "next(&ctr): current ≥ committed → NVS: committed += 100 (commit)",
            "NVS ✗ → false → нищо не се излъчва",
        ], "fn"),
        ("F14", "nextWaitMs(): колко да спи", [
            "txPending: само channelAccessNextMs(txAccess)",
            "иначе най-близкият от: acks[i].sendAt, hbSendAt, restartSentAt + radioAckTimeoutRestartMs()",
            "няма нищо → 0xFFFFFFFF (чакай само събитие)",
        ], "fn", "⑦"),
    ])

    b = D.boxes
    # ---------------- lane 0: хардуер и услуги
    D.box("HW", L0x, b["F1"].y, L0w, "SX1262 радио (SPI)", [
        "DIO1 → HIGH при събитие (RxDone, TxDone, CadDone)",
        "BUSY, RST, NSS; SPI2: SCK 12, MISO 13, MOSI 11",
        "в RX слуша непрекъснато и не заспива (само ESP32 спи)",
    ], "hw")
    D.box("ISR", L0x, b["F2"].y, L0w, "irqIsr(): ISR в IRAM", [
        "gpio_intr_disable(DIO1)  (иначе се вика безкрайно)",
        "vTaskNotifyGiveFromISR(exec_radio, &woken)",
        "portYIELD_FROM_ISR(woken)",
        "прекъсване на НИВО (HIGH): събитие не се губи",
        "hal_irq_rearm() го връща СЛЕД четене на радиото",
    ], "isr")
    D.box("TMR", L0x, b["F3"].y, L0w, "esp_timer exec_hb → hbTimerCb()", [
        "exec_radio_hb_due(): s_hbDue = true; xTaskNotifyGive(exec_radio)",
        "esp_timer_start_once(304 s): сам се подновява",
    ], "isr")
    D.box("RAD", L0x, b["F4"].y, L0w, "agro_radio (RadioLib + EspHal)", [
        "radio_setup: radioTimingInit → clamp txp → ConfigLoRa_t → reset+begin (retry) → OCP",
        "radio_listen: startReceive + hal_irq_rearm",
        "radio_read_packet: RX_DONE? → getPacketLength → readData (CRC)",
        "radio_transmit: standby → startTransmit → чака ToA → finishTransmit",
        "channelActive (CAD): startChannelScan 2 символа, poll DIO1 ≤ радио CAD timeout",
        "→ SPI драйвер на IDF (spi_master)",
    ], "svc")
    D.box("CRY", L0x, b["F8"].y, L0w, "crypto_common (mbedTLS, хардуерен AES)", [
        "Build: nonce[тип, ID, брояч BE] → AES-128-CTR → CMAC(nonce ‖ шифротекст) → 4 B",
        "Parse: ПЪРВО MAC (constant-time), после дешифрира",
        "wire: [ID 6][данни n][брояч 4][MAC 4]",
    ], "svc")
    D.box("CHA", L0x, b["F10"].y, L0w, "channel_access (автомат)", [
        "FIRST_CAD → CH_CLEAR при свободен канал",
        "зает → SENSING (CAD на всяка стъпка до срока)",
        "→ освободи се → BACKOFF (случаен слот 0–3) → последен CAD",
        "COMMAND: срок ToA(64 B), накрая GIVEUP",
        "TELEMETRY_SKIP: 2 CAD, зает → GIVEUP",
        "NextMs = по-ранното от nextAt и deadline",
    ], "svc")
    D.box("CEI", L0x, b["F13"].y, L0w, "CeilingCounter ('ceil')", [
        "таван в NVS, нов на всеки 100 стойности",
        "дълбок сън: RTC памет",
        "нечист рестарт: скача напред ≤ 100, никога назад",
    ], "svc")
    D.box("TIM", L0x, b["F14"].y, L0w, "radio_timing", [
        "всички ms от SF и BW (нищо хардкоднато)",
        "ToA, CAD, стъпка, слот, срок, ACK таймаути",
    ], "svc")
    D.box("NVS", L0x, D.boxes["TIM"].y + D.boxes["TIM"].h + 26, L0w, "NVS (namespace 'cfg')", [
        "id, nc, c<i>i, c<i>p, freq, sf, bw, txp, key, ceil",
    ], "hw")

    # ---------------- lane 2: опашки
    D.box("QC", L2x, b["F5"].y + 30, L2w, "cmdQ  (8 × CommandPacket)", [
        "exec_radio → exec_cmd",
        "{C_ID[4], com, status}",
    ], "queue")
    D.box("QA", L2x, b["F6"].y, L2w, "ackInQ  (8 × AckReq)", [
        "exec_cmd → exec_radio",
        "{pkt, sendAt = сега + 500 ms}",
    ], "queue")

    # ---------------- lane 3: exec_cmd
    D.box("C1", L3x, b["F5"].y + 30, L3w, "exec_cmd_task()", [
        "for (;;) if (xQueueReceive(cmdQ, &p, portMAX_DELAY)) exec_cmd_process(p);",
        "спи безкрайно, докато няма команда",
    ], "fn")
    D.box("C2", L3x, b["F5"].y + 30 + D.boxes["C1"].h + 26, L3w, "exec_cmd_process(p)", [
        "idx = consumers_index(C_ID)",
        "cmdOk = com ∈ {0xA1 = ON, 0xB2 = OFF}",
        "idx ≥ 0 && consumers_usable(idx) && cmdOk:",
        "    consumers_set(idx, com == ON)  → hal_gpio_write(пин)   status = ACK",
        "иначе → status = NACK  (няма консуматор / пинът не е позволен / невалидна команда)",
        "AckReq{pkt, sendAt = now + 500 ms}  → xQueueSend(ackInQ, 0)",
        "xTaskNotifyGive(exec_radio)  → буди радио задачата",
        "изпълнението е веднага, закъснява само ACK-ът (gateway трябва да излезе от TX)",
    ], "fn")
    c2 = D.boxes["C2"]
    D.box("GP", L3x, c2.y + c2.h + 26, L3w, "consumers + GPIO", [
        "consumers_index / usable / set / state",
        "hal_gpio_output (INPUT_OUTPUT: чете се реалното ниво), board_gpio_usable()",
        "състоянието при state response се чете от ПИНА, не от памет",
    ], "svc")
    D.box("RL", L3x, D.boxes["GP"].y + D.boxes["GP"].h + 26, L3w, "Реле / клапан", [
        "при всеки (ре)старт всички са изключени",
    ], "hw")

    # ---------------- стрелки: вътре в lane 1
    for a, c in [("F0", "F1"), ("F1", "F2"), ("F2", "F3"), ("F3", "F4"), ("F4", "F5")]:
        D.straight(a, "b", c, "t", tb=0.5)

    # ISR / таймер → цикъл
    D.straight("HW", "b", "ISR", "t", label="DIO1", lab_dx=26, lab_dy=0, anchor="start")
    D.straight("ISR", "r", "F2", "l", ta=0.5, tb=0.5, label="notify", dashed=True, color="#B45309")
    D.h_elbow("TMR", "r", "F2", "l", L0x + L0w + 112, tb=0.86, label="notify", dashed=True, color="#B45309", lab_at=2, lab_dx=-40, lab_dy=-4)

    # rxHandle → cmdQ → exec_cmd
    D.straight("F5", "r", "QC", "l", ta=0.35, tb=0.5, label="CommandPacket", lab_dy=-6)
    D.straight("QC", "r", "C1", "l", label="xQueueReceive", lab_dy=-6)
    D.straight("C1", "b", "C2", "t")
    # exec_cmd → ackInQ → drainAckIn
    D.h_elbow("C2", "l", "QA", "r", L2x + L2w + 30, ta=0.55, tb=0.5, label="AckReq", lab_at=1, lab_dx=-30, lab_dy=0, anchor="middle")
    D.straight("QA", "l", "F6", "r", label="xQueueReceive", lab_dy=-6)
    D.edge([D.P("C2", "t", 0.85), (D.P("C2", "t", 0.85)[0], b["F2"].y + 22), D.P("F2", "r", 0.28)], label="xTaskNotifyGive",
           dashed=True, color="#B45309", lab_at=1, lab_dx=-8, lab_dy=-8, anchor="end")
    D.straight("C2", "b", "GP", "t")
    D.straight("GP", "b", "RL", "t", label="пин HIGH/LOW", lab_dx=8, lab_dy=0, anchor="start")

    # към услугите (lane 0)
    xg = L0x + L0w
    D.h_elbow("F1", "l", "RAD", "r", xg + 22, ta=0.5, tb=0.10, label="setup / listen", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("F4", "l", "RAD", "r", xg + 46, ta=0.5, tb=0.34, label="read_packet", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("F10", "l", "RAD", "r", xg + 70, ta=0.18, tb=0.88, label="transmit / CAD", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("F10", "l", "CHA", "r", xg + 118, ta=0.80, tb=0.55, label="Poll / NextMs", lab_at=2, lab_dx=-4, lab_dy=-5, anchor="end")
    D.h_elbow("F8", "l", "CRY", "r", xg + 118, ta=0.45, tb=0.5, label="Build / Parse", lab_at=2, lab_dx=-4, lab_dy=-5, anchor="end")
    D.h_elbow("F12", "l", "CRY", "r", xg + 94, ta=0.4, tb=0.82, label="Build", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.straight("F13", "l", "CEI", "r", label="next()", lab_dy=-6)
    D.straight("F14", "l", "TIM", "r", label="времена", lab_dy=-6)
    D.h_elbow("F12", "r", "GP", "l", L1x + L1w + 26, ta=0.35, tb=0.5, label="consumers_state", dashed=True, lab_at=1, lab_dx=-8, lab_dy=0, anchor="end")

    return D
