#include <bdsp_protocol/byte_stream.h>
#include <BDSP/transceivers.h>
#include <BDSP/memory_manager.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

using namespace bdsp_protocol;
using signal_contracts::Status;
using Bytes = std::vector<std::uint8_t>;

struct CountingMemory : BDSP::core::IMemoryManager {
    size_t allocations = 0;
    void *allocate_memory(size_t bytes) override { ++allocations; return std::malloc(bytes); }
    void free_memory(void *pointer) override { std::free(pointer); }
};
struct PartialTransport : ByteTransport {
    Bytes sent;
    std::size_t limit = 3;
    TransportState state() const noexcept override { return TransportState::connected; }
    IoResult read(std::span<std::uint8_t>) noexcept override { return {Status::would_block, 0}; }
    IoResult write(std::span<const std::uint8_t> data) noexcept override {
        const auto count = std::min(limit, data.size());
        sent.insert(sent.end(), data.begin(), data.begin() + count);
        return {count ? Status::ok : Status::would_block, count};
    }
};

Bytes reference_packet(BDSP::COBSZPETransceiver& reference, std::uint8_t id,
                       Bytes& payload) {
    Bytes expected;
    reference.get_write_stream().set_stream_writer(
        [](uint8_t byte, void *context) { static_cast<Bytes *>(context)->push_back(byte); },
        &expected);
    assert(reference.send_data(id, payload.data(), payload.size()) == BDSP::SEND_PACKET_OK);
    return expected;
}
void receive_packet(PacketView packet, void *context) {
    auto *received = static_cast<std::pair<int *, Bytes *> *>(context);
    ++*received->first;
    assert(packet.id == 13);
    received->second->assign(packet.payload.begin(), packet.payload.end());
}
void record_error(BDSP::parse_packet_status_t error, void *context) {
    static_cast<std::vector<BDSP::parse_packet_status_t> *>(context)->push_back(error);
}
void partial_writes_and_retirement(PacketStream& stream, Bytes& payload,
                                   const Bytes& expected) {
    assert(stream.enqueue(13, payload) == Status::ok);
    assert(stream.enqueue(13, payload) == Status::overflow);
    payload.assign(payload.size(), 99); // Enqueued bytes own the payload snapshot.
    PartialTransport transport;
    transport.limit = 0;
    assert(stream.flush(transport).status == Status::would_block);
    assert(stream.pending_bytes() == expected.size());
    transport.limit = 3;
    while (stream.pending_bytes()) assert(stream.flush(transport).status == Status::ok);
    assert(transport.sent == expected);
    assert(stream.enqueue(13, payload) == Status::ok);
    assert(stream.discard_unstarted_outgoing() && stream.pending_bytes() == 0);
    assert(stream.enqueue(13, payload) == Status::ok);
    transport.limit = 1;
    assert(stream.flush(transport).transferred == 1);
    const auto pending = stream.pending_bytes();
    assert(!stream.discard_unstarted_outgoing() && stream.pending_bytes() == pending);
}
void framing_errors_and_recovery(PacketStream& stream, const Bytes& expected,
                                 int& received) {
    std::vector<BDSP::parse_packet_status_t> errors;
    stream.set_error_handler(record_error, &errors);
    auto corrupted = expected;
    corrupted[20] ^= 1;
    assert(stream.feed(corrupted) == Status::malformed);
    assert(errors.size() == 1 && errors.back() == BDSP::CHECKSUM_ERROR);
    assert(stream.feed(expected) == Status::ok && received == 2);
    assert(errors.size() == 1);
    std::array<uint8_t, 12> tiny{};
    std::array<uint8_t, 2200> outgoing{};
    PacketStream bounded(tiny, outgoing);
    bounded.set_error_handler(record_error, &errors);
    assert(bounded.feed(expected) == Status::overflow);
    assert(errors.size() == 2);
    assert(errors.back() == BDSP::EXCEEDING_THE_MAXIMUM_PACKET_SIZE_ERROR);
    bounded.reset();
    assert(bounded.feed(std::span(expected).first(3)) == Status::ok);
    bounded.reset(); // Reconnection fences framing and partial bytes.
}
void reset_during_receive(const Bytes& expected) {
    std::array<uint8_t, 2200> input{}, output{};
    PacketStream cancelled(input, output);
    std::pair<PacketStream *, int> state{&cancelled, 0};
    cancelled.set_packet_handler([](PacketView, void *context) {
        auto& current = *static_cast<std::pair<PacketStream *, int> *>(context);
        ++current.second;
        current.first->reset();
    }, &state);
    auto coalesced = expected;
    coalesced.insert(coalesced.end(), expected.begin(), expected.end());
    assert(cancelled.feed(coalesced) == Status::ok);
    assert(state.second == 1);
    assert(cancelled.feed(expected) == Status::ok && state.second == 2);
}
void boundary_pattern(BDSP::COBSZPETransceiver& reference, size_t size, int pattern) {
    Bytes input(size);
    for (size_t index = 0; index < size; ++index)
        input[index] = pattern == 0 ? 0 : pattern == 1 ? 255 : index % 256;
    const auto expected = reference_packet(reference, 31, input);
    Bytes encoded(expected.size(), 99);
    CountingMemory memory;
    BDSP::core::MemoryManager::set_new_manager(&memory);
    const auto result = encode_packet(31, input, encoded);
    BDSP::core::MemoryManager::set_new_manager(nullptr);
    assert(result && memory.allocations == 0 && encoded == expected);
    Bytes raw(65539), outgoing(expected.size());
    PacketStream receiver(raw, outgoing);
    size_t count = 0;
    receiver.set_packet_handler([](PacketView packet, void *context) {
        assert(packet.id == 31);
        ++*static_cast<size_t *>(context);
    }, &count);
    const size_t split = expected.size() / 2;
    assert(receiver.feed(std::span(expected).first(split)) == Status::ok);
    assert(receiver.feed(std::span(expected).subspan(split)) == Status::ok && count == 1);
}
void boundary_patterns(BDSP::COBSZPETransceiver& reference) {
    for (size_t size : {size_t(1), size_t(255), size_t(256), size_t(65535)})
        for (int pattern = 0; pattern < 3; ++pattern) boundary_pattern(reference, size, pattern);
}
void packet_roundtrip(BDSP::COBSZPETransceiver& reference) {
    Bytes payload(1024);
    for (size_t index = 0; index < payload.size(); ++index) payload[index] = index % 256;
    const auto expected = reference_packet(reference, 13, payload);
    std::array<uint8_t, 2200> input{}, output{};
    PacketStream stream(input, output);
    int received = 0;
    Bytes received_payload;
    std::pair context{&received, &received_payload};
    stream.set_packet_handler(receive_packet, &context);
    partial_writes_and_retirement(stream, payload, expected);
    stream.reset();
    assert(stream.pending_bytes() == 0);
    for (auto byte : expected) assert(stream.feed(std::span(&byte, 1)) == Status::ok);
    assert(received == 1 && received_payload[255] == 255 && received_payload[256] == 0);
    framing_errors_and_recovery(stream, expected, received);
    reset_during_receive(expected);
    std::array<uint8_t, 1> short_out{88};
    const auto encoded = encode_packet(13, payload, short_out);
    assert(encoded.status == Status::buffer_too_small && encoded.required_bytes > short_out.size() && short_out[0] == 88);
}
int main() {
    BDSP::COBSZPETransceiver reference;
    packet_roundtrip(reference);
    boundary_patterns(reference);
    std::cout << "BDSP upstream framing, arbitrary fragments, partial writes and overflow passed\n";
}
