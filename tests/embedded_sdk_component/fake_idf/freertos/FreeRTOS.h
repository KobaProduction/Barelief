#pragma once
#include <cstddef>
#include <cstdint>
using TickType_t=std::uint32_t;
using StackType_t=std::uint32_t;
using BaseType_t=int;
struct StaticQueue_t {std::uint8_t opaque[80];};
struct StaticTask_t {std::uint8_t opaque[256];};
using QueueHandle_t=void *;
using TaskHandle_t=void *;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
