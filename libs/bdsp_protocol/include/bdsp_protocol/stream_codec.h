#pragma once
#include <signal_contracts/frame.h>
namespace bdsp_protocol {
inline constexpr std::size_t MaxFrameBytes = 2*1024*1024;
inline constexpr std::size_t StreamHeaderBytes = 112;
enum class CodecError { none, size, version, flags, reserved, dimensions, values };
struct CodecResult {
    signal_contracts::Status status = signal_contracts::Status::ok;
    std::size_t required_bytes = 0, written_bytes = 0, required_samples = 0, required_id_bytes = 0;
    CodecError error = CodecError::none;
    std::size_t required_packet_bytes = 0;
    explicit operator bool() const noexcept {return status==signal_contracts::Status::ok;}
};
bool compatible_geometry(signal_contracts::FrameType,const signal_contracts::FrameMetadata&,std::size_t columns,std::size_t rows) noexcept;
// No allocation or exceptions. Insufficient capacity leaves destination and frame unchanged.
CodecResult encode(const signal_contracts::FrameView &, std::span<std::uint8_t>) noexcept;
CodecResult decode(std::span<const std::uint8_t>, std::span<float> samples,
                   std::span<char> stream_id, signal_contracts::FrameView &) noexcept;
CodecResult decode(std::span<const std::uint8_t>, std::span<float> samples,
                   std::span<char> stream_id, std::span<std::uint8_t> packet,
                   signal_contracts::FrameView &) noexcept;
const char *error_message(CodecError) noexcept;
}
