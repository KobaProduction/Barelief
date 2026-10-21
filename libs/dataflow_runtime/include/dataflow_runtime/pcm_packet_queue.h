#pragma once
#include <signal_contracts/data_frame.h>
#include <vector>

namespace dataflow_runtime {
struct PcmPacketQueueStats {
    std::size_t queued_samples=0,queued_packets=0,capacity_samples=0,capacity_packets=0;
    std::uint64_t accepted_samples=0,overflow_packets=0,dropped_samples=0,discontinuities=0;
};
// Packet dosing is distinct from resampling and window accumulation. Mono PCM
// lives in an owning bounded ring; caller buffers and stamps are never retained.
// Clock ticks/sample_begin are exact input sample indices at prepared Fs. This
// interface does not synchronize independent clocks or provide device drivers.
class PcmPacketQueue {
public:
    bool prepare(std::uint32_t sample_rate,std::size_t sample_capacity,std::size_t packet_capacity);
    signal_contracts::Status push(std::span<const float>,const signal_contracts::FrameStamp&) noexcept;
    signal_contracts::Status pop(std::span<float>,std::size_t& written,signal_contracts::FrameStamp&) noexcept;
    void clear() noexcept;
    PcmPacketQueueStats stats() const noexcept;
    std::size_t workspace_bytes() const noexcept;
    static std::size_t storage_bytes(std::size_t sample_capacity,std::size_t packet_capacity) noexcept;
private:
    struct Packet {signal_contracts::FrameStamp stamp;std::size_t size=0,consumed=0;};
    bool valid_packet(std::span<const float>,const signal_contracts::FrameStamp&) const noexcept;
    signal_contracts::Status retain_packet(std::span<const float>,const signal_contracts::FrameStamp&,bool gap) noexcept;
    void publish_stamp(const Packet&,std::size_t,signal_contracts::FrameStamp&) noexcept;
    std::size_t pop_packet(std::span<float>,Packet&,signal_contracts::FrameStamp&) noexcept;
    void consume_packet(Packet&,std::size_t) noexcept;
    std::vector<float> samples_;std::vector<Packet> packets_;
    std::uint32_t rate_=0;std::size_t sample_head_=0,sample_count_=0,packet_head_=0,packet_count_=0;
    std::uint64_t accepted_=0,overflow_=0,dropped_=0,discontinuities_=0,sequence_=0;
    signal_contracts::FrameStamp last_{};bool marker_=false,pending_gap_=true;
};
}
