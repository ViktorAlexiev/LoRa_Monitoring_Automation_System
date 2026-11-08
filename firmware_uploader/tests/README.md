# Тестове на firmware/uploader логиката

Нативни (C++, чрез `g++`) и Python тестове върху **реалните** `.cpp`/`.py` файлове от
`firmware_src/` и `firmware_uploader/` — не преписани копия. Компилират се директно
срещу изходния код, с мокнати Arduino/EEPROM/Wire библиотеки, за да могат да се
пуснат на обикновен компютър, без реален AVR/ESP32 хардуер.

## Как да пуснеш всичко

```bash
cd tests/native && ./run_all.sh          # ATmega + TTGO (LoRa_radio_node+TTGO)
```
```bash
cd tests/python && python -m unittest test_validation -v
```
(на тази машина Python е на `C:\Users\alexi\AppData\Local\Programs\Python\Python39\python.exe`,
не в PATH на Git Bash — извикай с пълния път, ако `python`/`python3` не се намира).

## Какво реално се тества (1021 native проверки + 67 python теста = 1088)

| Файл в firmware_src/ | Тестван ли е | Как |
|---|---|---|
| `*/crypto_common.cpp` | ✅ Пълно | Компилиран директно, срещу **верифицирана AES-128** (собствена реализация, тествана срещу официалния FIPS-197 вектор преди употреба). Round-trip за всичките 9 type-id-та × 7 дължини, отхвърляне на грешен ключ/type-id/подправен ciphertext/tag/counter/senderId, детерминизъм, nonce разделяне по поток. |
| `*/ceiling_counter.cpp` | ✅ Пълно | Мокната EEPROM. Commit граници, "unclean reset" симулация (нов обект без предишен RAM state), монотонност през 20 симулирани рестарта. |
| `executor/queues.cpp` | ✅ Пълно | FIFO ред, препълване на двете опашки (RX 8, ACK 8), премахване от средата на ACK опашката. |
| `executor/consumers.cpp` | ✅ Пълно | Търсене по ID, липсващ консуматор, пълен списък от 10, точност на сравнението (4 символа). |
| `repeater/dedup.cpp` | ✅ Пълно | Кръгов буфер (6 записа), wraparound, чувствителност към 1 бит разлика и различна дължина. |
| `executor/config_storage.cpp` | ✅ Пълно | `pinFromString` (числов/аналогов), пълен `loadConfigFromEeprom()` round-trip вкл. смесени типове пинове, клампа на `NUM_CONSUMERS`. |
| `sensor/config_storage.cpp` | ✅ Пълно | Честота (реална стойност + двата fallback случая: 0 и 0xFFFFFFFF), потвърждава default-а е 433 MHz (поправката от тази сесия). |
| `repeater/config_storage.cpp` | ✅ Пълно | Две независими честоти (RX/TX), всяка с отделен fallback. |
| `sensor/sensors_io.cpp` | ✅ Пълно | Мокнат `Wire`/`Adafruit_SHT31`. Нормално четене, I2C грешка, недостатъчно байтове, диапазонна проверка (темп. и влажност поотделно), конкретен regression тест за MSB/LSB поправката. |
| `gateway/lora_handlers.cpp` (dispatch логика) | ⚠️ Частично | Пресъздадена е **точната** dispatch логика (константи + аритметика) от реалния файл и тествана за колизии върху дължини 0-100. Самият `.cpp` НЕ се компилира (изисква LoRa/WiFi/MQTT/ArduinoJson мокове с малка допълнителна стойност). |
| `executor/radio_io.cpp` (dispatch логика) | ⚠️ Частично | Същото - CMD/STATE_REQ/STATE_RESP_ACK дължини, без числена колизия. |
| `firmware_uploader/validation.py` | ✅ Пълно | Всички публични валидатори (вкл. SF, BW, ленти: брой, повторения, разстояние ≥ 2×BW) - module/consumer ID, pin (вкл. резервираните 11/12/13), консуматорски списък (дубликати, резервирани пинове, конфликт с активен executor), WiFi/MQTT полета, LoRa честота (вкл. границите на EU обхватите). |
| `*/radio_timing.cpp` | ✅ Пълно | Time-on-Air срещу независима формула с `double` върху 9216 комбинации (SF7–12 × BW × дължини × CR × CRC), граница на LDRO (SF11/125 kHz), CAD таймаут (8/10/14/22/39/72 ms), стъпка, прозорци, всички ACK/state таймаути, отсъствие на 4000 ms под. |
| `*/channel_access.cpp` | ✅ Пълно | 13 теста с подменени CAD/random: COMMAND (първи CAD веднага, сондиране, backoff, give-up), TELEMETRY_FORCE/SKIP (най-много 2 CAD-а), най-много един CAD на `poll`, `millis()` wraparound. |
| `*/config_storage.cpp` (SF/BW) | ✅ Пълно | EEPROM 95/96: валидни стойности, 0xFF и невалидни -> SF7/125 kHz. |

## ESP32_PCB(temp_hum) - отделен набор (`tests/native/esp32`)

```bash
cd tests/native/esp32 && ./run_esp32.sh          # всичко (компилира + пуска)
./mutation_check.sh                              # мутационна проверка: умишлени грешки трябва да се улавят
```

Тества РЕАЛНИТЕ `.cpp`/`.ino` файлове на `firmware_src/ESP32_PCB(temp_hum)` срещу фалшиви Arduino/ESP-IDF
заместители (`fakelibs_esp32/`: Preferences/NVS, WiFi, PubSubClient, LoRa, сън, часовник, GPIO) и РЕАЛНАТА
библиотека ArduinoJson. Реалните `sensor.ino`, `gateway.ino`, `executor.ino`, `repeater.ino`,
`executor_wifi.ino` и `config_esp32.ino` се включват директно и се изпълняват (`setup()`/`loop()`).

| Какво | Как |
|---|---|
| `ceiling_counter` (NVS + RTC памет) | watermark, уникалност на nonce през рестарти, без запис във flash при събуждане от дълбок сън, ~351 записа/година за сензор |
| `config_storage` (sensor/executor/repeater/executor_wifi) | NVS ключове, default стойности, невалидни SF/BW/честота, клампа на консуматорите |
| `power_mgmt` | лек сън: тиковете от 8 s следват реалното време дори при чести събуждания; дълбок сън 113 x 8 s |
| `config_esp32.ino` | подава се точно `CFG:{...}` като от Firmware Uploader: ACK/NACK, запис в NVS, граници (SF, BW, GPIO 0-39, дължини), `net=1`, непипане на `ceil` |
| `executor_wifi` | реален сеанс срещу фалшив брокер: clean session, Last Will, QoS 1 абониране, филтър по `M_ID`, A1/B2/C3, JSON на отговорите, опашка от 8, reconnect/рестарт |
| sensor -> gateway (e2e) | реалният `sensor.ino` строи 22-байтов пакет, реалният `gateway.ino` го декриптира и публикува JSON; `-32768`, dedup, подправени/чужди пакети, всички SF/BW |
| gateway команди (e2e) | MQTT команда -> LoRa пакет -> ACK/NACK/TIMEOUT, 2 повторения, интервал > airtime при SF7/9/12, чужд ACK, заета линия, състояния, heartbeat; нито едно съобщение не е retained |
| LoRa executor | реалният `executor.ino`: boot (пинове LOW + restart response с 2 повторения), команди, NACK, чужд ключ/цел, подправени пакети, опашки, heartbeat на ~304 s, сън |
| repeater | препращане на истински sensor пакети на TX честотата, dedup (6 записа), опашка, заета линия, heartbeat |

**Мутационна проверка:** `mutation_check.sh` въвежда на копие по една умишлена грешка (~35 мутации: retain
флаг, липсващ филтър по `M_ID`, грешна честота на repeater, сън, NVS ключове, ...) и изисква `run_esp32.sh`
да се провали. Резултат към момента: всички са уловени.

**Какво НЕ е тествано:** реално радио (обхват, колизии, реален CAD на SX127x), реален Wi-Fi/брокер, реален
flash/NVS и износване, реалното събуждане на чипа от DIO0, реалното крипто (Crypto/AES_CMAC библиотеките -
тук е верифицираната AES-128 реализация), реалните пинове на платката (`board_pins.h` е временен).

**Забележка за Windows/MinGW:** тестовете се свързват статично (`-static`), защото в PATH има няколко
`libstdc++-6.dll` и динамичното свързване води до срив в iostream.

## ESP32_PCB(temp_hum)_ESPIDF - отделен набор (`tests/native/espidf`)

```bash
cd tests/native/espidf
./run_espidf.sh              # rtos core sx127x net sensor executor repeater gateway executor_wifi config
./run_compat.sh              # диференциален тест: ESP-IDF <-> стари Arduino прошивки (Python = независим арбитър)
./run_system.sh              # системна симулация: всички устройства като процеси в общ час и общ радио ефир
python mutation_check.py     # умишлени грешки в кода - тестовете трябва да ги уловят
```

Тества РЕАЛНИТЕ `components/` и `apps/<приложение>/main` на `firmware_src/ESP32_PCB(temp_hum)_ESPIDF` срещу
`fake_idf/` - **симулатор на FreeRTOS във виртуално време** (задачи, приоритети, ядра, опашки, mutex, notify,
`esp_timer`; "chaos" режим за преплитане на задачите), **модел на SX127x**, GPIO с прекъсвания, SPI, I2C със SHT31/SHT21,
NVS, Wi-Fi + event loop, **MQTT брокер** (сесии, абонаменти, Last Will, outbox поведение на esp-mqtt) и mbedtls.

| Набор | Какво пази |
|---|---|
| `rtos` | самият симулатор (приоритети, preemption, опашки, таймаути, mutex, notify, esp_timer, детерминизъм на chaos) |
| `core` | крипто (golden вектори от `cryptography`, RFC 4493/NIST), JSON (парсер+валидатор+fuzz), NVS обвивка, ceiling/nonce (flash/RTC), radio_timing, channel_access, watchdog, dedup, hal (GPIO/IRQ), парсер на конфигурацията (всеки отрязан префикс се отхвърля) |
| `sx127x` | драйверът срещу модела на чипа: регистри, SF/BW/LDRO, честота, TX/RX/CAD, DIO0; **времената в radio_timing съвпадат с airtime на чипа за всички SF/BW/дължини** |
| `net` | wifi_link/mqtt_link: reconnect, 2-минутен рестарт, clean session, Last Will, QoS 1 абониране при ВСЯКО свързване, нищо в outbox без връзка |
| `sensor` | цикъл, отказ на всеки сензор поотделно, CRC/диапазон, дълбок сън, nonce през дълбок сън/power-on, неконфигурирано устройство |
| `executor` | boot (пинове LOW), restart response (3 опита/ACK), команди/ACK/NACK, чужди/повредени пакети, state request, heartbeat, канал зает, енергия (>99.9 % сън, събуждане по DIO0), nonce, chaos |
| `repeater` | препращане байт по байт на TX честота, dedup, опашка, прием по време на чакане на канала, heartbeat, енергия, chaos |
| `gateway` | задачи по ядра/приоритети, sensor->JSON, dedup, state response + ACK по радиото, команден път (ACK/NACK/TIMEOUT/повторения), канал, nonce, мрежови прекъсвания, watchdog, chaos |
| `executor_wifi` | `wifi_commands`, филтър по M_ID, статуси QoS 1, Last Will, restart state response веднъж на boot, мрежови прекъсвания |
| `config` | протокол READY/ACK/NACK, шум преди CFG, повреден/отрязан/прекалено дълъг ред, UTF-8 в SSID |
| `compat` (Python) | същите кадри през старата и новата реализация дават еднакви MQTT съобщения/ACK/пинове; wire форматът е еднакъв с независим Python еталон |
| `system` (Python) | ВСИЧКИ устройства заедно (отделни процеси, общ виртуален час, общ ефир с колизии и CAD): sensor→repeater→gateway→MQTT, MQTT→gateway→executor→ACK→MQTT, Wi-Fi executor, 6-часов ход; намери колизията на два repeater-а и синхронизирането на heartbeat-ите |

**Какво НЕ е тествано:** реален ESP-IDF API (компилация), реален SX127x/антена/колизии, реален ток и сън, реален
Wi-Fi/брокер, реален flash/NVS, истински паралелно изпълнение на двете ядра (chaos преплита на границите на
извикванията на FreeRTOS/периферията, не на всяка инструкция). Виж чеклиста за bench теста в README на папката.

**Забележка (Windows/MinGW):** `-static` (както при другите набори). `mutation_check.py` ползва Git Bash (не WSL
`bash.exe`) и временна папка без интервали (`run_espidf.sh` не цитира пътищата).

## Какво НЕ е тествано тук (и защо)

- **`sensor/radio_tx.cpp`, `executor/radio_io.cpp`, `repeater/radio_io.cpp`,
  `gateway/lora_handlers.cpp` — цялото поведение** не се изпълнява нативно (изисква реални
  LoRa/SPI/WiFi/PubSubClient/Preferences). За тях `syntax_check_radio.sh` пуска `g++ -fsyntax-only`
  (типове, декларации, остатъчни препратки към премахнати константи). Логиката, която ги
  ръководи (`radio_timing`, `channel_access`, `crypto_common`, dispatch по дължина), е
  тествана отделно.
- **`*/cad.cpp` (Channel Activity Detection)** — чете/пише реални SX127x регистри
  през SPI. Няма смисъл от нативна симулация; изисква реален чип.
- **`*/power_mgmt.cpp` (watchdog, дълбок сън)** — AVR-специфични регистри
  (`WDTCSR`, `sleep_cpu()` и т.н.), не съществуват извън реален AVR чип.
- **Реално радио поведение** (обхват, колизии, CAD timing на живо, airtime) — виж
  разговора за link budget/ToA изчисления; никаква софтуерна симулация тук не го
  покрива достоверно за тази конкретна употреба на чипа (register-level CAD хак).

## AES-128 верификация

`aes128.h` е самостоятелна реализация, написана специално за този test harness (НЕ е
копие на production библиотеката "Crypto"/rweather). Верифицирана веднъж срещу
официалния **FIPS-197 Appendix B** тестов вектор (`aes_selfcheck.cpp`) преди да бъде
използвана зад заместителите на `AES.h`/`CTR.h`/`AES_CMAC.h`. Целта е да тестваме
**логиката на `crypto_common.cpp`** (wire формат, nonce разделяне, обработка на
грешки) с истинска, коректна крипто примитива — не да преоткриваме сигурността на
самия AES/CMAC алгоритъм (отговорност на production библиотеките, established трети
код).

## Открити реални находки (не хипотетични)

Докато пусках тези тестове (не докато ги пишех), излязоха две находки, недокументирани
преди това - записани в `CHANGES.md`:
- Точка 13: `config_avr.ino` коментарният пример ползва consumer ID формат ("B1C2"),
  който реалният GUI валидатор би отхвърлил.
