#pragma once
#include "stream_codec.h"
namespace bdsp_protocol {
inline constexpr std::size_t StreamFragmentBytes = 60000;
// Existing BDSP ID 13 payload: little-endian uint32 total, uint32 offset, contiguous bytes.
CodecResult encode_fragment(std::span<const std::uint8_t> frame,std::size_t offset,
                            std::span<std::uint8_t> destination,
                            std::size_t max_chunk_bytes=StreamFragmentBytes) noexcept;
class FrameAssembler {
public:
    explicit FrameAssembler(std::span<std::uint8_t> storage) noexcept :storage_(storage) {}
    // Complete frame is borrowed until reset/next accept. Failure clears both assembly and output.
    // Offset zero starts/restarts assembly, matching the existing desktop receiver.
    signal_contracts::Status accept(std::span<const std::uint8_t> fragment,
                                   std::span<const std::uint8_t> &complete) noexcept;
    void reset() noexcept {total_=received_=0;}
    std::size_t pending_bytes() const noexcept {return received_;}
private:
    std::span<std::uint8_t> storage_;
    std::size_t total_=0,received_=0;
};
}
