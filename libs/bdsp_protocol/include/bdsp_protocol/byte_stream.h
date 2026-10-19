#pragma once
#include "stream_codec.h"
#include <BDSP/streams/reader.h>
#include <BDSP/types.h>
#include <BDSP/strategies/cobs/read.h>
namespace bdsp_protocol {
enum class TransportState { disconnected, connecting, connected, failed };
struct IoResult { signal_contracts::Status status; std::size_t transferred; };
// Implementations never block; transferred is valid even when status reports a terminal condition.
class ByteTransport {
public:
    virtual ~ByteTransport() = default;
    virtual TransportState state() const noexcept = 0;
    virtual IoResult read(std::span<std::uint8_t>) noexcept = 0;
    virtual IoResult write(std::span<const std::uint8_t>) noexcept = 0;
};
struct PacketView { std::uint8_t id; std::span<const std::uint8_t> payload; };
using PacketHandler = void (*)(PacketView, void *);
CodecResult encode_packet(std::uint8_t id, std::span<const std::uint8_t> payload,
                          std::span<std::uint8_t> destination) noexcept;
// Bounded single-packet outgoing queue. enqueue snapshots payload; overflow never alters pending bytes.
// Incoming callback payload is borrowed until the next packet is received. Consume/copy inside callback.
// Caller buffers must be distinct and outlive the stream. All methods require external serialization.
// A callback may reset to cancel the rest of the current receive block. Recursive feed is rejected.
class PacketStream {
public:
    PacketStream(std::span<std::uint8_t> incoming, std::span<std::uint8_t> outgoing) noexcept;
    PacketStream(const PacketStream &) = delete;
    PacketStream &operator=(const PacketStream &) = delete;
    void set_packet_handler(PacketHandler handler, void *context) noexcept { handler_=handler;context_=context; }
    PacketHandler packet_handler() const noexcept {return handler_;}
    void set_error_handler(BDSP::receiver_error_handler_t handler, void *context) noexcept {error_handler_=handler;error_context_=context;}
    signal_contracts::Status feed(std::span<const std::uint8_t>) noexcept;
    signal_contracts::Status enqueue(std::uint8_t, std::span<const std::uint8_t>) noexcept;
    IoResult flush(ByteTransport &) noexcept;
    void reset() noexcept;
    std::size_t pending_bytes() const noexcept {return outgoing_size_-outgoing_offset_;}
    bool discard_unstarted_outgoing() noexcept {
        if(outgoing_offset_ || feeding_)return false;
        outgoing_size_=0;return true;
    }
    std::uint64_t overflow_count() const noexcept {return overflows_;}
private:
    bool read_byte(std::uint8_t) noexcept;
    void on_byte(std::uint8_t, BDSP::streams::stream_read_status_t) noexcept;
    void on_payload_byte(std::uint8_t) noexcept;
    void on_delimiter() noexcept;
    void dispatch_packet(std::size_t) noexcept;
    BDSP::strategies::COBSZPEReadStrategy strategy_;
    BDSP::streams::core::StreamReaderCore reader_;
    std::span<std::uint8_t> incoming_,outgoing_;
    std::size_t incoming_size_=0,outgoing_size_=0,outgoing_offset_=0;
    bool discarding_=false;
    bool feeding_=false;
    std::uint64_t generation_=0,feed_generation_=0;
    signal_contracts::Status feed_status_=signal_contracts::Status::ok;
    std::uint64_t overflows_=0;
    PacketHandler handler_=nullptr; void *context_=nullptr;
    BDSP::receiver_error_handler_t error_handler_=nullptr; void *error_context_=nullptr;
    void report_error(BDSP::parse_packet_status_t error) noexcept;
};
}
