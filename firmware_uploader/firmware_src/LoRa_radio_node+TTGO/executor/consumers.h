#ifndef CONSUMERS_H
#define CONSUMERS_H

#include <Arduino.h>

bool consumerExists(const char* c_id);
int8_t consumerIndex(const char* c_id);   // -1 ако няма такъв

#endif
