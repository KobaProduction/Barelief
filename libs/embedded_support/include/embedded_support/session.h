#pragma once
#include <bdsp_protocol/byte_stream.h>
#include <bdsp_protocol/stream_fragments.h>
#include <array>
namespace embedded_support {
using signal_contracts::Status;
enum class Role { publisher, subscriber };
enum class SessionState { stopped, handshake, ready, rejecting, failed };
struct SessionBuffers {
    std::span<std::uint8_t> packet_in, packet_out, read, transmit_frame, fragment, assembly;
    std::span<float> samples;
    std::span<char> decoded_id;
};
using FrameHandler = void (*)(void *, const signal_contracts::FrameView &);
// All caller buffers must be distinct and outlive Session. Configured UTF-8 ID
// is borrowed for its entire lifetime. Serialized task access only, never ISR.
// Frame callback spans are borrowed for that callback only; queue via encode/pool.
// One encoded frame + one packet pending. Transport methods must never block.
// Subscriber replay fence retains the last 16 retired publisher/producer tuples
// in fixed storage. Older evicted identities are outside that bounded window.
class Session {
public:
    Session(Role, signal_contracts::FrameType, std::string_view id, SessionBuffers) noexcept;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Status begin(std::uint64_t now_ms, std::uint64_t publisher_epoch) noexcept;
    void stop() noexcept;
    Status poll(bdsp_protocol::ByteTransport &, std::uint64_t now_ms) noexcept;
    Status publish(const signal_contracts::FrameView &) noexcept;
    void set_frame_handler(FrameHandler handler, void *user) noexcept {handler_=handler;user_=user;}
    SessionState state() const noexcept {return state_;}
    Status last_status() const noexcept {return last_;}
    std::uint64_t dropped() const noexcept {return dropped_;}
    std::size_t pending_bytes() const noexcept {return packets_.pending_bytes()+tx_size_-tx_offset_;}
private:
    void packet(bdsp_protocol::PacketView) noexcept;
    Status fail(Status) noexcept;
    Status pump() noexcept;
    Role role_; signal_contracts::FrameType type_; std::string_view id_; SessionBuffers buffers_;
    bdsp_protocol::PacketStream packets_;
    bdsp_protocol::FrameAssembler assembler_;
    std::array<std::uint8_t,259> request_{};
    std::size_t request_size_=0,tx_size_=0,tx_offset_=0;
    SessionState state_=SessionState::stopped;
    Status last_=Status::ok;
    std::uint64_t started_=0,epoch_=0,dropped_=0,rx_epoch_=0,rx_generation_=0,rx_sequence_=0;
    struct ProducerIdentity {
        std::uint64_t publisher=0,producer=0;
        bool operator==(const ProducerIdentity &) const = default;
    };
    std::uint64_t rx_producer_=0;
    std::array<ProducerIdentity,16> retired_{};
    std::size_t retired_count_=0,retired_next_=0;
    bool rx_identity_known_=false;
    bool gap_=true;
    FrameHandler handler_=nullptr;void *user_=nullptr;
};
}
