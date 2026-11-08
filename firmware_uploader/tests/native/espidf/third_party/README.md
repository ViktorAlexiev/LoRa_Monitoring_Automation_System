# Външен код за хост тестовете

Само за тестовете на компютър (прошивката ползва същите библиотеки от ESP-IDF / Component Registry).

| Библиотека | Версия | Откъде | Лиценз |
|---|---|---|---|
| RadioLib `src/` | 7.8.1 | https://github.com/jgromes/RadioLib (таг 7.8.1, архив codeload) - без `modules/LR11x0/firmware` (не се ползва) | MIT (`RadioLib/license.txt`) |
| cJSON | 1.7.14 | копие от `Arduino_JSON` на разработчика (`src/cjson`); ESP-IDF носи свое cJSON със същото API | MIT (`cjson/LICENSE`) |

Тестовете компилират РЕАЛНИЯ RadioLib (включително неговия `EspHal` за ESP-IDF) срещу заместителите на ESP-IDF в `fake_idf/` и
модел на SX1262 върху SPI командите - не заместител на RadioLib.
