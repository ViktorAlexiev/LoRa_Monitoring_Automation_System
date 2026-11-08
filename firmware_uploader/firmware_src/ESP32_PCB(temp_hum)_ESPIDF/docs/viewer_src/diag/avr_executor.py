from engine import Diagram


def build():
    D = Diagram("avr_executor", 2560)
    L0x, L0w = 30, 420
    L1x, L1w = 590, 780
    L2x, L2w = 1470, 480
    L3x, L3w = 2050, 470
    D.lane(L0x, L0w, "Услуги и памет")
    D.lane(L1x, L1w, "executor.ino и radio_io.cpp: цикълът loop()")
    D.lane(L2x, L2w, "Опашки и TX слот")
    D.lane(L3x, L3w, "Съня и хардуер")

    D.stack(L1x, L1w, 70, [
        ("E0", "setup(): еднократно след включване или рестарт", [
            "1  wdt_arm_8s_continuous()   // реален watchdog 8 s от първия ред",
            "2  loadConfigFromEeprom(): ID, консуматори, честота, SF/BW, ключ",
            "3  seedRandomUnique(MY_M_ID, 6, txCounter.peek())",
            "4  за всеки консуматор: pinMode(OUTPUT);  digitalWrite(LOW)   // безопасно начално състояние",
            "5  radio_setup();  enablePinChangeWake()",
            "6  sendRestartStateResponse()   // 'роден съм': маркира началото на живота на прошивката",
        ], "init"),
        ("E1", "loop(): основен цикъл (всичко е неблокиращо)", [
            "wdt_feed();  radioReceivePoll();  processRx();  ackManager();",
            "heartbeat_tick();  restartRespManager();  radioTxTick();",
            "заспива (deep_sleep_until_event) само ако: ackCount == 0 && rxCount == 0 && няма heartbeat",
            "     && няма restart отговор && TX слотът е свободен",
        ], "fn", 1),
        ("E2", "radioReceivePoll()", [
            "packetSize = LoRa.parsePacket()",
            "26 B = команда: първите 6 B са целеви ID → не е моят → игнорира (преди декриптиране)",
            "     cryptoParseWirePacket(TYPE_GW_CMD) → невалиден MAC → отхвърля;  иначе rxPush(CommandPacket)",
            "20 B = state request ИЛИ потвърждение на restart отговора: целеви ID, после пробва двата типа",
            "     STATE_REQ → sendStateResponse();   STATE_RESP_ACK → спира повторенията",
            "друга дължина → изчиства радиото",
        ], "fn", 2),
        ("E3", "processRx()", [
            "while (rxPop(&p)): idx = consumerIndex(C_ID);  cmdOk = com == 0xA1 (ON) || 0xB2 (OFF)",
            "ако idx >= 0 && cmdOk: digitalWrite(consumerPins[idx], ON ? HIGH : LOW);  status = ACK (0)",
            "иначе: status = NACK (2) - няма такъв консуматор или невалидна команда",
            "ackEnqueue(p, millis() + 500)   // пауза: Gateway трябва да излезе от TX (полудуплекс)",
        ], "fn", 3),
        ("E4", "ackManager() и общ TX слот", [
            "ако txPending → изчаква реда си;  иначе първият ACK с узряло sendAt",
            "строи cryptoBuildWirePacket(TYPE_EXEC_ACK, MY_M_ID, txCounter.next(), C_ID+com+status) директно в txBuf",
            "txCommit(TXK_ACK, wireLen, COMMAND);  ackRemoveAt(i)   // оттук пакетът е собственост на слота",
            "радиото е едно: един пакет наведнъж (ACK, state response, restart отговор или heartbeat)",
        ], "fn", 3),
        ("E5", "radioTxTick(): достъп до канала и предаване", [
            "channelAccessPoll(&txAccess, millis(), cadProbe, channelRandom)",
            "CH_WAIT → LoRa.receive() между проверките (за да не се изпусне входящ пакет)",
            "CH_CLEAR → loraTransmit(txBuf, txLen) → LoRa.receive();  txPending = false",
            "CH_GIVEUP (срокът изтече): ACK още има смисъл (по-млад от radioAckTimeoutCmdMs) → нов цикъл COMMAND",
            "     иначе отказва се; за heartbeat: пропуска се до следващия",
        ], "fsm", 4),
        ("E6", "sendStateResponseWire(typeId)", [
            "ако txPending → пропуска (преди строенето: иначе всеки отказ харчи стойност на брояча)",
            "plaintext: за всеки консуматор C_ID (4 B) + състояние (1 B от digitalRead) → до 50 B",
            "cryptoBuildWirePacket(STATE_RESP или STATE_RESP_RESTART, ...);  txCommit(..., COMMAND)",
            "wire = 14 + 5 x брой консуматори (до 64 B)",
        ], "fn", 5),
        ("E7", "restartRespManager() и heartbeat_tick()", [
            "restart: чака ACK до radioAckTimeoutRestartMs();  до 2 повторения (общо 3 опита), после се отказва мълчаливо",
            "heartbeat: wdt_ticks_consume(38) → ~5 минути;  hbSendAt = now + random(0, 1000)",
            "     слотът свободен → cryptoBuildWirePacket(TYPE_EXEC_HB) = 14 B;  txCommit(TXK_HB, SKIP)",
            "     зает канал → heartbeat-ът се пропуска (следващият идва по график)",
        ], "fsm", 6),
    ])

    y2 = D.stack(L2x, L2w, 70, [
        ("Q1", "rxBuffer[6] и ackQueue[6]", [
            "rxBuffer: кръгов буфер на CommandPacket {C_ID[4], com, status} = 6 B (packed)",
            "ackQueue + ackSendAt: линейна опашка; ackRemoveAt измества записите",
            "Gateway държи ЕДНА команда в полет → 6 места са голям запас",
        ], "queue"),
        ("Q2", "TX слот (един пакет наведнъж)", [
            "txPending, txKind (ACK / STATE_RESP / RESTART_RESP / HB), txBuf[64], txLen",
            "txCreatedAt: за срока на ACK;  txAccess: състоянието на автомата",
            "пакетът се строи в txBuf (без второ копие и memcpy)",
        ], "queue"),
        ("Q3", "Таблица на консуматорите", [
            "consumerList[10][5], consumerPins[10], NUM_CONSUMERS (от EEPROM)",
            "pinFromString: '7' → цифров пин;  'A3' → аналогов (A0 + 3)",
            "невалиден брой (над 10 или 0xFF) → 0 консуматора: безопасно",
        ], "queue"),
    ])

    b = D.boxes
    D.box("EE", L0x, b["E0"].y, L0w, "EEPROM (1 KB) - настройки", [
        "ID: 0;  брой консуматори: 6;  записи: 7..66 (10 x [ID 4][пин 2])",
        "честота: 67;  ключ: 75;  таван на брояча: 91;  SF: 95;  BW индекс: 96",
        "един общ nonce поток за heartbeat и ACK (типът в nonce-а ги разграничава)",
    ], "svc")
    D.box("CTR", L0x, b["E4"].y, L0w, "CeilingCounter txCounter", [
        "next(): таван +100 в EEPROM ПРЕДИ да ползва стойността",
        "общ за heartbeat, ACK и state response",
    ], "svc")
    D.box("CRY", L0x, b["E4"].y + D.boxes["CTR"].h + 26, L0w, "crypto_common", [
        "AESTiny128, CTR, AES-CMAC (4 B)",
        "downlink: [целеви ID 6 B чисто][wire]  → проверка на целта ПРЕДИ декриптиране",
        "форматът е идентичен с ESP-IDF executor-а и с gateway-а",
    ], "svc")
    D.box("CHA", L0x, max(b["E5"].y, D.boxes["CRY"].y + D.boxes["CRY"].h + 26), L0w, "channel_access (чист автомат)", [
        "COMMAND: CAD, сондиране до срок (ToA на 64 B), случаен backoff 0..3 слота, последен CAD",
        "TELEMETRY_SKIP: едно изчакване и втори CAD, иначе отказ",
    ], "svc")
    D.box("CAD", L0x, D.boxes["CHA"].y + D.boxes["CHA"].h + 26, L0w, "cad.cpp: CAD и предаване", [
        "channelActive: непрочетен пакет → зает",
        "loraTransmit: асинхронно, таймаут 1.25 x ToA + 100 ms, wdt_feed() в цикъла",
    ], "svc")
    D.box("WDT", L0x, max(b["E7"].y, D.boxes["CAD"].y + D.boxes["CAD"].h + 26), L0w, "Watchdog (WDE=1, WDIE=1, 8 s)", [
        "ISR(WDT_vect): wdt_ticks++ (тик за heartbeat);  WDIE се вдига от wdt_feed() в loop()",
        "забит цикъл → следващият таймаут рестартира чипа;  след рестарт 'роден съм' се праща пак",
    ], "isr")

    D.box("CON", L3x, b["E3"].y, L3w, "Консуматори (релета)", [
        "изходни пинове (цифрови или A0..A5), започват LOW",
        "digitalWrite HIGH/LOW по команда; digitalRead връща състоянието за state response",
    ], "hw")
    D.box("PC", L3x, b["E1"].y, L3w, "PCINT2 и PWR_DOWN", [
        "DIO0 (D2) HIGH при пакет → PCINT18 → събужда от PWR_DOWN",
        "deep_sleep_until_event: ако DIO0 вече е HIGH преди cli() → не заспива",
        "ток ~0.1 µA; BOD изключен за съня; Timer0 спира",
    ], "isr")
    D.box("SXR", L3x, b["E5"].y, L3w, "SX127x (SPI)", [
        "NSS 10, RST 9, DIO0 2",
        "честота към Gateway (EEPROM);  17 dBm, sync 0x12, CRC",
        "полудуплекс: приема и предава на една честота",
    ], "hw")

    for a, c in [("E0", "E1"), ("E1", "E2"), ("E2", "E3"), ("E3", "E4"), ("E4", "E5"), ("E5", "E6"), ("E6", "E7")]:
        D.straight(a, "b", c, "t")
    D.h_elbow("E2", "r", "Q1", "l", L1x + L1w + 40, ta=0.6, tb=0.4, label="rxPush", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("E4", "r", "Q2", "l", L1x + L1w + 20, ta=0.5, tb=0.5, label="txCommit", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("E3", "r", "Q3", "l", L1x + L1w + 60, ta=0.6, tb=0.5, label="consumerIndex", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("E3", "r", "CON", "l", L2x + L2w + 40, ta=0.4, tb=0.5, label="digitalWrite", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("E5", "r", "SXR", "l", L2x + L2w + 60, ta=0.5, tb=0.5, label="SPI", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end", dashed=True, color="#6B7380")
    D.h_elbow("E1", "r", "PC", "l", L2x + L2w + 20, ta=0.5, tb=0.5, label="сън / събуждане", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")

    xg = L0x + L0w
    D.h_elbow("E0", "l", "EE", "r", xg + 24, ta=0.5, tb=0.5, label="loadConfigFromEeprom", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("E4", "l", "CTR", "r", xg + 24, ta=0.35, tb=0.5, label="next()", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("E4", "l", "CRY", "r", xg + 48, ta=0.6, tb=0.5, label="Build", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("E5", "l", "CHA", "r", xg + 24, ta=0.4, tb=0.5, label="Poll", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("E5", "l", "CAD", "r", xg + 48, ta=0.75, tb=0.5, label="CAD", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("E1", "l", "WDT", "r", xg + 24, ta=0.3, tb=0.5, label="wdt_feed", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end", dashed=True, color="#6B7380")
    return D
