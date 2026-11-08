/*
  dedup.h - кръгов буфер за дубликати на радио пакети (repeater: 6 x 32 B, gateway: 8 x 64 B).

  Пази суровите wire байтове (ciphertext+counter+tag са различни при всяка трансмисия благодарение
  на nonce-а), без timestamp: най-старият запис се презаписва при нов. Директният и препратеният от
  Repeater пакет са байт по байт еднакви, затова дубликатът се познава със сравнение на цялото
  съдържание заедно с дължината. Не е thread-safe - ползва се само от задачата, която приема пакетите.
*/
#ifndef DEDUP_H
#define DEDUP_H

#include <stdint.h>
#include <string.h>

template <uint8_t N, uint8_t MAXLEN>
class DedupRing {
 public:
  bool seen(const uint8_t *data, uint8_t len) const {
    if (len == 0) return false;
    for (uint8_t i = 0; i < N; i++) {
      if (len_[i] == len && memcmp(buf_[i], data, len) == 0) return true;
    }
    return false;
  }

  void add(const uint8_t *data, uint8_t len) {
    if (len > MAXLEN) len = MAXLEN;
    memcpy(buf_[head_], data, len);
    len_[head_] = len;
    head_ = (uint8_t)((head_ + 1) % N);
  }

 private:
  uint8_t buf_[N][MAXLEN] = {};
  uint8_t len_[N] = {};
  uint8_t head_ = 0;
};

#endif
