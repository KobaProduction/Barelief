#include <bdsp_protocol/control_codec.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

using namespace bdsp_protocol;
static bool forbid_heap = false;
void* operator new(std::size_t size) {
    if (forbid_heap) std::abort();
    if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
    std::abort();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main() {
    ProtobufControlCodec codec;
    WireControlMessage message;
    message.identity = {11,12,13};
    WireCommand command;
    command.id = WireCommandId::state;
    command.request_id = 14;
    command.expected_revision = 15;
    message.body = command;
    constexpr std::uint8_t golden[]{0x08,0x01,0x12,0x06,0x08,0x0b,0x10,0x0c,0x18,0x0d,0x1a,0x06,0x08,0x02,0x10,0x0e,0x18,0x0f};
    std::array<std::uint8_t,2048> output{}, scratch{};
    output.fill(0xa5);
    forbid_heap = true;
    auto encoded = codec.encode(message,output,scratch);
    assert(encoded && encoded.produced == sizeof(golden));
    assert(std::memcmp(output.data(),golden,sizeof(golden)) == 0);
    assert(output[sizeof(golden)] == 0xa5);
    output.fill(0xa5);
    encoded = codec.encode(message,std::span(output).first(sizeof(golden)-1),scratch);
    assert(encoded.status == ControlCodecStatus::buffer_too_small);
    assert(encoded.produced == 0 && encoded.consumed == 0);
    assert(encoded.required_wire_bytes == sizeof(golden));
    assert(encoded.required_scratch_bytes == sizeof(golden));
    for (auto byte : output) assert(byte == 0xa5);
    encoded = codec.encode(message,output,std::span(scratch).first(sizeof(golden)-1));
    assert(encoded.status == ControlCodecStatus::buffer_too_small);
    for (auto byte : output) assert(byte == 0xa5);

    WireControlMessage decoded;
    decoded.identity = {99,98,97};
    auto result = codec.decode(golden,{},decoded);
    assert(result && result.consumed == sizeof(golden));
    assert(decoded.identity.client == 11 && decoded.identity.session == 12 && decoded.identity.epoch == 13);
    auto decoded_command = std::get<WireCommand>(decoded.body);
    assert(decoded_command.id == WireCommandId::state && decoded_command.request_id == 14);
    assert(decoded_command.expected_revision == 15);
    // Every proper prefix is rejected; no partially published destination.
    for (std::size_t size=0; size<sizeof(golden); ++size) {
        decoded.identity = {99,98,97};
        result = codec.decode(std::span(golden).first(size),{},decoded);
        assert(!result && result.consumed == 0 && result.produced == 0);
        assert(decoded.identity.client == 99 && decoded.identity.epoch == 97);
    }

    // Duplicate scalar fields are rejected instead of nanopb last-value-wins.
    std::array<std::uint8_t,sizeof(golden)+3> bad{};
    std::memcpy(bad.data(),golden,sizeof(golden));
    decoded.identity = {99,98,97};
    bad[sizeof(golden)] = 0x08; bad[sizeof(golden)+1] = 0x01;
    result = codec.decode(std::span(bad).first(sizeof(golden)+2),{},decoded);
    assert(result.status == ControlCodecStatus::malformed && decoded.identity.client == 99);
    bad[sizeof(golden)] = 0xf8; bad[sizeof(golden)+1] = 0x01; bad[sizeof(golden)+2] = 0x00;
    result = codec.decode(bad,{},decoded);
    assert(result.status == ControlCodecStatus::unsupported_field && decoded.identity.client == 99);
    bad[sizeof(golden)] = 0; // illegal protobuf field zero
    result = codec.decode(std::span(bad).first(sizeof(golden)+1),{},decoded);
    assert(result.status == ControlCodecStatus::malformed && decoded.identity.client == 99);
    ControlCodecLimits limits;
    limits.max_wire_bytes = sizeof(golden)-1;
    result = codec.decode(golden,{},decoded,limits);
    assert(result.status == ControlCodecStatus::limit_exceeded);

    // Graph arena admission is atomic; successful text is owned by arena.
    WireNode node{1,"TestSignal",WireTestSignal{},true};
    WireEdge edge{1,2,0,0};
    command.id = WireCommandId::prepare;
    command.processing = WireProcessingGraph{"graph-A",1,{&node,1},{&edge,1}};
    message.body = command;
    encoded = codec.encode(message,output,scratch);
    assert(encoded);
    const auto graph_size = encoded.produced;
    std::array<WireNode,1> nodes{};
    std::array<WireEdge,1> edges{};
    std::array<char,128> text{};
    text.fill('X');
    nodes[0].id = 777;
    edges[0].from = 888;
    decoded.identity = {99,98,97};
    result = codec.decode(std::span(output).first(graph_size),
        {nodes,edges,std::span(text).first(16),{},{},{}},decoded);
    assert(result.status == ControlCodecStatus::buffer_too_small);
    assert(result.required_arena.nodes == 1 && result.required_arena.edges == 1);
    assert(result.required_arena.text_bytes == 17);
    assert(decoded.identity.client == 99 && nodes[0].id == 777 && edges[0].from == 888);
    for (auto character : text) assert(character == 'X');
    result = codec.decode(std::span(output).first(graph_size),{nodes,edges,text,{},{},{}},decoded);
    assert(result);
    output.fill(0); // incoming bytes may be reused after decode
    const auto graph = *std::get<WireCommand>(decoded.body).processing;
    assert(graph.graph_identity == "graph-A" && graph.nodes[0].kind == "TestSignal");
    assert(graph.nodes[0].id == 1 && graph.edges[0].to == 2);
    assert(std::holds_alternative<WireTestSignal>(graph.nodes[0].parameters));
    WireReply reply;
    reply.request_id = UINT64_MAX;
    reply.state.epoch = UINT64_MAX;
    reply.state.revision = UINT64_MAX;
    reply.prepared_token = UINT64_MAX;
    reply.memory = {UINT64_MAX,UINT64_MAX,UINT64_MAX};
    reply.error_location.port = 0;
    reply.validation_location.edge_index = 0;
    message.body = reply;
    encoded = codec.encode(message,output,scratch);
    assert(encoded);
    result = codec.decode(std::span(output).first(encoded.produced),{},decoded);
    assert(result);
    const auto roundtrip_reply = std::get<WireReply>(decoded.body);
    assert(roundtrip_reply.request_id == UINT64_MAX && roundtrip_reply.state.revision == UINT64_MAX);
    assert(roundtrip_reply.prepared_token == UINT64_MAX && roundtrip_reply.memory.scratch_bytes == UINT64_MAX);
    assert(roundtrip_reply.error_location.port && *roundtrip_reply.error_location.port == 0);
    assert(roundtrip_reply.validation_location.edge_index && *roundtrip_reply.validation_location.edge_index == 0);
    assert(!roundtrip_reply.error_location.edge_index && !roundtrip_reply.validation_location.port);
    WireCommand set;
    set.id = WireCommandId::set_parameters;
    set.node = 7;
    set.parameters = WireGain{-0.0};
    message.body = set;
    encoded = codec.encode(message,output,scratch);
    assert(encoded);
    result = codec.decode(std::span(output).first(encoded.produced),{},decoded);
    assert(result);
    const auto gain = std::get<WireGain>(*std::get<WireCommand>(decoded.body).parameters).gain;
    assert(gain == 0 && std::signbit(gain));
    forbid_heap = false;
    std::puts("protobuf control golden/atomic/owned arena tests passed");
}

