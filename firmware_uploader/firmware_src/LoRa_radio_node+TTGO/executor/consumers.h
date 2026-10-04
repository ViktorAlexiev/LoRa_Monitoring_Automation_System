#ifndef CONSUMERS_H
#define CONSUMERS_H

#include <Arduino.h>

bool consumerExists(const char* c_id);
int consumerIndex(const char* c_id);

#endif
