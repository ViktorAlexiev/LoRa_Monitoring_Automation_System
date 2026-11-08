#ifndef FAKE_ESP_ERR_H
#define FAKE_ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK                    0
#define ESP_FAIL                  (-1)
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_NVS_NOT_FOUND         0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES     0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x110e
#define ESP_ERR_NVS_READ_ONLY         0x1107
#define ESP_ERR_NVS_INVALID_LENGTH    0x110c
#define ESP_ERR_NVS_TYPE_MISMATCH     0x1103

const char *esp_err_to_name(esp_err_t e);

#endif
