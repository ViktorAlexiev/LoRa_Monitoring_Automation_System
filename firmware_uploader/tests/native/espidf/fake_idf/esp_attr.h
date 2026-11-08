#ifndef FAKE_ESP_ATTR_H
#define FAKE_ESP_ATTR_H
// IRAM_ATTR / RTC_DATA_ATTR: на хоста са празни. RTC паметта се имитира от самите статични променливи
// (те оцеляват между "събужданията" в един и същ процес; при power-on тестът пуска нов процес).
#define IRAM_ATTR
#define RTC_DATA_ATTR
#endif
