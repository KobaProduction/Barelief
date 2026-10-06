#include <embedded_support/profile.h>
#include <bdsp_protocol/stream_codec.h>
#include <limits>
namespace embedded_support {
using signal_contracts::Status;
Admission admit(const Profile &p, std::size_t id, application::MemoryRequirements runtime) noexcept {
    Admission a;
    if(!id || id>256 || p.sample_rate<1000 || p.sample_rate>192000 || !p.block_samples ||
       p.block_samples>4096 || p.fft_size<512 || p.fft_size>4096 || (p.fft_size&(p.fft_size-1)) ||
       !p.mel_bands || p.mel_bands>p.fft_size/2 || !p.queue_frames || p.queue_frames>8 ||
       !p.utilization_percent || p.utilization_percent>100) return a;
    a.frame_bytes=bdsp_protocol::StreamHeaderBytes+id+4*(p.spectral?p.mel_bands:2*p.block_samples);
    if(a.frame_bytes>p.max_frame_bytes || p.max_frame_bytes>bdsp_protocol::MaxFrameBytes) {
        a.status=Status::buffer_too_small;return a;
    }
    // Conservative bound for upstream COBS-ZPE: at most two encoded bytes per
    // source byte plus header/checksum/delimiters, for every existing fragment.
    const auto fragments=(a.frame_bytes+59999)/60000;
    a.wire_bytes=2*(a.frame_bytes+8*fragments)+16*fragments;
    const std::uint64_t denominator=p.spectral?1:p.block_samples;
    const std::uint64_t frequency=p.spectral?60:p.sample_rate;
    a.required_bytes_per_second=(a.wire_bytes*frequency+denominator-1)/denominator;
    const std::uint64_t capacity=p.link==Link::uart_8n1?p.uart_baud/10:p.tcp_bytes_per_second;
    a.allowed_bytes_per_second=capacity*p.utilization_percent/100;
    if(a.required_bytes_per_second>a.allowed_bytes_per_second){a.status=Status::exhausted;return a;}
    const auto limit=std::numeric_limits<std::size_t>::max();
    a.reserved_bytes=p.adapter_bytes;
    // Include fixed pool capacity even when frames are smaller than their slots.
    for(auto bytes : {p.max_frame_bytes*p.queue_frames,runtime.state_bytes,runtime.buffer_bytes,runtime.scratch_bytes}) {
        if(bytes>limit-a.reserved_bytes){a.status=Status::exhausted;return a;}
        a.reserved_bytes+=bytes;
    }
    a.status=a.reserved_bytes<=p.memory_bytes?Status::ok:Status::exhausted;
    return a;
}
application::RuntimeLimits runtime_limits(const Profile &p) noexcept {
    application::RuntimeLimits limits;
    limits.graph={16,16};limits.max_frame_samples=p.block_samples>p.fft_size?p.block_samples:p.fft_size;
    limits.max_history_frames=0;
    const auto limit=std::numeric_limits<std::size_t>::max();
    const auto reserve=(p.max_frame_bytes>limit/(p.queue_frames?p.queue_frames:1) ||
                        p.max_frame_bytes*p.queue_frames>limit-p.adapter_bytes)
        ?limit:p.adapter_bytes+p.max_frame_bytes*p.queue_frames;
    limits.max_memory_bytes=reserve<p.memory_bytes?p.memory_bytes-reserve:0;
    return limits;
}
}
