#pragma once
// Preserve this host fixture's existing silent logging contract.
enum { ESP_LOG_ERROR = 1 };
#define _ESP_LOG_ENABLED(level) 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
