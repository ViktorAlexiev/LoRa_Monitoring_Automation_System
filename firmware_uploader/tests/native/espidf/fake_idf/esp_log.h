#ifndef FAKE_ESP_LOG_H
#define FAKE_ESP_LOG_H

#include <stdarg.h>
#include "esp_err.h"

// "" fmt: форматът ТРЯБВА да е текстов литерал (реалният ESP_LOGx го конкатенира с префикса) - иначе не се компилира
// Логовете се събират в sim::log_lines() (и се печатат, ако е включено SIM_LOG_STDOUT=1)
void sim_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) sim_log('E', tag, "" fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) sim_log('W', tag, "" fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) sim_log('I', tag, "" fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) sim_log('D', tag, "" fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) sim_log('V', tag, "" fmt, ##__VA_ARGS__)

#endif
