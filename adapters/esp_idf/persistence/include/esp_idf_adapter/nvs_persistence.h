#pragma once
#include <embedded_support/persistence.h>
#include <nvs.h>
namespace esp_idf_adapter {
// Host opens/closes namespace and initializes NVS. This adapter never erases
// a partition on errors. Handle access is serialized in a control task.
class NvsProcessingStorage final : public embedded_support::ProcessingStoragePort {
public:
    explicit NvsProcessingStorage(nvs_handle_t handle, std::size_t max_record_bytes) noexcept
        :handle_(handle),maximum_(max_record_bytes) {}
    signal_contracts::Status read(unsigned,std::span<std::uint8_t>,std::size_t&) noexcept override;
    signal_contracts::Status commit(unsigned,std::span<const std::uint8_t>) noexcept override;
private:
    nvs_handle_t handle_;
    std::size_t maximum_;
};
}
