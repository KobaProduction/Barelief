#pragma once
#include <signal_contracts/data_contract.h>

namespace domain {
// Immutable metadata snapshot. Copies share ownership; view() rebases borrowed fields.
// No payload, actual coordinates, DSP state or presentation fields belong here.
class OwnedProcessingContract {
public:
    OwnedProcessingContract() noexcept = default;
    OwnedProcessingContract(const OwnedProcessingContract&) noexcept;
    OwnedProcessingContract& operator=(const OwnedProcessingContract&) noexcept;
    OwnedProcessingContract(OwnedProcessingContract&&) noexcept;
    OwnedProcessingContract& operator=(OwnedProcessingContract&&) noexcept;
    ~OwnedProcessingContract();
    explicit operator bool() const noexcept { return storage_ != nullptr; }
    signal_contracts::DataContract view() const noexcept;
    std::size_t owned_bytes() const noexcept;
private:
    struct Storage;
    Storage* storage_ = nullptr;
    void release() noexcept;
    friend signal_contracts::Status own_processing_contract(const signal_contracts::DataContract&,
        const signal_contracts::FrameLimits&, std::size_t, OwnedProcessingContract&) noexcept;
};
// Preflight candidate + retained out allocation ledger, then nothrow allocation.
// Refusal leaves out intact. Other caller-retained snapshots are charged by their caller.
signal_contracts::Status own_processing_contract(const signal_contracts::DataContract&,
    const signal_contracts::FrameLimits&, std::size_t max_owned_bytes, OwnedProcessingContract&) noexcept;
}
