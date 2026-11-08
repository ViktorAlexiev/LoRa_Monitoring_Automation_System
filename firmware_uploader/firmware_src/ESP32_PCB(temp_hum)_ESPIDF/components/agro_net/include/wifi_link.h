/*
  wifi_link.h - Wi-Fi станция (esp_wifi + esp_netif + esp_event), със събитийно (не опресняващо) управление.

  * Свързва се при старт; при загуба на връзка опитва отново на всеки WIFI_RECONNECT_MS.
  * Ако няма връзка WIFI_RESTART_MS (2 мин) - рестартира прошивката (както в Arduino версията).
  * Изключен е режимът за пестене на ток (WIFI_PS_NONE): gateway и Wi-Fi executor са постоянно захранени
    и командите не бива да чакат DTIM интервал.
*/
#ifndef WIFI_LINK_H
#define WIFI_LINK_H

#include <stdint.h>
#include <stdbool.h>

#define WIFI_RECONNECT_MS   UINT32_C(5000)
#define WIFI_RESTART_MS     (UINT32_C(2) * UINT32_C(60) * UINT32_C(1000))

typedef void (*WifiGotIpFn)(void *ctx);   // вика се от системната event задача при всяко получаване на IP

bool wifi_link_start(const char *ssid, const char *password, WifiGotIpFn onGotIp, void *ctx);
bool wifi_link_connected(void);
// Колко ms няма връзка (0 ако е свързан). Отброява от старта, ако никога не е успял да се свърже.
uint32_t wifi_link_down_ms(void);

#endif
