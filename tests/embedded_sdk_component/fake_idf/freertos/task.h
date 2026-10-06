#pragma once
#include "FreeRTOS.h"
extern "C" {
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
TaskHandle_t xTaskCreateStatic(void (*)(void *),const char *,std::uint32_t,void *,unsigned,StackType_t *,StaticTask_t *);
}
