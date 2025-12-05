#ifndef RTT_MUTEX_H
#define RTT_MUTEX_H

#include "FreeRTOS.h"
#include "semphr.h"

// shared mutex for RTT
extern SemaphoreHandle_t rtt_mutex;

#endif
