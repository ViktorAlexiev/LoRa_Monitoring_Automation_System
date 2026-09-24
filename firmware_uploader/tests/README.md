# Тестове на firmware/uploader логиката

Нативни (C++, чрез `g++`) и Python тестове върху **реалните** `.cpp`/`.py` файлове от
`firmware_src/` и `firmware_uploader/` — не преписани копия. Компилират се директно
срещу изходния код, с мокнати Arduino/EEPROM/Wire библиотеки, за да могат да се
пуснат на обикновен компютър, без реален AVR/ESP32 хардуер.

## Как да пуснеш всичко

```bash
cd tests/native && ./run_all.sh
```
```bash
cd tests/python && python -m unittest test_validation -v
```
(на тази машина Python е на `C:\Users\alexi\AppData\Local\Programs\Python\Python39\python.exe`,
не в PATH на Git Bash — извикай с пълния път, ако `python`/`python3` не се намира).

## Какво реално се тества (748 native + 56 python = 804 проверки)

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
| `firmware_uploader/validation.py` | ✅ Пълно | Всички публични валидатори - module/consumer ID, pin (вкл. резервираните 11/12/13), консуматорски списък (дубликати, резервирани пинове, конфликт с активен executor), WiFi/MQTT полета, LoRa честота (вкл. границите на EU обхватите). |

## Какво НЕ е тествано тук (и защо)

- **`sensor/radio_tx.cpp`, `executor/radio_io.cpp`, `repeater/radio_io.cpp`,
  `gateway/lora_handlers.cpp` — цялите файлове** не се компилират нативно. Изискват
  реалистични LoRa.h/SPI.h/WiFi.h/PubSubClient.h/ArduinoJson.h/Preferences.h мокове —
  голям обем работа с ограничена добавена стойност, защото рисковата логика вътре в
  тях (crypto извиквания, wire дължини) вече е покрита индиректно от
  `test_crypto_common.cpp` и `test_length_dispatch.cpp`.
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
