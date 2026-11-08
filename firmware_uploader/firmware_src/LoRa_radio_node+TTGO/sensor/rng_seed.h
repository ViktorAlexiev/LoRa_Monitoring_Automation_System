#ifndef RNG_SEED_H
#define RNG_SEED_H

#include <Arduino.h>

// Seed на random() от нещо УНИКАЛНО за устройството (ID + стойността на nonce брояча от EEPROM) + micros().
// randomSeed(micros()) сам дава почти един и същ seed при всички устройства със същата прошивка (стартът е
// детерминиран), а jitter-ът/backoff-ът е точно за да разминава устройствата.
// FNV-1a 32 бита върху ID (id[0..idLen-1]) и 4-те байта на брояча.
inline void seedRandomUnique(const uint8_t *id, uint8_t idLen, uint32_t counter) {
  uint32_t h = UINT32_C(2166136261);
  for (uint8_t i = 0; i < idLen; i++) { h ^= id[i]; h *= UINT32_C(16777619); }
  for (uint8_t i = 0; i < 4; i++) { h ^= (uint8_t)(counter >> (8 * i)); h *= UINT32_C(16777619); }
  h ^= micros();
  randomSeed(h ? h : 1);   // randomSeed(0) е валиден, но на някои ядра го третира като "без seed"
}

#endif
