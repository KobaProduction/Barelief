#pragma once
#include "FreeRTOS.h"
extern "C" {
BaseType_t xQueueReceive(QueueHandle_t,void *,TickType_t);
BaseType_t xQueueSend(QueueHandle_t,const void *,TickType_t);
BaseType_t xQueueReset(QueueHandle_t);
QueueHandle_t xQueueCreateStatic(std::size_t,std::size_t,std::uint8_t *,StaticQueue_t *);
}
