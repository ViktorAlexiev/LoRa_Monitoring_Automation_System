# Промени по firmware-а — лог

Единен документ, обединяващ работния списък от прегледа и реалния лог на приложените промени
(преди в два отделни файла: `PLANNED_CHANGES.md` и `FIXED_ISSUES.md`).

Всичко по-долу със статус **приложено** е реално направено в кода (`firmware_src/`,
`config.ini`, `main.py`). Бекенд промените за разпознаване на новия формат на
`module_states_response` (restart state response) НЕ са включени — по тях потребителят работи
отделно.

---

## 1. CFG ред се отрязваше при Executor с 10 консуматора

**Статус:** приложено.

**Файл:** `firmware_src/config_avr/config_avr.ino`.

**Проблем:** компактният JSON CFG пакет за Executor с 10 консуматора е ~343 символа, над
лимита от 319 полезни символа на буфера `lineBuf[LINE_BUF_SIZE]` (бил 320). Байтовете над
лимита се изхвърляха тихо. Полето `key` е последно в JSON-а, затова беше първото засегнато —
AES ключът не се записваше в EEPROM, но firmware-ът пак връщаше `ACK`.

**Приложено решение:** `LINE_BUF_SIZE` вдигнат от 320 на 400 — достатъчен запас над най-дългия
реален ред.

---

## 2. Gateway dedup сравняваше по пълна дължина — не отрязваше trailing маркера

**Статус:** приложено.

**Файл:** `firmware_src/gateway/lora_handlers.cpp` — `gwDedupSeen()`/`gwDedupAdd()`.

**Проблем:** коментарът твърдеше, че trailing `REPEATED_MARKER` байтът (от Repeater, прави
пакета 31 B вместо 30 B) се "отрязва" преди dedup сравнение. Реално сравнението беше по пълна
дължина — директен (30 B) и препратен (31 B) вариант на едно и също измерване никога не се
разпознаваха като дубликат, потенциално двукратна публикация в MQTT `sensors`.

**Приложено решение:** `gwDedupBuf`/`gwDedupSeen`/`gwDedupAdd` пренаписани да пазят и сравняват
само първите `SENSOR_WIRE_LEN` (30) байта, независимо от приетата дължина (30 или 31).

---

## 3. `WDT_CYCLES_BETWEEN_SEND = 1` (Sensor) — интервал ~8s вместо ~15 мин

**Статус:** приложено.

**Файл:** `firmware_src/sensor/power_mgmt.h`.

**Проблем:** самият коментар в кода го документираше като известен, неоправен проблем — Sensor
мереше и предаваше на всеки ~8 сек вместо предвидените ~15 мин. Последици: ~112× по-често
будене, по-кратък живот на батерията, ~112× повече LoRa трансмисии, по-често писане в EEPROM.

**Приложено решение:** стойността сменена от `1` на `113` (113 × 8s ≈ 904s ≈ 15.07 мин).

---

## 4. `sht21_read` — ред на изчисление + липсваща диапазонна проверка

**Статус:** приложено.

**Файл:** `firmware_src/sensor/sensors_io.cpp`.

**Проблем 1:** `uint16_t raw = (Wire.read() << 8) | Wire.read();` — редът на оценка на
операндите не е дефиниран от C++; `Wire.read()` има странични ефекти (консумира байт от
буфера), затова е възможна размяна на MSB/LSB при друг компилатор/оптимизация.

**Проблем 2:** `sensors_read()` пишеше sentinel `255.0f` само при I2C грешка, не и при
физически невъзможна стойност (напр. от евентуална размяна на байтове).

**Приложено решение:**
- MSB/LSB четенето разделено в отделни редове (`uint16_t msb = Wire.read(); uint16_t lsb =
  Wire.read();`), премахва зависимостта от реда на оценка.
- Добавена диапазонна проверка в `sht21_read()` (-40..85°C, 0..100% RH) — стойност извън тези
  граници вече връща `false`, `sensors_read()` пише sentinel `255.0f`.
- Добавени Serial предупреждения при всяка от трите възможни грешки на четене.

---

## 5. MQTT буфер (PubSubClient) можеше да не побере state response с много консуматори

**Статус:** приложено.

**Файл:** `firmware_src/gateway/wifi_mqtt.cpp`.

**Проблем:** `PubSubClient` има собствен вътрешен буфер за целия MQTT пакет, по подразбиране
често 256 B в разпространените версии на библиотеката. State response JSON с ~10 консуматора
(~190-200 B payload + топик + overhead) можеше да доближи/надхвърли лимита; `publish()` връща
`false` при неуспех, но резултатът не се проверяваше — съобщението тихо не достигаше бекенда.

**Приложено решение:**
- `mqttClient.setBufferSize(512)` в `mqtt_wifi_setup()`, преди първото `connect()`.
- `publishJson()` вече проверява резултата от `publish()` и логва грешка по Serial при неуспех.

---

## 6. Несъответствия в коментари/съобщения

**Статус:** приложено (четирите подточки).

- **A.** `config_avr.ino` — "Module ID (5 bytes...)" → "6 bytes" (реалната стойност на
  `MODULE_ID_LEN`).
- **B.** `main.py` (~ред 553-554) — коментарите вече сочат правилните EEPROM адреси 67/71
  вместо остарелите 66/70.
- **C.** `executor/executor.ino` — вместо фиксиран низ `"M_ID=M01"` при всеки boot, вече печата
  реалния `MY_M_ID`, зареден от EEPROM.
- **D.** `repeater/radio_io.h` — коментарът за `REPEATED_MARKER` вече не твърди невярна връзка
  с `gateway/packets.h` (константата никога не е била дефинирана там).

---

## 7. Резервна честота на Sensor — 434 MHz → 433 MHz

**Статус:** приложено.

**Файл:** `firmware_src/sensor/config_storage.h`.

**Проблем:** единствената резервна честота на Sensor (ползвана при неконфигуриран/изтрит
EEPROM) сочеше 434 MHz — честотата "Sensor→Repeater", не "до Gateway" (433 MHz). При
повреден EEPROM Sensor падаше по подразбиране в режим, сякаш винаги минава през Repeater.

**Приложено решение:** `LORA_FREQ_DEFAULT_HZ` сменена от `434000000UL` на `433000000UL`.

---

## 8 + 11 (firmware частта). Криптиран state channel + restart state response + CAD навсякъде

**Статус:** приложено (firmware частта). Бекенд разпознаването на новия формат остава отделно.

Най-голямата промяна в сесията — засяга `crypto_common.h` (4 идентични копия),
`executor/radio_io.h/.cpp`, `executor/executor.ino`, `executor/packets.h`,
`gateway/lora_handlers.h/.cpp`, `gateway/packets.h`, `gateway/config_storage.h`, плюс нов
`cad.h/.cpp` в `sensor/`, `executor/`, `gateway/` (Repeater вече го имаше).

**Проблем (точка 8):** `StateRequestPacket`/`StateResponsePacket` се предаваха в чист вид, без
AES/CMAC — позволяваше пасивно подслушване на състоянието на консуматорите и активно подправяне
на отговора (Gateway приемаше съдържание само по съвпадение на `M_ID`, без проверка за
автентичност).

**Приложено решение (точка 8):**
- Нови crypto type-id-та в `crypto_common.h`: `CRYPTO_TYPE_STATE_REQ` (6), `CRYPTO_TYPE_STATE_RESP`
  (7), `CRYPTO_TYPE_STATE_RESP_RESTART` (8), `CRYPTO_TYPE_STATE_RESP_ACK` (9).
- `CRYPTO_MAX_PT` вдигнат от 32 на 50 (state response с 10 консуматора = 50 B plaintext).
- Нов wire формат: state request (Gateway→Executor, 20 B, target M_ID + крипто wire с празен
  plaintext); state response (Executor→Gateway, 14+5×N B, N=брой консуматори); state-resp ACK
  (Gateway→Executor, 20 B, само за RESTART варианта).
- Gateway разпознава входящи state response пакети по `(len - CRYPTO_OVERHEAD) % 5 == 0`, без
  числена колизия с останалите типове.

**Контекст (точка 11, от прегледа на поведението "LOW при рестарт на Executor" — виж раздел
"Съзнателно непроменени неща" по-долу):** Executor слага всички консуматорски пинове на LOW при
всеки `setup()` — това поведение остава непроменено (безопасен избор), но вече се добавя
видимост към бекенда за точния момент на рестарт.

**Приложено решение (точка 11, firmware страна):**
- `executor/executor.ino`: веднага след `radio_setup()` в `setup()` се вика
  `sendRestartStateResponse()`. `loop()` вика `restartRespManager()` на всяка обиколка;
  `restart_response_pending()` пречи на устройството да заспи, докато чака ACK.
- Retry на всеки 4 секунди (`STATE_RESP_ACK_TIMEOUT_MS = 4000`, същия интервал като Gateway
  командния retry), до 3 общо опита. След неуспех — Executor се отказва мълчаливо.
- `gateway/lora_handlers.cpp`: `handleStateResponse()` публикува JSON в
  `module_states_response` **абсолютно еднакво** за нормален и RESTART вариант (без маркер в
  MQTT payload-а). За RESTART варианта допълнително праща `sendStateRespAck()` обратно.

**CAD (Channel Activity Detection) навсякъде:** преди съществуваше само в Repeater. Копиран
`cad.h/.cpp` в `sensor/`, `executor/`, `gateway/` (изисква патчнат `LoRa.h` с public
`readRegister`/`writeRegister` — същото изискване, което важеше за Repeater; трябва да е
наличен глобално в Arduino library инсталацията, ползвана от `arduino-cli`, за всички build
target-и). CAD вече проверява канала преди: Sensor TX, Executor ACK/NACK/HB/state response,
Gateway команда/state request/state-resp ACK.

---

## 9. Диагностика при неуспешен `LoRa.begin()` — унифицирано поведение

**Статус:** приложено.

**Файлове:** `sensor/radio_tx.cpp`, `executor/radio_io.cpp`, `repeater/radio_io.cpp`,
`gateway/lora_handlers.cpp`.

**Проблем:** четирите устройства се държаха различно при неуспешна инициализация на LoRa
модула — тих безкраен hang (Repeater), заливащ Serial без пауза (Executor), никаква проверка
(Sensor), или почти правилно поведение (Gateway).

**Приложено решение:** и четирите вече правят едно и също — цикъл, който извиква `LoRa.begin()`
многократно и печата диагностично съобщение по Serial на всеки 10 секунди
(`LORA_BEGIN_RETRY_MSG_MS = 10000`).

**Допълнително открито и поправено извън първоначалния списък:** Repeater изобщо не викаше
`Serial.begin()` никъде — сериен дебъг там беше напълно невъзможен. Добавен
`Serial.begin(115200)` в `repeater.ino` `setup()`, плюс нови информативни съобщения при
прием/препращане/heartbeat в `repeater/radio_io.cpp`.

---

## 10. Резервирани пинове (+11, 12, 13) + бележка в Settings

**Статус:** приложено.

**Файлове:** `firmware_uploader/config.ini`, `firmware_uploader/main.py`.

**Проблем:** `reserved_pins` пазеше само логическите пинове на LoRa модула (2, 9, 10), не и
хардуерния SPI на ATmega328P (SCK=13, MISO=12, MOSI=11), фиксиран независимо от
`LoRa.setPins()`. Консуматор на пин 11/12/13 минаваше валидацията, но реално конфликтираше с
LoRa комуникацията.

**Приложено решение:**
- `config.ini`: `[reserved_pins] pins = 2,9,10,11,12,13`.
- `main.py` (Settings таб): добавена бележка под полето, обясняваща че 11/12/13 са хардуерният
  SPI за LoRa чипа.

---

## 12. Радио-ниво CRC (`LoRa.enableCrc()`) — включен еднакво навсякъде

**Статус:** приложено.

**Файлове:** `sensor/radio_tx.cpp`, `repeater/radio_io.cpp`, `gateway/lora_handlers.cpp`
(`executor/radio_io.cpp` вече го имаше).

**Проблем:** само Executor викаше `LoRa.enableCrc()` — хардуерна CRC проверка на SX127x за
полезния товар, независима от AES-CMAC-а на приложно ниво. Sensor, Repeater и Gateway приемаха
повредени по въздуха пакети чак до `cryptoParseWirePacket()`, разчитайки изцяло на MAC-а да
хване повредата (работи, но е допълнителен пропилян радио цикъл за нещо, което CRC-то би
хванало по-рано и по-евтино).

**Приложено решение:** `LoRa.enableCrc()` добавен в `radio_setup()`/`lora_init()`/
`lora_radio_setup()` на трите останали устройства, веднага след `setSyncWord()`.

**Ефект върху airtime:** незначителен — worst-case пакетът (64 B, Executor state response) има
идентично Time-on-Air с и без CRC (118.016 ms и в двата случая, SF7/BW125). По-малките пакети
удължават с 0-5 ms в зависимост от точната им дължина (виж изчисленията в разговора за детайли).

---

## 13. Подвеждащ пример в `config_avr.ino` коментара за consumer ID формат

**Статус:** приложено — намерено чрез нов автоматичен тест (`tests/python/test_validation.py`),
коментарът в `config_avr.ino` е поправен (примерът вече ползва "AB12"/"CD34").

**Файл:** `firmware_src/config_avr/config_avr.ino` (коментар, ред ~9); реалната валидация е в
`firmware_uploader/validation.py` (`ID_PATTERN`) и `main.py:669`.

**Находка:** коментарният пример в `config_avr.ino` показва
`CFG:{"id":"CS001","consumers":[{"id":"B1C2","pin":"A3"},{"id":"D4E5","pin":"10"}]}` —
consumer ID-та "B1C2"/"D4E5" са смесен формат (буква-цифра-буква-цифра). Реалният валидатор
в приложението (`ID_PATTERN = r'^[A-Z]+[0-9]+$'`, споделен за module ID и consumer ID)
изисква **всички букви в началото, после всички цифри** (напр. "AB12") — GUI-то дори изрично
го документира така в текста си (main.py:669: "главни букви в началото, после цифри").
Потвърдено с автоматичен тест: `validate_consumer_id("B1C2")` връща `False`.

**Не е бъг във `validation.py`** — валидаторът и GUI помощният текст са последователни
помежду си. Проблемът е единствено остарял/грешен пример в код коментара на `config_avr.ino`,
който технически никога не би минал през реалния GUI на приложението.

**Приложено решение:** примерът в коментара сменен на "AB12"/"CD34", за да не подвежда бъдещ
разработчик, че смесен формат е поддържан.

---

## 14. SF/BW като централна настройка (uploader → EEPROM/NVS → firmware)

**Статус:** приложено (firmware + uploader + тестове). Библиотеките на тази машина не са
компилирани с `arduino-cli` (виж бележката най-долу) — проверено с native тестове и syntax check.

- `config.ini [lora]`: `sf` (7–12), `bw_khz` (62.5 / 125 / 250), еднакви за цялата мрежа.
  Settings таб: комбобокси за SF и BW; валидация; предупредителен диалог при смяна (засяга
  всички устройства — нужен е нов config + firmware upload на всяко).
- CFG пакетът носи `sf` и `bw` (Hz) за ВСИЧКИ типове устройства, вкл. Gateway.
- Съхранение: AVR EEPROM адрес **95 = SF**, **96 = BW индекс** (0 = 62.5k, 1 = 125k, 2 = 250k);
  празна (0xFF)/невалидна стойност → фабрична SF7/125 kHz. Gateway NVS (namespace `cfg`):
  `sf` (UChar), `bw` (ULong, Hz).
- `config_avr.ino` / `config_esp32.ino`: парсват и валидират `sf`/`bw` (NACK при невалидни);
  ако липсват в пакета, старите стойности в EEPROM/NVS не се пипат.
- Премахнати фиксираните макроси `LORA_SF` / `LORA_BANDWIDTH_HZ` от `radio_tx.h`,
  `radio_io.h` (executor, repeater), `lora_handlers.h`; заменени с глобалите `LORA_SF`,
  `LORA_BW_HZ`, прочетени от `config_storage`.
- **LDRO:** библиотеката LoRa 0.8.0 (`setLdoFlag`) съкращава при SF11/BW125, затова
  `radioApplyModemSettings()` (`cad.cpp`) задава `RegModemConfig3` (0x26) бит 3 изрично
  (LDRO при Ts > 16 ms) след SF/BW/CR/preamble.

## 15. Всички времена се извеждат от един модул `radio_timing`

**Статус:** приложено; покрито с `test_radio_timing` (9216 комбинации срещу независима формула).

Нов модул `radio_timing.h/.cpp` (идентично копие във sensor/, executor/, repeater/, gateway/):
Time-on-Air по Semtech AN1200.13 в целочислена аритметика, символно време, LDRO. Всичко
производно от SF/BW, **без фиксирани стойности и без 4000 ms праг:**

| Величина | Формула |
|---|---|
| CAD timeout | 2×(2^SF+32)/BW + 5 ms |
| CAD стъпка | max(CAD timeout, 4×Ts) |
| Sensing прозорец (команди) | toa(64 B) |
| Чакане при телеметрия | toa(30 B) |
| Слот (backoff) | стъпката |
| ACK timeout на команда | 1.5×(toa26 + 500 + toa20) + 2×toa64 |
| ACK timeout на restart отговор | 1.5×(toa64 + 200 + toa20) + 2×toa64 |
| Timeout на state request | 1.5×(toa20 + 200 + toa64) + 2×toa64 |

`ACK_TIMEOUT_MS`, `CAD_TIMEOUT_MS`, `STATE_RESP_ACK_TIMEOUT_MS` са премахнати; те и всички
съответни повторения използват функциите `radioAckTimeout*Ms()`.
(Изпълнява бившите задачи „CAD_TIMEOUT да се мащабира със SF“ и „ACK timeout със SF“.)

## 16. Неблокиращ достъп до канала (`channel_access`)

**Статус:** приложено; 13 native теста (вкл. `millis()` wraparound), максимум един CAD на `poll`.

Модул `channel_access.h/.cpp` — автомат със инжектирани CAD/random функции (тестваем):

- **Команден път** (Gateway команда / state request / state-resp ACK; Executor ACK/NACK,
  state response, restart response): първи CAD веднага. Ако е зает → неблокиращо наблюдение
  (един CAD на стъпка, докато мине прозорецът toa(64)); после случаен backoff 0–3 слота и
  краен CAD; при изтичане — отказ (по-горният слой повтаря).
- **Телеметрия** (Sensor, HB, препращания на Repeater): CAD; ако е зает — едно случайно
  чакане ≤ toa(30) и втори CAD; още зает → `FORCE` (Sensor/препращане предава въпреки това)
  или `SKIP` (HB се пропуска).
- Gateway: опашка за изпращане с 4 места (`gwTxEnqueue`/`gwTxTick`), извиквана първа в
  `lora_managers_tick()`; `waitForClearChannel` премахната.
- Executor: единичен TX слот (ACK / STATE_RESP / RESTART_RESP / HB), тикан от `radioTxTick()`
  в `loop()`; sleep се разрешава само при `!radio_tx_busy()`.
- Repeater: един собственик на достъпа (препращане или HB), CAD се прави на TX честотата;
  `enter_rx_mode()` само след като CAD реално е пуснат.
- `cad.cpp`: `channelActive()` ползва `radioCadTimeoutMs()`.

## 17. Ленти (lanes) и верига от Repeater-и (само uplink); премахнат маркерът 0xD1

**Статус:** приложено (firmware + uploader).

- **Модел:** лента 0 = Gateway (на нея слушат Gateway и Executor-и), лента 1 = първо ниво
  Sensor→Repeater, лента 2… = следващи нива (`extra_lanes_mhz`). Repeater с входна лента K
  слуша на K и предава на K−1. Sensor избира лента; Executor/Gateway ползват лента 0.
  Разстояние между ленти ≥ 2×BW (при BW 125 kHz ≥ 0.25 MHz; препоръчително ~0.5 MHz),
  най-много 6 ленти, валидни EU честоти, без повторения.
- **Firmware:** `REPEATED_MARKER` (0xD1) премахнат — Repeater само dedup-ва и препраща на
  другата честота, Gateway приема само 30-байтови сензорни пакети (`len == SENSOR_WIRE_LEN`).
  Гарантира приключване на веригата, защото няма честотен цикъл (uplink-only).
- **Uploader:** `validation.py` — `validate_lora_sf`, `validate_lora_bw_khz`,
  `parse_extra_lanes`, `validate_lora_lanes`; `main.py` — Sensor избира лента, Repeater —
  входна лента (RX = лента K, TX = K−1, записани в EEPROM 67 / 71), инфо ред със SF/BW,
  Settings: допълнителни ленти, SF, BW + валидация и предупреждение. Регистърът/логът записват
  `lane`, `freq_hz`, `sf`, `bw_hz`.
- Приетият риск „0xD1 колизия с CMAC“ **отпада** (маркерът не съществува).

## Тестове и проверки (по този етап)

`firmware_uploader/tests/`: native C++ (`run_all.sh`) — crypto, ceiling, queues, consumers,
dedup, config_storage ×3, sensors_io, radio_timing, channel_access, length dispatch; python
(`unittest`, 67 теста за `validation.py`); `syntax_check_radio.sh` — g++ `-fsyntax-only` на
`radio_tx.cpp`, `radio_io.cpp` ×2, `lora_handlers.cpp`. Всички минават.

**Бележка (тази машина):** инсталираните копия на LoRa нямат публичните `readRegister`/
`writeRegister` (нужни за CAD), а Crypto/AES_CMAC не са намерени; ArduinoJson е 7.x, а
Gateway ползва v6 API. Библиотеките ще се оправят на лаптопа; после — реален
`arduino-cli compile` на всички скечове.

---

## Съзнателно непроменени неща (приети рискове)

Пълното описание на тези точки, с конкретни сценарии как биха могли да бъдат експлоатирани по
радиото, е в `Security_Accepted_Risks.docx`. Тук са само кратко изброени:

- **Липса на replay защита** — нито Executor, нито Gateway проверяват дали counter-ът на
  подателя нараства.
- **Нулиране на `cmdceil`** (nonce брояч на командите на Gateway) при пре-конфигуриране, заради
  начина по който `config_esp32.bin` (4 MB merged образ) презаписва NVS зоната.
- **Кратък MAC (4 от 16 байта AES-CMAC)** и `memcmp` без постоянно време при проверка.
- **Единен споделен мрежов ключ** за цялата мрежа — компрометиране на едно устройство
  компрометира цялата система.
- **Jamming (радио заглушаване)** — присъщо ограничение на LoRa/ISM протоколите. Системата има
  ръчно управление на консуматорите, независимо от радиото, за случай на повреда/заглушаване; в
  допълнение земеделието не е процес, изискващ детерминистична обработка в реално време, и на
  обект от този тип обичайно има човек, който следи за проблеми и може да действа при нужда.
- **Състоянието на консуматорите не се пази при рестарт на Executor** (`executor.ino` слага
  всички пинове на LOW при `setup()`) — съзнателен избор за безопасност ("спри и чакай изрична
  команда"). Логиката остава непроменена; добавена е само видимост за бекенда чрез restart
  state response (виж точка 8+11 по-горе).
