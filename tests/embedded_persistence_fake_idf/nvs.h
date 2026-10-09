#pragma once
#include <cstddef>
#include <cstdint>
using nvs_handle_t=std::uint32_t;
using esp_err_t=int;
inline constexpr int ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=1, ESP_ERR_NVS_INVALID_LENGTH=2,
    ESP_ERR_NVS_NOT_ENOUGH_SPACE=3, ESP_FAIL=4;
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,std::size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,std::size_t);
esp_err_t nvs_commit(nvs_handle_t);
