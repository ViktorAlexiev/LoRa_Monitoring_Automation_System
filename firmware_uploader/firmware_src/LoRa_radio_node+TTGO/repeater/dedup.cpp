#include "dedup.h"
#include <string.h>

static uint8_t dedupBuf[DEDUP_BUFFER_SIZE][DEDUP_MAX_LEN];
static uint8_t dedupLen[DEDUP_BUFFER_SIZE] = {0};
static uint8_t dedupHead = 0;

bool dedupSeen(const uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < DEDUP_BUFFER_SIZE; i++) {
    if (dedupLen[i] == len && memcmp(dedupBuf[i], data, len) == 0) return true;
  }
  return false;
}

void dedupAdd(const uint8_t *data, uint8_t len) {
  if (len > DEDUP_MAX_LEN) len = DEDUP_MAX_LEN;
  memcpy(dedupBuf[dedupHead], data, len);
  dedupLen[dedupHead] = len;
  dedupHead = (dedupHead + 1) % DEDUP_BUFFER_SIZE;
}
