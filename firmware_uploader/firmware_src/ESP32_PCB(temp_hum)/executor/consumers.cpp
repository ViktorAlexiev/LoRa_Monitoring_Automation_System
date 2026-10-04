#include "consumers.h"
#include "config_storage.h"

bool consumerExists(const char* c_id) {
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++)
    if (strncmp(consumerList[i], c_id, 4) == 0) return true;
  return false;
}

int consumerIndex(const char* c_id) {
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++)
    if (strncmp(consumerList[i], c_id, 4) == 0) return i;
  return -1;
}
