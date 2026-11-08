#include <stdio.h>
#include "config_app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "agro_hal.h"

static const char *TAG = "config";

static int32_t consoleGetChar(void) { return getchar(); }   // EOF (-1), ако няма данни (конзолата не блокира)
static void consolePrint(const char *line) {
  printf("%s\n", line);
  fflush(stdout);
}

extern "C" void app_main(void) {
  if (!hal_nvs_init()) {
    printf("NACK:NVS не може да се инициализира\n");
    return;
  }
  ConfigIo io = {consoleGetChar, consolePrint};
  ConfigResult r = config_run(&io, CONFIG_TIMEOUT_MS, CONFIG_READY_MS);
  if (r == CONFIG_ACKED) ESP_LOGI(TAG, "конфигурацията е записана");
  else ESP_LOGW(TAG, "няма конфигурация");
  // config-firmware не прави нищо друго - реалното приложение се качва отгоре
  vTaskDelay(portMAX_DELAY);
}
