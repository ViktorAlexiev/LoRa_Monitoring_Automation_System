from engine import Diagram


def build():
    D = Diagram("avr_sensor", 2560)
    L0x, L0w = 30, 420
    L1x, L1w = 590, 780
    L2x, L2w = 1470, 640
    L3x, L3w = 2200, 320
    D.lane(L0x, L0w, "Услуги и памет")
    D.lane(L1x, L1w, "sensor.ino: setup() и loop() (линейно, без прекъсвания освен watchdog)")
    D.lane(L2x, L2w, "sensors_io.cpp: SHT31 и SHT21 през I2C")
    D.lane(L3x, L3w, "Хардуер")

    D.stack(L1x, L1w, 70, [
        ("S0", "setup(): еднократно след включване или рестарт", [
            "1  wdt_start()              // реален watchdog 8 s от първия ред",
            "2  loadConfigFromEeprom()   // ID, честота, SF, BW, ключ, таван на брояча",
            "3  DBG_BEGIN(115200)",
            "4  seedRandomUnique(SENSOR_ID, 6, txCounter.peek())   // уникален seed на random()",
            "5  печата 'Sensor node ready' и DBG_FLUSH()",
        ], "init"),
        ("S1", "loop(): един цикъл на събуждане (повтаря се безкрайно)", [
            "1  delay(random(0, 1000));  wdt_feed()   // jitter: разминава TX-а с други сензори",
            "2  sensors_begin(); sensors_read(); sensors_end(); wdt_feed()   // 4 стойности float, грешка = 255.0",
            "3  lora_init() ? send_sensor_packet(...) : пропуска цикъла (мъртво радио)",
            "4  deep_sleep(113)   // 113 тика x 8 s = 904 s, ~15 минути",
            "нищо не работи между циклите: чипът спи в PWR_DOWN",
        ], "fn", 1),
        ("S2", "lora_init()", [
            "radioTimingInit(SF, BW, CR 4/5, преамбюл 8, CRC)   // всички времена от SF/BW",
            "LoRa.setPins(NSS 10, RST 9, DIO0 2)",
            "while (!LoRa.begin(freq)): след 5 опита → false (цикълът се пропуска); иначе wdt_feed() и delay(200)",
            "radioApplyModemSettings(): SF, BW, CR, преамбюл + LDRO по реалното символно време",
            "setTxPower(17 dBm);  setSyncWord(0x12);  enableCrc()",
        ], "fn", 3),
        ("S3", "send_sensor_packet(s_t, s_h, a_t, a_h)", [
            "1  encodeCompact: float → int16 x100 (+-0.5 за закръгляване); v >= 254.5 → -32768 'няма данни'",
            "2  pkt = {S_T, S_H, A_T, A_H}: 4 x int16, packed = 8 B (static_assert)",
            "3  cryptoBuildWirePacket(TYPE_SENSOR, SENSOR_ID, txCounter.next(), pkt) = 22 B",
            "     [ID 6][шифровани данни 8][брояч 4][CMAC таг 4]",
            "4  listen-before-talk (кутия по-долу)",
            "5  loraTransmit(wire, 22): асинхронно, с таймаут;  след това LoRa.sleep()",
            "форматът е ИДЕНТИЧЕН с ESP-IDF прошивката (gateway-ят е един)",
        ], "fn", 3),
        ("S4", "Listen-before-talk: channel_access (политика TELEMETRY_FORCE)", [
            "channelAccessStart(&ca, CH_POLICY_TELEMETRY_FORCE)",
            "while (channelAccessPoll(&ca, millis(), channelActive, channelRandom) == CH_WAIT) { wdt_feed(); delay(1); }",
            "FIRST_CAD: свободно → CH_CLEAR веднага",
            "зает → ЕДНО случайно изчакване до radioTelemetryWaitMs() (ToA на 30 B), после втори CAD",
            "пак зает → FORCE: предава въпреки това (измерването е по-ценно от чистия ефир)",
            "най-много 2 CAD-а; сензорът няма друга работа, затова блокира",
        ], "fsm", 3),
        ("S5", "deep_sleep(113)", [
            "DBG_FLUSH();  wdt_count = 0;  wdt_reset()   // нова фаза: първият тик след пълни ~8 s",
            "PRR: изключва АЦП, USART0, SPI, TWI, Timer1, Timer2 (Timer0 остава)",
            "while (wdt_count < cycles) { power_down_once(); wdt_feed(); }",
            "всеки тик е прекъсване от watchdog; wdt_feed() вдига WDIE (доказателство 'жив съм')",
            "накрая включва модулите обратно; loop() започва наново",
        ], "init", 4),
        ("S6", "power_down_once(): един сън до прекъсване", [
            "set_sleep_mode(SLEEP_MODE_PWR_DOWN);  cli();  sleep_enable();",
            "sleep_bod_disable()   // brown-out детекторът добавя десетки µA в сън",
            "sei();  sleep_cpu();  sleep_disable();",
            "инструкцията след sei() се изпълнява преди прекъсване → събуждането не се губи",
            "буди само watchdog-ът (ISR(WDT_vect): wdt_count++)",
        ], "isr"),
    ])

    y2 = D.stack(L2x, L2w, 70, [
        ("T1", "sensors_begin() и sensors_end()", [
            "begin: Wire.begin();  SHT31: soft reset 0x30 0xA2;  endTransmission();  delay(2)",
            "липсващ сензор не е грешка тук - ще се види при четенето",
            "end: Wire.end()   // TWI изключен: не тегли ток и освобождава пиновете",
        ], "fn"),
        ("T2", "sensors_read(s_t, s_h, a_t, a_h)", [
            "почва: sht31_read(st, sh) ? стойностите : 255.0 и за двете",
            "въздух: sht21_read(0xF3, ...) за температура, sht21_read(0xF5, ...) за влажност - отделно",
            "при грешка печата предупреждение и праща 255.0 (после → -32768 в пакета)",
        ], "fn"),
        ("T3", "sht31_read(t, h) - почва, адрес 0x44", [
            "1  Wire: команда 0x24 0x00 (еднократно, високо повторение, без clock stretching)",
            "2  delay(20)   // измерването трае до 15 ms",
            "3  requestFrom(0x44, 6): [T hi][T lo][CRC][H hi][H lo][CRC]",
            "4  crc8(b,2,0xFF) == b[2] и crc8(b+3,2,0xFF) == b[5] → иначе false",
            "5  t = -45 + 175 x rt/65535;  h = 100 x rh/65535",
            "6  t < -41 или t > 126 → false (извън обхвата на SHT31)",
        ], "fn", 1),
        ("T4", "sht21_read(cmd, result, isTemp) - въздух, адрес 0x40", [
            "1  Wire: cmd 0xF3 (температура) или 0xF5 (влажност), no-hold master",
            "2  delay(isTemp ? 90 : 30)   // максимално време за конверсия + запас",
            "3  requestFrom(0x40, 3): MSB, LSB, CRC - четени в отделни редове (Wire.read има страничен ефект)",
            "4  crc8({msb,lsb}, 2, 0x00) != crc → false",
            "5  raw &= 0xFFFC (статус битове);  T = -46.85 + 175.72 x raw/65536;  RH = -6 + 125 x raw/65536",
            "6  диапазон: T -40..85, RH 0..100, иначе false",
        ], "fn", 1),
        ("T5", "crc8(d, n, init)", [
            "Sensirion CRC-8, полином 0x31",
            "crc ^= байт; 8 пъти: (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1",
            "SHT31: начална стойност 0xFF;  SHT21: 0x00",
        ], "fn"),
    ])

    b = D.boxes
    D.box("EE", L0x, b["S0"].y, L0w, "EEPROM (1 KB) - настройки", [
        "ID: адрес 0 (6 B);  честота: 67 (4 B);  ключ: 75 (16 B)",
        "таван на брояча: 91 (4 B);  SF: 95;  BW индекс: 96",
        "пише ги config_avr.ino; реалната програма само чете",
        "празно (0xFF) → стойности по подразбиране",
    ], "svc")
    D.box("CTR", L0x, b["S3"].y, L0w, "CeilingCounter txCounter", [
        "next(): current >= committed → пише нов таван +100 в EEPROM ПРЕДИ да ползва стойността",
        "при внезапен рестарт скача напред най-много със 100, никога назад",
        "EEPROM издържа ~100 000 записа → пише се на 100 стойности",
    ], "svc")
    D.box("CRY", L0x, b["S3"].y + D.boxes["CTR"].h + 26, L0w, "crypto_common", [
        "библиотека Crypto: AESTiny128, режим CTR, AES-CMAC (първите 4 B)",
        "nonce = тип (не се праща) + ID + брояч",
        "encrypt-then-MAC; wire = [ID 6][шифър][брояч 4][таг 4]",
    ], "svc")
    D.box("CHA", L0x, max(b["S4"].y, D.boxes["CRY"].y + D.boxes["CRY"].h + 26), L0w, "channel_access (чист автомат)", [
        "Start / Poll; няма собствено време (millis() отвън)",
        "CAD и случайното число се подават като указатели към функции",
    ], "svc")
    D.box("CAD", L0x, D.boxes["CHA"].y + D.boxes["CHA"].h + 26, L0w, "cad.cpp: радио през регистри", [
        "channelActive: непрочетен пакет → зает; иначе режим CAD, чака CadDone, чисти САМО флаговете на CAD",
        "loraTransmit: endPacket(true) асинхронно, чака TxDone до 1.25 x ToA + 100 ms",
        "radio_timing: всички времена от SF/BW (Semtech AN1200.13)",
    ], "svc")
    D.box("WDT", L0x, max(b["S5"].y, D.boxes["CAD"].y + D.boxes["CAD"].h + 26), L0w, "Watchdog (WDE=1, WDIE=1, 8 s)", [
        "първият таймаут е прекъсване (будилник), хардуерът чисти WDIE",
        "wdt_feed() вдига WDIE; без него следващият таймаут РЕСТАРТИРА чипа",
        "изисква Optiboot-подобен bootloader; -DAGRO_HW_WDT=0 връща режим само-прекъсване",
    ], "isr")
    D.box("SLP", L0x, D.boxes["WDT"].y + D.boxes["WDT"].h + 26, L0w, "ATmega328P: PWR_DOWN", [
        "ток ~0.1 µA за самия чип; Timer0 спира → millis() не напредва",
        "разбужда се само от watchdog",
    ], "hw")

    D.box("SHS", L3x, b["T3"].y, L3w, "SHT31 почва (0x44)", [
        "I2C (TWI) на A4/A5",
        "отговаря на soft reset",
        "един чип: темп. и влажност с една команда",
    ], "hw")
    D.box("SHA", L3x, b["T4"].y, L3w, "SHT21 въздух (0x40)", [
        "същата I2C шина",
        "две отделни команди: температура и влажност",
    ], "hw")
    D.box("SXR", L3x, max(b["S3"].y, D.boxes["SHA"].y + D.boxes["SHA"].h + 26), L3w, "SX127x (SPI)", [
        "NSS 10, RST 9, DIO0 2",
        "433 MHz по подразбиране (от EEPROM)",
        "мощност 17 dBm, sync 0x12",
    ], "hw")

    for a, c in [("S0", "S1"), ("S1", "S2"), ("S2", "S3"), ("S3", "S4"), ("S4", "S5"), ("S5", "S6")]:
        D.straight(a, "b", c, "t")
    D.h_elbow("S1", "r", "T1", "l", L1x + L1w + 40, ta=0.35, tb=0.3, label="стъпка 2", lab_at=0, lab_dy=-6)
    for a, c in [("T1", "T2"), ("T2", "T3"), ("T3", "T4"), ("T4", "T5")]:
        D.straight(a, "b", c, "t")
    D.h_elbow("T3", "r", "SHS", "l", L2x + L2w + 40, ta=0.5, tb=0.5, label="I2C", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("T4", "r", "SHA", "l", L2x + L2w + 40, ta=0.5, tb=0.5, label="I2C", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("S3", "r", "SXR", "l", L2x + L2w + 80, ta=0.5, tb=0.5, label="SPI", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end", dashed=True, color="#6B7380")

    xg = L0x + L0w
    D.h_elbow("S0", "l", "EE", "r", xg + 24, ta=0.5, tb=0.5, label="loadConfigFromEeprom", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("S3", "l", "CTR", "r", xg + 24, ta=0.35, tb=0.4, label="next()", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("S3", "l", "CRY", "r", xg + 48, ta=0.6, tb=0.5, label="Build", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("S4", "l", "CHA", "r", xg + 24, ta=0.4, tb=0.5, label="Poll", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("S4", "l", "CAD", "r", xg + 48, ta=0.75, tb=0.5, label="CAD", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("S5", "l", "WDT", "r", xg + 24, ta=0.4, tb=0.5, label="wdt_feed", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("S6", "l", "SLP", "r", xg + 24, ta=0.5, tb=0.5, label="sleep_cpu", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    return D
