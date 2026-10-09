#include <esp_idf_adapter/nvs_persistence.h>
namespace esp_idf_adapter {
using signal_contracts::Status;
namespace {
const char* key(unsigned slot) noexcept {return slot==0?"processing_a":"processing_b";}
Status status(esp_err_t error) noexcept {
    if(error==ESP_OK)return Status::ok;
    if(error==ESP_ERR_NVS_NOT_FOUND)return Status::unavailable;
    if(error==ESP_ERR_NVS_INVALID_LENGTH)return Status::buffer_too_small;
    if(error==ESP_ERR_NVS_NOT_ENOUGH_SPACE)return Status::exhausted;
    return Status::disconnected;
}
}
Status NvsProcessingStorage::read(unsigned slot,std::span<std::uint8_t> bytes,std::size_t& written) noexcept {
    if(slot>1 || !maximum_)return Status::invalid_argument;
    std::size_t size=0;auto result=nvs_get_blob(handle_,key(slot),nullptr,&size);
    if(result!=ESP_OK)return status(result);
    if(size>maximum_ || size>bytes.size())return Status::buffer_too_small;
    result=nvs_get_blob(handle_,key(slot),bytes.data(),&size);
    if(result!=ESP_OK)return status(result);
    if(size>bytes.size() || size>maximum_)return Status::buffer_too_small;
    written=size;return Status::ok;
}
Status NvsProcessingStorage::commit(unsigned slot,std::span<const std::uint8_t> bytes) noexcept {
    if(slot>1 || bytes.empty() || bytes.size()>maximum_)return Status::invalid_argument;
    auto result=nvs_set_blob(handle_,key(slot),bytes.data(),bytes.size());
    if(result!=ESP_OK)return status(result);
    return status(nvs_commit(handle_));
}
}
