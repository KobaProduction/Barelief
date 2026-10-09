#pragma once
#include <esp_err.h>
#include <cstddef>
#include <cstdint>
using nvs_handle_t=std::uint32_t;
enum nvs_open_mode_t {NVS_READONLY,NVS_READWRITE};
inline constexpr int ESP_ERR_NVS_NOT_FOUND=1,ESP_ERR_NVS_INVALID_LENGTH=2,ESP_ERR_NVS_NOT_ENOUGH_SPACE=3;
esp_err_t nvs_open(const char*,nvs_open_mode_t,nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,std::size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,std::size_t);
esp_err_t nvs_commit(nvs_handle_t);
