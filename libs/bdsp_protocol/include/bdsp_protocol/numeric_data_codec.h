#pragma once
#include <bdsp_protocol/numeric_data_message.h>
namespace bdsp_protocol::numeric_data {
struct Requirements {
    std::size_t wire = 0, bytes = 0, text = 0, channels = 0, planes = 0;
};
struct Result {
    signal_contracts::Status status = signal_contracts::Status::ok;
    Requirements required;
    std::size_t produced = 0, consumed = 0;
    explicit operator bool() const noexcept { return status == signal_contracts::Status::ok; }
};
struct Arena {
    std::span<std::byte> bytes;
    std::span<char> text;
    std::span<std::uint32_t> channels;
    std::span<signal_contracts::PlaneLayout> planes;
};
// Stable disjoint input/output/scratch/arena/source objects required for the call.
// Zero heap. Decode copies ALL borrowed fields; caller arena remains their owner.
// Error leaves destination/scratch-excluded arenas unchanged; produced/consumed=0.
// Capacity requirements exact only after successful complete validation.
// Encode needs scratch >=required.wire. Arena bytes base must be aligned to8;
// bytes/text/channel/layout capacities are separate, not a total pool size.
// ReferenceContext is immutable/stable/disjoint for the call. Decode retains no
// references to its bindings; output lifetime depends solely on caller Arena.
Result measure(const Message &, const Limits &, ReferenceContext = {}) noexcept;
Result encode(const Message &, std::span<std::byte> output, std::span<std::byte> scratch, const Limits &,
              ReferenceContext = {}) noexcept;
Result inspect(std::span<const std::byte>, const Limits &, ReferenceContext = {}) noexcept;
Result decode(std::span<const std::byte>, Message &output, Arena, const Limits &,
              ReferenceContext = {}) noexcept;
} // namespace bdsp_protocol::numeric_data
