#include "config_app.h"

#include <stdio.h>
#include <string.h>
#include "cfg_parse.h"
#include "agro_hal.h"

static CfgLineBuffer s_line;      // 2 KB - статичен (не на стека)

ConfigResult config_run(const ConfigIo *io, uint32_t timeoutMs, uint32_t readyIntervalMs) {
  s_line = CfgLineBuffer();
  uint32_t start = hal_millis();
  uint32_t lastReady = start - readyIntervalMs;     // първият READY веднага

  while ((uint32_t)(hal_millis() - start) < timeoutMs) {
    // "READY" на всеки 500 ms, за да знае Python кога прошивката реално слуша
    if ((uint32_t)(hal_millis() - lastReady) >= readyIntervalMs) {
      io->print("READY");
      lastReady = hal_millis();
    }

    int32_t c = io->getChar();
    if (c < 0) {
      hal_delay_ms(10);
      continue;
    }
    if (!s_line.feed(c)) {
      if (s_line.overflowed()) io->print("NACK:ред твърде дълъг");
      continue;
    }
    const char *line = s_line.line();
    if (!cfg_is_config_line(line)) continue;        // други редове (лог на boot-а и т.н.) се игнорират

    CfgReply r;
    cfg_apply_line(line, &r);
    if (r.ack) {
      io->print("ACK");
      return CONFIG_ACKED;
    }
    char out[CFG_LINE_MAX / 16];
    snprintf(out, sizeof(out), "NACK:%s", r.reason);
    io->print(out);
  }
  io->print("NACK:Timeout - няма CFG пакет в рамките на прозореца");
  return CONFIG_TIMED_OUT;
}
