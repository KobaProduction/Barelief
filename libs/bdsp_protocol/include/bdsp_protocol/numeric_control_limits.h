#pragma once
#include <signal_contracts/data_contract.h>
namespace bdsp_protocol::numeric_control {
inline constexpr std::size_t HardDepth = 32;
struct Limits {
    std::size_t wire_bytes = 0, arena_bytes = 0, string_bytes = 0, total_text_bytes = 0, records = 0, nodes = 0,
                edges = 0, ports = 0, parameters = 0, channels = 0, capability_rows = 0, extensions = 0,
                extension_bytes = 0, depth = 0;
    signal_contracts::FrameLimits frame{};
};
// Explicit adapter envelopes; maxima are not promised simultaneously. Arena +
// equally sized staging are supplied off the processing callback/task stack.
// Device transport envelope is not an ESP stack/kernel acceptance claim.
constexpr Limits desktop_limits() noexcept {
    return {524288, 524288, 256,  131072, 8192,
            128,    256,    2048, 2048,   16,
            256,    64,     4096, 32,     {16, 800000, 800000, 65536, 256, 32768}};
}
constexpr Limits device_limits() noexcept {
    // Nine nodes admit the explicit Window/N/coefficient/product SDK graph.
    return {16384, 16384, 64, 2048, 128, 9, 16, 32, 32, 2, 4, 8, 128, 32, {2, 4096, 32768, 2048, 64, 256}};
}
enum class Status {
    Ok,
    Malformed,
    UnsupportedVersion,
    UnsupportedField,
    Limit,
    BufferTooSmall,
    InvalidArgument,
    Overflow,
    CodecFailure
};
struct Result {
    Status status = Status::Ok;
    std::size_t consumed = 0, produced = 0, required_wire = 0, required_scratch = 0, required_arena = 0,
                required_text = 0;
    explicit operator bool() const noexcept {
        return status == Status::Ok;
    }
};
} // namespace bdsp_protocol::numeric_control
