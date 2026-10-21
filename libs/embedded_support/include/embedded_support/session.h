#pragma once
#include <bdsp_protocol/byte_stream.h>
#include <bdsp_protocol/stream_fragments.h>
#include <bdsp_protocol/numeric_data_codec.h>
#include <dataflow_runtime/runtime.h>
#include <array>
namespace embedded_support {
using signal_contracts::Status;
enum class Role { publisher, subscriber };
enum class SessionState { stopped, ready, failed };
struct NumericSessionBuffers {
    std::span<std::uint8_t> packet_in, packet_out, read, transmit_frame, encode_scratch, fragment, assembly;
    bdsp_protocol::numeric_data::Arena decode;
};
struct NumericSessionBinding {
    bdsp_protocol::numeric_data::Binding binding;
    bdsp_protocol::numeric_data::Negotiated negotiated;
    std::uint64_t nonce=0, session=0, revision=0, publisher_epoch=0;
};
using NumericFrameHandler=void (*)(void*,const bdsp_protocol::numeric_data::Message&);
// Positive negotiation and endpoint binding come from the trusted control plane.
// This DATA channel opens only after that exchange; no speculative downgrade.
// Caller metadata/storage are stable, distinct and outlive this object. Handler
// sees decoded borrowed views only during callback. No heap/ticks/OS ownership.
class NumericSession {
public:
    // Optional shared PacketStream composition: sender copies/enqueues the full
    // fragment on ok, accepts nothing on would_block. It never owns OS reads.
    using PacketSender=Status (*)(void*,std::uint8_t,std::span<const std::uint8_t>);
    NumericSession(Role,const NumericSessionBinding&,NumericSessionBuffers,
        bdsp_protocol::numeric_data::Limits,PacketSender=nullptr,void* sender_user=nullptr,
        std::size_t packet_payload_capacity=65535) noexcept;
    Status begin() noexcept;
    void stop() noexcept;
    Status poll(bdsp_protocol::ByteTransport&) noexcept;
    Status publish(const bdsp_protocol::numeric_data::Message&) noexcept;
    Status accept_packet(bdsp_protocol::PacketView) noexcept;
    Status drain_packets() noexcept;
    void set_frame_handler(NumericFrameHandler h,void* p) noexcept {handler_=h;user_=p;}
    SessionState state() const noexcept {return state_;}
    Status last_status() const noexcept {return last_;}
    std::size_t pending_bytes() const noexcept {return packets_.pending_bytes()+tx_size_-tx_offset_;}
    std::uint64_t dropped() const noexcept {return dropped_;}
private:
    void accept_fragment(std::span<const std::uint8_t>) noexcept;
    void packet(bdsp_protocol::PacketView) noexcept;
    void decode_complete(std::span<const std::uint8_t>) noexcept;
    Status encode_outgoing(const bdsp_protocol::numeric_data::Message&) noexcept;
    Status validate_binding(const signal_contracts::NumericContract*&) const noexcept;
    Status validate_buffers() const noexcept;
    Status validate_marker(const signal_contracts::NumericContract&) const noexcept;
    void stamp_outgoing(bdsp_protocol::numeric_data::Message&) const noexcept;
    bool accepts_envelope(const bdsp_protocol::numeric_data::Message&) const noexcept;
    void accept_message(bdsp_protocol::numeric_data::Message&) noexcept;
    Status read_packets(bdsp_protocol::ByteTransport&) noexcept;
    Status pump() noexcept;
    Status fail(Status) noexcept;
    bool matches(const bdsp_protocol::numeric_data::Message&) const noexcept;
    Role role_;
    PacketSender sender_=nullptr;
    void* sender_user_=nullptr;
    std::size_t packet_payload_capacity_=65535;
    const NumericSessionBinding& binding_;
    NumericSessionBuffers buffers_;
    bdsp_protocol::numeric_data::Limits limits_;
    bdsp_protocol::PacketStream packets_;
    bdsp_protocol::FrameAssembler assembler_;
    SessionState state_=SessionState::stopped;
    Status last_=Status::ok;
    std::size_t tx_size_=0,tx_offset_=0;
    std::uint64_t sequence_=0,received_=0,dropped_=0;
    bool gap_=true;
    NumericFrameHandler handler_=nullptr;void* user_=nullptr;
};
using DeliveryFence=void (*)(void*) noexcept;
// Composition injects the one executor and a non-failing queue/binding retirement
// callback. Only accepted Apply/Stop retire old DATA. The callback runs before
// the application publishes its new revision: it must retire, never inspect or
// rebind the new graph. Host owns partially transmitted socket/UART cleanup.
class DeliveryFencedRuntime final : public application::GraphRuntimePort {
public:
    DeliveryFencedRuntime(dataflow_runtime::GraphExecutor& executor,DeliveryFence fence,void* user) noexcept
        :executor_(executor),fence_(fence),user_(user) {}
    Status apply(application::PreparedExecution&&) override;
    Status shutdown() override;
private:
    dataflow_runtime::GraphExecutor& executor_;
    DeliveryFence fence_;
    void* user_;
};
}
