/*
  cfg_parse.h - разбор и запис на конфигурацията (stage 1 на двуетапното качване).

  Python програмата (Firmware Uploader) праща по серийния порт ред:

      CFG:{"id":"CS001","freq":"433000000","freq_tx":"433500000","sf":"7","bw":"125000",
           "key":"<32 hex>",
           "consumers":[{"id":"V01","pin":"25"},{"id":"P01","pin":"26"}],
           "net":"1","wifi_ssid":"MyNet","wifi_password":"secret",
           "mqtt_ip":"192.168.4.2","mqtt_port":"1883","mqtt_user":"","mqtt_password":""}

  Само "id" е задължително. Незадължително "txp":"17" - изходна мощност в dBm (-9..12 - регулаторен таван за 433 MHz, до максимума на модула в
  board_pins.h); без него устройствата ползват 17 dBm. "net":"1" изисква wifi_ssid и mqtt_ip. Записва в NVS (namespace "cfg") със
  СЪЩИТЕ ключове и типове като Arduino config_esp32 firmware-а:
    id, freq, freqtx, sf, bw, txp (ново, u8 със знак), key, nc, c<i>i, c<i>p, wssid, wpass, mqip, mqport, mquser, mqpass
  Ключът "ceil" (nonce watermark) не се пипа тук.

  Разликите от Arduino версията (по-строго, не по-различно): числата се валидират (само цифри);
  пинът трябва да е използваем на платката (board_gpio_usable); над 10 консуматора и повтарящи се
  consumer id са NACK (преди се игнорираха тихо); парсерът е истински JSON парсер (кавичка/запетая в
  парола не го чупят) вместо търсене на подниз.
*/
#ifndef CFG_PARSE_H
#define CFG_PARSE_H


#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "cfg_limits.h"

#define CFG_MODULE_ID_LEN      6
#define CFG_CONSUMER_ID_LEN    4
#define CFG_MAX_CONSUMERS      10
#define CFG_LINE_MAX           2048

#define CFG_REASON_LEN 96

struct CfgReply {
  bool ack;
  char reason[CFG_REASON_LEN];      // причина при NACK
};

bool cfg_is_config_line(const char *line);

// Валидира ЦЕЛИЯ пакет и чак тогава записва в NVS (при NACK нищо не се променя).
void cfg_apply_line(const char *line, CfgReply *reply);

// Натрупва символи от серийния порт до нов ред. feed() връща true, когато в line() има цял ред.
// Прекалено дълъг ред се изхвърля целият (overflowed() съобщава за това веднъж).
class CfgLineBuffer {
 public:
  bool feed(int32_t c);
  const char *line() const { return buf_; }
  bool overflowed();

 private:
  char buf_[CFG_LINE_MAX + 1];
  size_t len_ = 0;
  bool overflow_ = false;
  bool reportOverflow_ = false;
};

#endif
