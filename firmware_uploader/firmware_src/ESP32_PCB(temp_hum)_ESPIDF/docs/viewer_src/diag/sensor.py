from engine import Diagram


def build():
    D = Diagram("sensor", 2560)
    L0x, L0w = 30, 420
    L1x, L1w = 590, 780
    L2x, L2w = 1470, 640
    L3x, L3w = 2200, 320
    D.lane(L0x, L0w, "Услуги и памет")
    D.lane(L1x, L1w, "Единствената задача: app_main() (линейно, без цикъл)")
    D.lane(L2x, L2w, "sensors_io.cpp: двата SHT31")
    D.lane(L3x, L3w, "Хардуер")

    D.stack(L1x, L1w, 70, [
        ("A0", "app_main(): целият цикъл на събуждане", [
            "няма задачи и опашки: след дълбокия сън чипът се стартира наново от app_main()",
            "1  sensor_tx_reset()        s_counterReady = false",
            "2  hal_nvs_init()",
            "3  sensor_config_load(&cfg)",
            "4  sensors_init();  sensors_read(&sample);  sensors_deinit()",
            "5  sensor_radio_init(&cfg) ? sensor_send_packet(&cfg, &sample) : лог 'радиото не стартира'",
            "6  power_deep_sleep_us(113 × 8 s = 904 s ≈ 15 min)   // не връща",
        ], "init"),
        ("A1", "sensor_config_load(c)", [
            "стойности по подразбиране: freq 433 MHz, sf 7, bw 125 kHz, txp 17 dBm",
            "NvsStore.open(NVS_NAMESPACE, само за четене) → неуспех: 'не е конфигурирано', връща подразбиранията",
            "id: getString('id') → hasId = успех и id[0] != 0",
            "freq: getU32 (0 или липсва → подразбиране);   sf: radioSfValid, иначе 7;   bw: radioBwToIndex ≠ 0xFF, иначе 125 kHz",
            "txp: (int8_t) getU8('txp')   // със знак: два комплемента",
            "key: getBlob('key', 16 B) → hasKey = точно 16 B;  няма ключ → 'няма да предавам'",
        ], "fn", 3),
        ("A2", "sensor_radio_init(cfg)", [
            "RadioParams{freqHz, sf, bwHz, txPowerDbm} от конфигурацията",
            "return radio_setup(&p, 5)   // до 5 опита за radio.begin(); неуспех → app_main пропуска цикъла и заспива",
        ], "fn", 5),
        ("A3", "sensor_send_packet(cfg, sample)", [
            "1  !hasId || !hasKey → лог 'не е конфигурирано', return false",
            "2  !s_counterReady → s_txCounter.begin('ceil');  s_counterReady = true",
            "3  pkt = SensorPacketData{S_T, S_H, A_T, A_H}: 4 × int16, little-endian, packed = 8 B (static_assert)",
            "4  hal_delay_ms(hal_random(1000))   // jitter: разминава TX-а с други сензори",
            "5  senderId[6] = id (допълнен с нули);  S_ID пътува в чист вид, нужен на gateway-я преди decrypt",
            "6  s_txCounter.next(&counter) → false → return false (брояч не е записан в NVS, пакет не се излъчва)",
            "7  wireLen = cryptoBuildWirePacket(wire, key, TYPE_SENSOR, senderId, counter, pkt, 8) = 22 B",
            "     [ID 6][шифровани данни 8][брояч 4][CMAC таг 4];  wireLen == 0 → лог, return false",
            "8  listen-before-talk: channelAccessStart(PERSIST) и цикъл (кутия по-долу)",
            "9  ok = radio_transmit(wire, wireLen);  грешка → лог",
            "10 radio.sleep(false)   // най-малък ток; при следващото събуждане begin() прави пълна инициализация",
            "11 return ok",
        ], "fn", 5),
        ("A4", "Listen-before-talk: автомат channel_access (политика PERSIST)", [
            "channelAccessStart(&ca, CH_POLICY_TELEMETRY_PERSIST)",
            "while ((r = channelAccessPoll(&ca, hal_millis(), channelActive, channelRandom)) == CH_WAIT) {",
            "    w = channelAccessNextMs(&ca, hal_millis());",
            "    hal_delay_ms(w > 0 ? w : 1);        // блокира задачата: сензорът няма друга работа",
            "}",
            "състояния: FIRST_CAD → ако е зает: SENSING (изчаква пауза) → BACKOFF (случаен слот) → последен CAD",
            "CH_CLEAR (свободен канал) → излиза от цикъла и предава",
            "срокът изтече при запушен канал → пак CH_CLEAR: измерването е по-ценно от чистия ефир",
            "цикълът не може да остане вечно: NextMs е по-ранното от nextAt и deadline",
        ], "fsm", 5),
        ("A5", "power_deep_sleep_us(904 s)", [
            "esp_sleep_enable_timer_wakeup(us);  esp_deep_sleep_start();",
            "for (;;) {}   // дотук не се стига; чипът се изключва",
            "RAM се губи; пазят се само RTC паметта (брояч) и NVS (flash)",
            "събуждането е като нов старт: пак app_main()",
        ], "init", 6),
    ])

    # lane 2: sensors_io
    y2 = D.stack(L2x, L2w, 70, [
        ("B1", "sensors_init() и sensors_deinit()", [
            "sensors_init: за i от 0 до 1 (почва, въздух):",
            "    channelClose(s_ch[i])        // безопасно повторно извикване",
            "    any |= channelOpen(s_ch[i])",
            "sensors_deinit: channelClose и на двата канала (шините не трябват повече)",
            "каналите са таблица s_ch[2]: име, I2C порт, SDA, SCL, адрес 0x44, шина, устройство, ok",
        ], "fn", 4),
        ("B2", "channelOpen(c)", [
            "i2c_new_master_bus: порт 0 (SDA 4, SCL 5) за почва, порт 1 (SDA 6, SCL 7) за въздух",
            "    clk_source по подразбиране, glitch_ignore_cnt 7, вътрешен pull-up (платката трябва външни)",
            "    грешка → лог, bus = null, return false",
            "i2c_master_bus_add_device: адрес 0x44, 7 бита, 100 kHz;  грешка → channelClose, return false",
            "c.ok = true",
            "soft reset: i2c_master_transmit(0x30 0xA2, 100 ms)   // липсващ сензор не е грешка тук",
            "hal_delay_ms(2);  return true",
        ], "fn"),
        ("B3", "sensors_read(out)", [
            "out.s_t = s_h = a_t = a_h = -32768   (SENSOR_RAW_NODATA)",
            "channelRead(почва, &t, &h) ? s_t = sensorEncodeCompact(t), s_h = sensorEncodeCompact(h) : лог 'няма данни'",
            "channelRead(въздух, &t, &h) ? a_t, a_h по същия начин : лог 'няма данни'",
            "температура и влажност са от едно измерване → валидни само заедно",
            "sensorEncodeCompact(v) = (int16)(v × 100 ± 0.5)   // закръгляване, стойност × 0.01",
        ], "fn"),
        ("B4", "channelRead(c, &t, &h)", [
            "1  !c.ok → return false",
            "2  i2c_master_transmit(0x24 0x00, 100 ms): еднократно измерване, високо повторение, без clock stretching",
            "3  hal_delay_ms(20)   // измерването трае до 15 ms",
            "4  i2c_master_receive(b[6], 100 ms): [T hi][T lo][CRC][H hi][H lo][CRC]",
            "5  return sht31_decode(b, t, h)",
            "всяка I2C грешка → false → 'няма данни'",
        ], "fn"),
        ("B5", "sht31_decode(b, &t, &h) и crc8", [
            "crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5] → false   // и двете CRC трябва да са верни",
            "crc8: полином 0x31, начална стойност 0xFF, XOR и 8 измествания на байт",
            "rt = (b0 << 8) | b1;   rh = (b3 << 8) | b4",
            "t = −45 + 175 × rt / 65535;     h = 100 × rh / 65535",
            "t < −41 или t > 126 → false   // извън обхвата на сензора (−40..125 °C с 1 °C запас)",
            "иначе *tempC = t;  *humPct = h;  return true",
        ], "fn"),
        ("B6", "channelClose(c)", [
            "i2c_master_bus_rm_device(dev);  i2c_del_master_bus(bus)",
            "dev = bus = null;  ok = false",
            "извиква се и в началото на channelOpen, и след четенето",
        ], "fn"),
    ])

    b = D.boxes
    # lane 0: услуги и памет
    D.box("NVS", L0x, b["A1"].y, L0w, "NvsStore (NVS във flash)", [
        "ключове: id, freq, sf, bw, txp, key (16 B)",
        "пише ги config приложението",
        "празен NVS → устройството не е конфигурирано",
    ], "svc")
    D.box("RAD1", L0x, b["A2"].y, L0w, "agro_radio: radio_setup", [
        "SPI → SX1262, radio.begin() до 5 пъти",
        "честота, sf, bw, мощност; sync word на мрежата",
    ], "svc")
    D.box("CTR", L0x, b["A3"].y, L0w, "CeilingCounter 'ceil'", [
        "begin: ако е събуждане от дълбок сън и RTC паметта е валидна, взема current и committed оттам",
        "иначе чете таван от NVS",
        "next: current ≥ committed → записва таван +100 в NVS (при неуспех пакетът не се излъчва)",
        "после current++ и копие в RTC паметта",
    ], "svc")
    D.box("CRY", L0x, b["A3"].y + D.boxes["CTR"].h + 26, L0w, "crypto_common", [
        "Build: nonce от тип, ID и брояч → AES-128-CTR на 8 B → CMAC, първите 4 B",
        "wire = [ID 6][шифровани 8][брояч 4][таг 4] = 22 B",
    ], "svc")
    D.box("CHA", L0x, max(b["A4"].y, D.boxes["CRY"].y + D.boxes["CRY"].h + 26), L0w, "channel_access.h (чист автомат)", [
        "Start / Poll / NextMs",
        "channelActive (CAD) и channelRandom се подават отвън",
        "няма собствено време: часовникът идва от hal_millis()",
    ], "svc")
    D.box("RAD2", L0x, b["A4"].y + D.boxes["CHA"].h + 26, L0w, "agro_radio: CAD и предаване", [
        "channelActive: startChannelScan 2 символа, чака DIO1",
        "radio_transmit: standby → startTransmit → чака времето във въздуха → finishTransmit",
        "radio.sleep(false): радиото спи",
    ], "svc")
    D.box("SLP", L0x, max(b["A5"].y, D.boxes["RAD2"].y + D.boxes["RAD2"].h + 26), L0w, "ESP32-S3: дълбок сън", [
        "таймер за събуждане 904 s",
        "ток в съня: единици µA",
        "RTC памет остава, цялата RAM се губи",
    ], "hw")

    # lane 3: хардуер
    D.box("SHS", L3x, b["B2"].y, L3w, "SHT31 почва", [
        "I2C порт 0, SDA 4, SCL 5, адрес 0x44",
        "отговаря на soft reset",
    ], "hw")
    D.box("SHA", L3x, b["B2"].y + D.boxes["SHS"].h + 26, L3w, "SHT31 въздух", [
        "I2C порт 1, SDA 6, SCL 7, адрес 0x44",
        "две независими шини",
    ], "hw")
    D.box("MEAS", L3x, b["B4"].y, L3w, "Измерване", [
        "0x2400: еднократно, високо повторение",
        "до 15 ms, чака се 20 ms",
        "отговор 6 B с две CRC",
    ], "hw")

    # поток в lane 1
    for a, c in [("A0", "A1"), ("A1", "A2"), ("A2", "A3"), ("A3", "A4"), ("A4", "A5")]:
        D.straight(a, "b", c, "t")
    # A0 -> sensors_io
    D.h_elbow("A0", "r", "B1", "l", L1x + L1w + 40, ta=0.3, tb=0.2, label="стъпка 4", lab_at=0, lab_dy=-6)
    # вътре в sensors_io
    for a, c in [("B1", "B2"), ("B2", "B3"), ("B3", "B4"), ("B4", "B5")]:
        D.straight(a, "b", c, "t")
    # B6 е вика се от B1/B2: пунктир отдясно
    D.h_elbow("B1", "r", "B6", "r", L2x + L2w + 20, ta=0.8, tb=0.5, label="channelClose", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end", dashed=True, color="#6B7380")
    # хардуер
    D.h_elbow("B2", "r", "SHS", "l", L2x + L2w + 40, ta=0.4, tb=0.5, label="I2C", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("B2", "r", "SHA", "l", L2x + L2w + 60, ta=0.8, tb=0.5)
    D.h_elbow("B4", "r", "MEAS", "l", L2x + L2w + 40, ta=0.5, tb=0.5, label="transmit / receive", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    # услуги в lane 0
    xg = L0x + L0w
    D.h_elbow("A1", "l", "NVS", "r", xg + 24, ta=0.5, tb=0.5, label="getString / getU32 / getBlob", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("A2", "l", "RAD1", "r", xg + 24, ta=0.5, tb=0.5, label="radio_setup", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("A3", "l", "CTR", "r", xg + 24, ta=0.45, tb=0.4, label="begin / next", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("A3", "l", "CRY", "r", xg + 48, ta=0.65, tb=0.5, label="Build", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("A4", "l", "CHA", "r", xg + 24, ta=0.35, tb=0.5, label="Poll / NextMs", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("A4", "l", "RAD2", "r", xg + 48, ta=0.75, tb=0.6, label="CAD", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("A3", "l", "RAD2", "r", xg + 72, ta=0.9, tb=0.85, label="radio_transmit", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    D.h_elbow("A5", "l", "SLP", "r", xg + 24, ta=0.5, tb=0.5, label="deep sleep", lab_at=1, lab_dx=-6, lab_dy=0, anchor="end")
    return D
