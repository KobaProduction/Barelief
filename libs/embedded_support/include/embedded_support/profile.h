#pragma once
#include <application/graph_lifecycle.h>
namespace embedded_support {
enum class Link { tcp, uart_8n1 };
// Admission limits, not measurements of the chip's real heap/CPU/network capacity.
struct Profile {
    Link link = Link::tcp;
    std::size_t memory_bytes = 256 * 1024, adapter_bytes = 32 * 1024;
    std::size_t max_frame_bytes = 2304, queue_frames = 2;
    std::uint32_t sample_rate = 8000, block_samples = 256, fft_size = 2048, mel_bands = 8;
    std::uint32_t uart_baud = 2000000, tcp_bytes_per_second = 200000;
    std::uint32_t utilization_percent = 80;
    bool spectral = false; // raw PCM first; history is always OFF in this profile.
};
struct Admission {
    signal_contracts::Status status = signal_contracts::Status::invalid_argument;
    std::size_t frame_bytes = 0, wire_bytes = 0, reserved_bytes = 0;
    std::uint64_t required_bytes_per_second = 0, allowed_bytes_per_second = 0;
};
Admission admit(const Profile &, std::size_t stream_id_bytes,
                application::MemoryRequirements runtime = {}) noexcept;
application::RuntimeLimits runtime_limits(const Profile &) noexcept;
}
