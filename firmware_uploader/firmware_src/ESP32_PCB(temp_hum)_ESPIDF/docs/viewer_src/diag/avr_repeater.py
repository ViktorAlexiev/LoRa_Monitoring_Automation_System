from engine import Diagram


def build():
    D = Diagram("avr_repeater", 2560)
    L0x, L0w = 30, 420
    L1x, L1w = 590, 780
    L2x, L2w = 1470, 480
    L3x, L3w = 2050, 470
    D.lane(L0x, L0w, "Услуги и памет")
    D.lane(L1x, L1w, "repeater.ino и radio_io.cpp: цикълът loop()")
    D.lane(L2x, L2w, "Опашка и достъп до канала")
    D.lane(L3x, L3w, "Съня и хардуер")

    D.stack(L1x, L1w, 70, [
        ("R0", "setup(): еднократно след включване или рестарт", [
            "1  wdt_arm_8s_continuous()   // реален watchdog 8 s от първия ред",
            "2  DBG_BEGIN(115200);  loadConfigFromEeprom()",
            "3  seedRandomUnique(REPEATER_ID, 6, hbCounter.peek())   // уникален backoff на устройство",
            "4  radio_setup(): на RX честотата, до успех (с wdt_feed()); влиза в приемане",
            "5  enablePinChangeWake()    // събуждане от DIO0 (PCINT18 = D2)",
        ], "init"),
        ("R1", "loop(): основен цикъл (всичко е неблокиращо)", [
            "wdt_feed();   // 'жив съм': при забиване следващият таймаут рестартира чипа",
            "packetSize = LoRa.parsePacket();  if (packetSize) receive_and_queue(packetSize)",
            "process_pending_forwards();   heartbeat_tick();",
            "if (!any_forward_pending() && !heartbeat_pending()) deep_sleep_until_event()",
            "будни остава само докато нещо чака конкретен millis() (backoff, jitter)",
        ], "fn", 1),
        ("R2", "receive_and_queue(len)", [
            "1  len > DEDUP_MAX_LEN (24) → изчиства радиото и изхвърля цял пакет (отрязан би бил повреден кадър)",
            "2  чете байтовете в buf[24]",
            "3  dedupSeen(buf, i) → дубликат (чут и от друг repeater) → игнорира",
            "4  dedupAdd(buf, i)",
            "5  първи свободен слот от 4: sendAt = millis() + random(0, 50)",
            "6  няма слот → 'forward опашка пълна', пакетът се губи",
            "БЕЗ проверка на MAC: gateway-ът отхвърля фалшивото накрая",
        ], "fn", 2),
        ("R3", "process_pending_forwards()", [
            "accessOwner == NONE → търси слот, чието време е дошло → owner = FWD",
            "channelAccessStart(&ca, TELEMETRY_FORCE)   // по-добре колизия, отколкото загубено измерване",
            "r = channelAccessPoll(&ca, now, cadOnTxFreq, channelRandom)",
            "CH_WAIT → ако е правен CAD: enter_rx_mode() (слуша на RX честотата) и излиза",
            "иначе → transmitNow(buf, len) → слотът се освобождава, owner = NONE",
        ], "fsm", 3),
        ("R4", "heartbeat_tick()", [
            "!hbPending && wdt_ticks_consume(4) → hbPending, hbSendAt = now + random(0, 1000)   // ~32 s",
            "owner == NONE && времето е дошло → owner = HB, channelAccessStart(TELEMETRY_SKIP)",
            "CH_CLEAR → cryptoBuildWirePacket(TYPE_REPEATER_HB, REPEATER_ID, hbCounter.next()) = 14 B",
            "     transmitNow(...)   // heartbeat без данни: ID + брояч + таг",
            "зает → enter_rx_mode(), heartbeat-ът се пропуска до следващия",
        ], "fsm", 4),
        ("R5", "transmitNow(buf, len) и enter_rx_mode()", [
            "transmitNow: loraTransmit(buf, len) (асинхронно, с таймаут), после enter_rx_mode()",
            "enter_rx_mode: LoRa.setFrequency(RX честота);  LoRa.receive()",
            "CAD се прави на TX честотата (cadOnTxFreq), приемането е на RX честотата",
            "RX 434 MHz: лентата на подателите;  TX 433 MHz: лентата по-близо до gateway-а",
            "всеки хоп е на друга лента → цикъл е физически невъзможен",
        ], "fn", 3),
        ("R6", "deep_sleep_until_event()", [
            "DBG_FLUSH();  изключва АЦП и USART0;  PWR_DOWN;  cli()",
            "ако digitalRead(DIO0) == HIGH → sei(), връща модулите, излиза без сън",
            "     (смяната на ниво е минала преди cli(); иначе би спал до тика на watchdog-а с пакет в радиото)",
            "sleep_enable();  sleep_bod_disable();  sei();  sleep_cpu();  sleep_disable()",
            "буди: PCINT2 (DIO0 се вдига при пакет) или watchdog (~8 s, разписание на heartbeat)",
        ], "init", 5),
    ])

    y2 = D.stack(L2x, L2w, 70, [
        ("Q1", "fwdQueue[4]: PendingForward", [
            "buf[24], len, sendAt (millis), active",
            "пакетът чака backoff 0..50 ms, без delay(): loop() продължава да приема",
            "4 слота; пълна → пакетът се губи",
        ], "queue"),
        ("Q2", "accessOwner: един собственик на канала", [
            "OWNER_NONE / OWNER_FWD / OWNER_HB",
            "радиото е едно: само един автомат за достъп до канала върти наведнъж",
            "ca (ChannelAccess) е общ; accessSlot = кой слот се предава",
            "между проверките радиото се връща на RX честотата (cadRan)",
        ], "queue"),
        ("Q3", "dedup: кръгов буфер 6 x 24 B", [
            "dedupSeen: сравнява дължина и байтове (memcmp)",
            "dedupAdd: презаписва най-стария запис",
            "препратеният пакет е байт по байт същият → следващото ниво го хваща точно",
            "144 B RAM",
        ], "queue"),
    ])

    b = D.boxes
    D.box("EE", L0x, b["R0"].y, L0w, "EEPROM (1 KB) - настройки", [
        "ID: 0;  RX честота: 67;  TX честота: 71;  ключ: 75",
        "таван на брояча на heartbeat: 91;  SF: 95;  BW индекс: 96",
        "празно (0xFF) → RX 434 MHz, TX 433 MHz, SF 7, BW 125 kHz",
    ], "svc")
    D.box("CTR", L0x, b["R4"].y, L0w, "CeilingCounter hbCounter", [
        "next() за nonce на heartbeat-а; таван +100 в EEPROM",
        "само heartbeat-ът е собствен поток: препратените пакети не се променят",
    ], "svc")
    D.box("CRY", L0x, b["R4"].y + D.boxes["CTR"].h + 26, L0w, "crypto_common", [
        "библиотека Crypto: AESTiny128, CTR, AES-CMAC",
        "heartbeat: plaintext 0 B → wire = ID 6 + брояч 4 + таг 4 = 14 B",
    ], "svc")
    D.box("CHA", L0x, max(b["R3"].y, D.boxes["CRY"].y + D.boxes["CRY"].h + 26), L0w, "channel_access (чист автомат)", [
        "Start / Poll; политики TELEMETRY_FORCE (препращане) и TELEMETRY_SKIP (heartbeat)",
        "случайно изчакване до ToA на 30 B, после втори CAD",
    ], "svc")
    D.box("CAD", L0x, D.boxes["CHA"].y + D.boxes["CHA"].h + 26, L0w, "cad.cpp: CAD и предаване", [
        "channelActive: непрочетен пакет → зает, радиото не се пипа",
        "loraTransmit: асинхронно, таймаут 1.25 x ToA + 100 ms, wdt_feed() в цикъла",
    ], "svc")
    D.box("WDT", L0x, max(b["R6"].y, D.boxes["CAD"].y + D.boxes["CAD"].h + 26), L0w, "Watchdog (WDE=1, WDIE=1, 8 s)", [
        "ISR(WDT_vect): wdt_ticks++ (тик за heartbeat); WDIE НЕ се вдига в ISR",
        "wdt_feed() в началото на loop() вдига WDIE; без него → рестарт",
        "wdt_ticks_consume(n): атомарно изважда n тика (ATOMIC_BLOCK)",
    ], "isr")

    D.box("PC", L3x, b["R6"].y, L3w, "PCINT2 (порт D)", [
        "PCICR |= PCIE2;  PCMSK2 |= PCINT18 (D2 = DIO0)",
        "ISR(PCINT2_vect) {}  → празна, само събужда",
        "работи и в PWR_DOWN; задейства се при смяна на ниво",
    ], "isr")
    D.box("SLP", L3x, b["R6"].y + D.boxes["PC"].h + 26, L3w, "ATmega328P: PWR_DOWN", [
        "ток ~0.1 µA за самия чип; BOD изключен за съня",
        "будно само докато нещо чака millis()",
    ], "hw")
    D.box("SXR", L3x, b["R3"].y, L3w, "SX127x (SPI)", [
        "NSS 10, RST 9, DIO0 2 (HIGH = чака пакет)",
        "RX честота: слуша подателите;  TX честота: към gateway-а",
        "17 dBm, sync 0x12, CRC",
    ], "hw")

    for a, c in [("R0", "R1"), ("R1", "R2"), ("R2", "R3"), ("R3", "R4"), ("R4", "R5"), ("R5", "R6")]:
        D.straight(a, "b", c, "t")
    D.h_elbow("R2", "r", "Q3", "l", L1x + L1w + 40, ta=0.35, tb=0.5, label="dedup", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("R2", "r", "Q1", "l", L1x + L1w + 20, ta=0.75, tb=0.5, label="слот", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("R3", "r", "Q2", "l", L1x + L1w + 40, ta=0.5, tb=0.5, label="accessOwner", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("R5", "r", "SXR", "l", L2x + L2w + 40, ta=0.5, tb=0.5, label="SPI", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end", dashed=True, color="#6B7380")
    D.h_elbow("R6", "r", "PC", "l", L2x + L2w + 40, ta=0.5, tb=0.5, label="събуждане", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")

    xg = L0x + L0w
    D.h_elbow("R0", "l", "EE", "r", xg + 24, ta=0.5, tb=0.5, label="loadConfigFromEeprom", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("R4", "l", "CTR", "r", xg + 24, ta=0.35, tb=0.5, label="next()", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("R4", "l", "CRY", "r", xg + 48, ta=0.6, tb=0.5, label="Build", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("R3", "l", "CHA", "r", xg + 24, ta=0.4, tb=0.5, label="Poll", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("R3", "l", "CAD", "r", xg + 48, ta=0.75, tb=0.5, label="CAD", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("R1", "l", "WDT", "r", xg + 24, ta=0.3, tb=0.5, label="wdt_feed", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end", dashed=True, color="#6B7380")
    return D
