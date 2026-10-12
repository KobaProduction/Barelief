#pragma once
#include <signal_contracts/data_frame.h>
namespace bdsp_protocol::numeric_data {
// Explicit adapter envelope, not signal_contracts defaults or kernel capabilities.
struct Limits {
    signal_contracts::FrameLimits frame;
    std::size_t message_bytes, text_bytes, extension_bytes, history_rows, identity_bytes;
};
constexpr Limits desktop_limits() noexcept {
    return {{16, 800000, 800000, 65536, 256, 32768}, 900000, 65536, 4096, 2000, 256};
}
constexpr Limits device_block_limits() noexcept {
    // DATA5 adds 41 bytes of exact signal-interval/latency provenance to DATA4.
    return {{2, 512, 2048, 1024, 64, 256}, 2281, 2048, 128, 0, 32};
}
} // namespace bdsp_protocol::numeric_data
