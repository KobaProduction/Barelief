#pragma once
#include <cstddef>
#include <cstdint>

namespace bdsp_protocol {
// Hard stack bound applies even if a caller supplies a larger configurable bound.
inline constexpr std::size_t control_codec_max_depth = 32;
// Transport bounds, distinct from application admission/runtime budgets.
struct ControlCodecLimits {
    std::size_t max_wire_bytes = 512 * 1024;
    std::size_t max_nodes = 128, max_edges = 256;
    std::size_t max_graph_identity_bytes = 256, max_field_string_bytes = 256;
    std::size_t max_total_string_bytes = 128 * 1024;
    std::size_t max_capability_kinds = 26, max_capability_types = 9;
    std::size_t max_depth = 16;
};
enum class ControlCodecStatus : std::uint8_t {
    ok, malformed, unsupported_version, unsupported_field, limit_exceeded,
    buffer_too_small, invalid_argument, overflow, codec_failure
};
struct ControlStorageRequirements {
    std::size_t nodes = 0, edges = 0, text_bytes = 0;
    std::size_t capability_kinds = 0, receive_types = 0, publish_types = 0;
};
struct ControlCodecResult {
    ControlCodecStatus status = ControlCodecStatus::ok;
    std::size_t consumed = 0, produced = 0;
    std::size_t required_wire_bytes = 0, required_scratch_bytes = 0;
    ControlStorageRequirements required_arena;
    explicit operator bool() const noexcept { return status == ControlCodecStatus::ok; }
};
} // namespace bdsp_protocol
