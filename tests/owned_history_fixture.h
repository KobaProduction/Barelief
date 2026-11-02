#pragma once
#include <signal_contracts/data_frame.h>
#include <array>
namespace history_fixture {
// External ring ingress pins an owner and reports its accepted-row ordinal.
struct Arena {
    alignas(64) std::array<std::byte,65536> payload{},metadata{};
    std::array<signal_contracts::BufferSlot,8> payload_slots{},metadata_slots{};
    signal_contracts::BufferPool values{payload,payload_slots,8192},descriptors{metadata,metadata_slots,8192};
};
}
