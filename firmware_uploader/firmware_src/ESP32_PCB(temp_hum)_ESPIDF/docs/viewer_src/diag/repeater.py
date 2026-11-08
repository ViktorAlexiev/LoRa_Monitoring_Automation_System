from engine import Diagram


def build():
    D = Diagram("repeater", 2440)
    L0x, L0w = 30, 400
    L1x, L1w = 570, 780
    L2x, L2w = 1420, 280
    L3x, L3w = 1770, 570
    D.lane(L0x, L0w, "Услуги и хардуер")
    D.lane(L1x, L1w, "Задача rep_rx (ядро 1, приоритет 6): приема")
    D.lane(L2x, L2w, "Опашка и mutex")
    D.lane(L3x, L3w, "Задача rep_fwd (ядро 1, приоритет 5): препраща")

    D.stack(L1x, L1w, 70, [
        ("P0", "app_main(): старт на приложението", [
            "1  hal_nvs_init();  rep_config_load(): id, freq (RX, 434 MHz), freqtx (TX, 433 MHz), sf, bw, txp, key",
            "2  няма id или ключ → return",
            "3  fwdQ = xQueueCreate(4, FwdItem)",
            "4  mutex = xSemaphoreCreateMutex()   (с наследяване на приоритет)",
            "5  rep_radio_reset(): dedup, опашка, mutex",
            "6  xTaskCreatePinnedToCore(rep_rx_task, 4096 B, приоритет 6, ядро 1)",
            "7  xTaskCreatePinnedToCore(rep_fwd_task, 4096 B, приоритет 5, ядро 1)",
            "8  power_enable_auto_light_sleep(DIO1)",
        ], "init"),
        ("P1", "rep_rx_task(): старт на задачата", [
            "xSemaphoreTake(mutex);  rep_radio_begin();  xSemaphoreGive(mutex)",
            "    radio_setup(&p, 0) на RX честотата (до успех; мъртъв чип → държи mutex-а завинаги, няма watchdog)",
            "    rep_enter_rx_mode(): radio_set_frequency(freqRx) + radio_listen()",
            "hal_irq_attach(DIO1, задачата)",
            "esp_timer rep_hb: първи срок на случайно време от 1 s до 304 s",
        ], "fn"),
        ("P2", "Главен цикъл на rep_rx_task", [
            "for (;;) {",
            "    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);     // само DIO1: спи безкрайно, чипът заспива леко",
            "    xSemaphoreTake(mutex, portMAX_DELAY);",
            "    rep_rx_service();",
            "    xSemaphoreGive(mutex);",
            "}",
        ], "fn"),
        ("P3", "rep_rx_service()", [
            "до 16 пъти: n = radio_read_packet(buf[32], 32)",
            "RX_NONE → break;   CRC_ERROR → пропуска (повреден пакет не се препраща);   иначе → rxHandle(buf, n)",
            "накрая hal_irq_rearm(DIO1)",
        ], "fn"),
        ("P4", "rxHandle(buf, len)", [
            "i = min(len, 32);   i ≤ 0 → край",
            "s_dedup.seen(buf, i)? → дубликат (чут и от друг repeater) → игнорира",
            "s_dedup.add(buf, i)   (преди да знае дали опашката ще го приеме)",
            "FwdItem{kind = FRAME, len = i, buf, sendAt = now + random(50 ms)}",
            "xQueueSend(fwdQ, 0): пълна (4) → пакетът се губи, лог",
            "БЕЗ проверка на MAC: препраща всичко с вярна CRC и sync word на неговата честота;",
            "gateway-ът отхвърля фалшивото накрая",
        ], "fn"),
        ("P5", "rep_enter_rx_mode() и честоти", [
            "radio_set_frequency(freqRx) → radio.standby() + setFrequency;  radio_listen()",
            "RX честота (434 MHz): слуша лентата на подателите (sensor и repeater от по-далечното ниво)",
            "TX честота (433 MHz): лентата по-близо до gateway-а; там се прави CAD и се предава",
            "всеки хоп е на друга честота → цикъл е физически невъзможен",
            "препратеният пакет е идентичен с оригинала → dedup на следващото ниво го хваща",
        ], "fsm"),
    ])

    b = D.boxes
    D.box("HW", L0x, b["P1"].y, L0w, "SX1262 радио (SPI)", [
        "DIO1 → HIGH при събитие",
        "честотата се сменя само в standby",
        "между операциите радиото е на RX честотата",
    ], "hw")
    D.box("ISR", L0x, b["P2"].y, L0w, "irqIsr(): ISR в IRAM", [
        "gpio_intr_disable(DIO1);  vTaskNotifyGiveFromISR(rep_rx)",
        "portYIELD_FROM_ISR(woken)",
        "ниво HIGH: събитие не се губи",
    ], "isr")
    D.box("TMR", L0x, b["P3"].y, L0w, "esp_timer rep_hb → hbTimerCb()", [
        "rep_hb_due(): FwdItem{kind = HB, sendAt = now + random(1000 ms)} → fwdQ",
        "esp_timer_start_once(304 s): сам се подновява",
    ], "isr")
    D.box("RAD", L0x, b["P4"].y, L0w, "agro_radio (RadioLib + EspHal)", [
        "radio_setup / radio_listen / radio_read_packet",
        "radio_set_frequency: standby + setFrequency",
        "radio_transmit, channelActive (CAD)",
    ], "svc")
    D.box("DED", L0x, b["P4"].y + D.boxes["RAD"].h + 26, L0w, "DedupRing<6, 32>", [
        "6 последни пакета, ≤ 32 B",
        "сравнява цялото съдържание",
    ], "svc")

    # lane 2: опашка и mutex
    D.box("QF", L2x, b["P4"].y + 40, L2w, "fwdQ  (4 × FwdItem)", [
        "rep_rx, таймер → rep_fwd",
        "FwdItem{kind (FRAME | HB), len, buf[32], sendAt}",
        "sendAt: не по-рано от (backoff / jitter)",
    ], "queue")
    D.box("MX", L2x, D.boxes["QF"].y + D.boxes["QF"].h + 40, L2w, "Mutex на радиото", [
        "взема се за всяка стъпка с радиото:",
        "CAD, смяна на честота, предаване",
        "между проверките е СВОБОДЕН → rep_rx приема",
        "няма вложени mutex-и → няма deadlock",
        "наследяване на приоритет: rep_fwd временно става 6",
    ], "queue")

    # lane 3: rep_fwd
    y3 = D.stack(L3x, L3w, 70, [
        ("F1", "rep_fwd_task()", [
            "for (;;)",
            "    if (xQueueReceive(fwdQ, &it, portMAX_DELAY)) rep_fwd_process(it);",
            "спи безкрайно, когато няма какво да препраща",
        ], "fn"),
        ("F2", "rep_fwd_process(it)", [
            "1  if (now < it.sendAt) hal_delay_ms(it.sendAt − now)   // mutex-ът НЕ се държи",
            "2  channelAccessStart(&ca, kind == HB ? TELEMETRY_SKIP : TELEMETRY_PERSIST)",
            "3  for (;;) {",
            "      xSemaphoreTake(mutex);   s_cadRan = false;",
            "      r = channelAccessPoll(&ca, now, cadOnTxFreq, channelRandom);",
            "4    CH_WAIT: ако е правен CAD → rep_enter_rx_mode();  w = channelAccessNextMs();",
            "         xSemaphoreGive(mutex);  hal_delay_ms(w > 0 ? w : 1);  continue",
            "5    FRAME → transmitNow(it.buf, it.len)   (PERSIST: CH_CLEAR и при изтекъл срок)",
            "6    HB и CH_CLEAR → ctr = nextCounter('ceil');  Build(REPEATER_HB) = 14 B;",
            "         transmitNow(wire);  иначе rep_enter_rx_mode()",
            "7    HB и CH_GIVEUP → rep_enter_rx_mode();  'пропускам този heartbeat'",
            "8    xSemaphoreGive(mutex);  return",
            "   }",
        ], "fsm"),
        ("F3", "cadOnTxFreq()", [
            "s_cadRan = true;  radio_set_frequency(freqTx);  return channelActive();",
            "CAD се прави на TX честотата, защото там ще се предава",
        ], "fn"),
        ("F4", "transmitNow(buf, len)", [
            "radio_set_frequency(freqTx)   (изрично: радиото да е на TX честотата)",
            "ok = radio_transmit(buf, len)",
            "rep_enter_rx_mode()           (обратно на RX честотата)",
            "return ok",
        ], "fn"),
        ("F5", "hbTimerCb() и rep_hb_due()", [
            "изпълнява се в esp_timer задачата, затова е кратко",
            "rep_hb_due(): FwdItem{HB, sendAt = now + random(1000)} → xQueueSend(fwdQ, 0)",
            "esp_timer_start_once(s_hbTimer, 304 s): сам се подновява",
        ], "fn"),
    ])
    D.box("SA", L3x, y3, L3w, "agro_radio (RadioLib + EspHal)", [
        "radio_set_frequency(hz): standby + setFrequency;   radio_transmit: standby → startTransmit → чака ToA → finishTransmit",
        "channelActive (CAD): startChannelScan 2 символа, poll DIO1;   radio_listen: startReceive + hal_irq_rearm",
    ], "svc")
    y3 = D.boxes["SA"].y + D.boxes["SA"].h + 26
    D.box("CHA", L3x, y3, L3w, "channel_access (автомат)", [
        "PERSIST (кадри): FIRST_CAD → SENSING → BACKOFF → CH_CLEAR;  срокът изтече → пак CH_CLEAR (не GIVEUP)",
        "    вторият repeater изчаква края на първия предаване (и дубликатът се отсява от dedup)",
        "SKIP (heartbeat): 2 CAD, зает → GIVEUP",
        "NextMs = по-ранното от nextAt и deadline",
    ], "svc")
    y3 = D.boxes["CHA"].y + D.boxes["CHA"].h + 26
    D.box("CRY", L3x, y3, L3w, "crypto_common + CeilingCounter", [
        "само за собствения heartbeat (REPEATER_HB, 14 B);  Build: nonce → AES-128-CTR → CMAC",
        "брояч 'ceil' в NVS: таван на всеки 100 стойности",
    ], "svc")

    # стрелки lane 1
    for a, c in [("P0", "P1"), ("P1", "P2"), ("P2", "P3"), ("P3", "P4"), ("P4", "P5")]:
        D.straight(a, "b", c, "t")
    D.straight("HW", "b", "ISR", "t", label="DIO1", lab_dx=26, lab_dy=0, anchor="start")
    D.straight("ISR", "r", "P2", "l", label="notify", dashed=True, color="#B45309")
    xg = L0x + L0w
    D.h_elbow("P1", "l", "RAD", "r", xg + 22, ta=0.4, tb=0.15, label="setup / listen", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("P3", "l", "RAD", "r", xg + 46, ta=0.5, tb=0.5, label="read_packet", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("P4", "l", "DED", "r", xg + 70, ta=0.4, tb=0.5, label="seen / add", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("TMR", "r", "QF", "l", xg + 130, ta=0.5, tb=0.2, label="HB", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end", dashed=True, color="#B45309")

    # lane 1 -> lane 2 -> lane 3
    D.straight("P4", "r", "QF", "l", ta=0.6, tb=0.55, label="FwdItem", lab_dy=-6)
    D.h_elbow("QF", "r", "F1", "l", L2x + L2w + 30, ta=0.5, tb=0.5, label="xQueueReceive", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.straight("F1", "b", "F2", "t")
    D.straight("F2", "b", "F3", "t") if False else None
    D.h_elbow("F2", "l", "MX", "r", L2x + L2w + 54, ta=0.2, tb=0.5, label="Take / Give", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("P2", "r", "MX", "l", L1x + L1w + 28, ta=0.75, tb=0.3, label="Take / Give", lab_at=1, lab_dx=6, lab_dy=0, anchor="start")
    xr = L3x + L3w + 14
    D.h_elbow("F2", "r", "CHA", "r", xr, ta=0.45, tb=0.5)
    D.h_elbow("F2", "r", "CRY", "r", xr + 16, ta=0.8, tb=0.5)
    D.h_elbow("F3", "r", "SA", "r", xr + 32, ta=0.5, tb=0.3)
    D.h_elbow("F4", "r", "SA", "r", xr + 48, ta=0.5, tb=0.75)
    return D
