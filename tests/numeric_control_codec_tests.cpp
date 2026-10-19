#include <bdsp_protocol/numeric_control_codec.h>
#include <bdsp_protocol/version_admission.h>
#include <array>
#include <cstdio>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <new>
#include <algorithm>
#ifdef NUMERIC_CONTROL_MAPPER_TESTS
#include <control/numeric_processing_message_mapper.h>
#include <bdsp_protocol/numeric_data_codec.h>
#endif
using namespace bdsp_protocol::numeric_control;
namespace {
bool tracking = false;
std::size_t allocations = 0, c_allocations = 0;
unsigned failures = 0, checks = 0;
void expect(bool b, int line) {
    ++checks;
    if (!b) {
        ++failures;
        std::printf("FAIL line%d\n", line);
    }
}
#define CHECK(v) expect(bool(v), __LINE__)
Limits limits{65536, 65536, 256,  32768, 2048,
              128,   256,   512,  512,   512,
              128,   32,    4096, 32,    {32, 65536, 1048576, 65536, 256, 65536}};
std::array<std::uint8_t, 65536> wire{}, scratch{};
alignas(std::max_align_t) std::array<std::byte, 65536> arena{}, stage{};
Result encode(const Codec &c, const Message &m, const Limits &l = limits) {
    tracking = true;
    auto r = c.encode(m, wire, scratch, l);
    tracking = false;
    return r;
}
Result decode(const Codec &c, std::size_t n, Message &m, const Limits &l = limits) {
    tracking = true;
    auto r = c.decode({wire.data(), n}, arena, stage, m, l);
    tracking = false;
    return r;
}
bool same_literal(const Literal &x, const Literal &y) {
    if (x.index() != y.index())
        return false;
    if (auto p = std::get_if<float>(&x))
        return std::bit_cast<std::uint32_t>(*p) == std::bit_cast<std::uint32_t>(std::get<float>(y));
    if (auto p = std::get_if<double>(&x))
        return std::bit_cast<std::uint64_t>(*p) == std::bit_cast<std::uint64_t>(std::get<double>(y));
    if (auto p = std::get_if<std::int64_t>(&x))
        return *p == std::get<std::int64_t>(y);
    return std::get<std::uint64_t>(x) == std::get<std::uint64_t>(y);
}
void exact_request(const Codec& c) {
    Message m;
    m.request = UINT64_MAX;
    m.expected_revision = 9007199254740993ULL;
    auto e = encode(c, m);
    CHECK(e);
    Message out;
    CHECK(decode(c, e.produced, out));
    CHECK(out.request == UINT64_MAX);
    CHECK(out.expected_revision == m.expected_revision);
}
void truncated_golden(const Codec& c,std::span<const std::uint8_t> golden) {
    Message out;
    for (std::size_t n = 0; n < golden.size(); ++n) {
        out.request = 77;
        arena.fill(std::byte{0xa5});
        const auto truncated = decode(c, n, out);
        CHECK(!truncated && truncated.consumed == 0 && truncated.produced == 0);
        CHECK(out.request == 77);
        CHECK(std::all_of(arena.begin(), arena.end(), [](auto b) { return b == std::byte{0xa5}; }));
    }
}
void malformed_basic(const Codec& c,std::size_t size) {
    Message out;
    Message plain;
    // Duplicate known field and unknown mandatory/outside extension namespace.
    if (c.id() == CodecId::Cbor) {
        wire[0] = 0xa9;
        wire[size] = 1;
        wire[size + 1] = 2;
    } else {
        wire[size] = 8;
        wire[size + 1] = 2;
    }
    arena.fill(std::byte{0xa5});
    out.request = 77;
    auto result = decode(c, size + 2, out);
    CHECK(!result);
    CHECK(result.consumed == 0);
    CHECK(out.request == 77);
    CHECK(arena.front() == std::byte{0xa5});
    auto encoded = encode(c, plain);
    if (c.id() == CodecId::Cbor) {
        wire[0] = 0xa9;
        wire[encoded.produced] = 10;
        wire[encoded.produced + 1] = 0;
    } else {
        wire[encoded.produced] = 80;
        wire[encoded.produced + 1] = 0;
    }
    CHECK(decode(c, encoded.produced + 2, out).status == Status::UnsupportedField);
    encoded = encode(c, plain);
    wire[1 + (c.id() == CodecId::Cbor ? 1 : 0)] = 1;
    CHECK(decode(c, encoded.produced, out).status == Status::UnsupportedVersion);
    encoded = encode(c, plain);
    CHECK(!decode(c, encoded.produced - 1, out));
}
void basic_limits(const Codec& c,std::size_t golden_size) {
    Message plain;
    auto shallow = limits;
    shallow.depth = 1;
    CHECK(!encode(c, plain, shallow));
    auto zero = limits;
    zero.wire_bytes = 0;
    CHECK(encode(c, plain, zero).status == Status::Limit);
    wire.fill(0xcc);
    tracking = true;
    const auto result = c.encode(plain, {wire.data(), 1}, scratch, limits);
    tracking = false;
    CHECK(result.status == Status::BufferTooSmall);
    CHECK(result.produced == 0);
    CHECK(wire[0] == 0xcc);
    CHECK(result.required_wire == golden_size);
}
void basic(const Codec &c) {
    exact_request(c);
    Message plain;
    auto e = encode(c, plain);
    CHECK(e);
    const std::uint8_t cb[]{0xa8, 1, 4, 2,    0, 3, 0, 4, 0, 5,    0, 6,    0, 7,   0xa6,
                            1,    2, 4, 0x60, 5, 0, 6, 0, 7, 0x20, 8, 0xf5, 9, 0x80};
    const std::uint8_t pb[]{8, 4, 16, 0, 24, 0, 32, 0, 40, 0, 48, 0, 58, 12, 8, 2, 34, 0, 40, 0, 48, 0, 56, 1, 64, 1};
    auto golden = c.id() == CodecId::Cbor ? std::span<const std::uint8_t>{cb} : std::span<const std::uint8_t>{pb};
    CHECK(e.produced == golden.size());
    CHECK(std::memcmp(wire.data(), golden.data(), golden.size()) == 0);
    Message out;
    CHECK(decode(c, golden.size(), out));
    // Retained schema-independent oracle: every proper prefix must refuse
    // without publishing a partial message or changing destination storage.
    truncated_golden(c,golden);
    malformed_basic(c,e.produced);
    basic_limits(c,golden.size());
}
void preflight_regression(const Codec &c) {
    Parameter parameter{"label", std::string_view{"x"}};
    Node node{1, "TestSignal", EndpointParameters{{&parameter, 1}}, {}, {}};
    node.enabled = false;
    domain::Edge edge{1, 2, 0, 0, 3};
    Graph graph{"graph", {}, {&node, 1}, {&edge, 1}};
    CapabilityRow row{node, "k", "b", signal_contracts::ValueDomain::FiniteOnly, {21, 22, 23, 24, 25}};
    std::array<std::uint8_t, 5> extension_bytes{1, 2, 3, 4, 5};
    Extension extension{1024, extension_bytes};
    for (unsigned mask = 0; mask != 4; ++mask) {
        Reply reply;
        reply.error = Error::Validation;
        reply.diagnostic.node = -7;
        reply.diagnostic.port = UINT16_MAX;
        reply.diagnostic.edge = UINT64_MAX;
        reply.diagnostic.parameter = "why";
        reply.state.lifecycle = Lifecycle::Stopped;
        reply.state.revision = UINT64_MAX;
        reply.state.resources = {11, 12, 13, 14, 15};
        if (mask & 1)
            reply.state.applied = graph;
        if (mask & 2)
            reply.capabilities = Capabilities{4, bdsp_protocol::numeric_data::PayloadVersion, 3, {&row, 1}, {31, 32, 33, 34, 35}};
        Message message;
        message.client = UINT64_MAX;
        message.session = 9007199254740993ULL;
        message.epoch = 88;
        message.request = UINT64_MAX;
        message.body = reply;
        message.extensions = {&extension, 1};
        wire.fill(0xcc);
        scratch.fill(0xdd);
        tracking = true;
        const auto measured = c.measure(message, limits);
        tracking = false;
        CHECK(measured);
        CHECK(measured.consumed == 0 && measured.produced == 0);
        CHECK(measured.required_wire > 0 && measured.required_scratch == measured.required_wire);
        CHECK(measured.required_text == 8 + ((mask & 1) ? 21 : 0) + ((mask & 2) ? 18 : 0));
        CHECK(std::all_of(wire.begin(), wire.end(), [](auto b) { return b == 0xcc; }));
        CHECK(std::all_of(scratch.begin(), scratch.end(), [](auto b) { return b == 0xdd; }));
        if (!measured)
            continue;
        tracking = true;
        auto encoded = c.encode(message, std::span(wire).first(measured.required_wire),
                                std::span(scratch).first(measured.required_scratch), limits);
        tracking = false;
        CHECK(encoded && encoded.produced == measured.required_wire);
        if (!encoded)
            continue;
        Message decoded;
        tracking = true;
        auto restored =
            c.decode(std::span(wire).first(encoded.produced), std::span(arena).first(measured.required_arena),
                     std::span(stage).first(measured.required_arena), decoded, limits);
        tracking = false;
        CHECK(restored && restored.required_arena == measured.required_arena);
        CHECK(restored.required_scratch == measured.required_arena);
        CHECK(restored.required_text == measured.required_text);
        CHECK(decoded.request == UINT64_MAX);
        auto check_node = [](const Node &n) {
            CHECK(n.id == 1 && n.kind == "TestSignal" && !n.enabled);
            CHECK(n.inputs.empty() && n.outputs.empty());
            CHECK(std::holds_alternative<EndpointParameters>(n.parameters));
            if (const auto *parameters = std::get_if<EndpointParameters>(&n.parameters)) {
                CHECK(parameters->values.size() == 1);
                if (parameters->values.size() == 1) {
                    CHECK(parameters->values[0].key == "label");
                    const auto *value = std::get_if<std::string_view>(&parameters->values[0].value);
                    CHECK(value && *value == "x");
                }
            }
        };
        auto check_reply = [&](const Message &m, bool has_extension) {
            CHECK(m.schema_version == 4 && m.client == UINT64_MAX && m.session == 9007199254740993ULL);
            CHECK(m.epoch == 88 && m.request == UINT64_MAX && m.expected_revision == 0);
            CHECK(std::holds_alternative<Reply>(m.body));
            const auto *r = std::get_if<Reply>(&m.body);
            if (!r)
                return;
            CHECK(r->command == CommandId::State && r->error == Error::Validation);
            CHECK(r->diagnostic.node == -7 && r->diagnostic.port == UINT16_MAX && r->diagnostic.edge == UINT64_MAX);
            CHECK(r->diagnostic.parameter == "why");
            CHECK(r->state.lifecycle == Lifecycle::Stopped && !r->state.run_intent);
            CHECK(r->state.revision == UINT64_MAX && r->state.token == 0);
            CHECK(r->state.resources.plan == 11 && r->state.resources.payload == 12 &&
                  r->state.resources.scratch == 13 && r->state.resources.stack == 14 && r->state.resources.peak == 15);
            CHECK(r->state.applied.has_value() == bool(mask & 1));
            if (r->state.applied) {
                const auto &g = *r->state.applied;
                CHECK(g.graph_id == "graph" && g.nodes.size() == 1 && g.edges.size() == 1);
                CHECK(g.versions.document == graph.versions.document && g.versions.graph == graph.versions.graph &&
                      g.versions.processing == graph.versions.processing &&
                      g.versions.control == graph.versions.control && g.versions.data == graph.versions.data);
                if (g.nodes.size() == 1)
                    check_node(g.nodes[0]);
                if (g.edges.size() == 1)
                    CHECK(g.edges[0].from == 1 && g.edges[0].to == 2 && g.edges[0].output_port == 0 &&
                          g.edges[0].input_port == 0 && g.edges[0].conversion == 3);
            }
            CHECK(r->capabilities.has_value() == bool(mask & 2));
            if (r->capabilities) {
                const auto &caps = *r->capabilities;
                CHECK(caps.control_version == 4 && caps.data_version == bdsp_protocol::numeric_data::PayloadVersion && caps.codec_mask == 3 &&
                      caps.rows.size() == 1);
                CHECK(caps.maximum.plan == 31 && caps.maximum.payload == 32 && caps.maximum.scratch == 33 &&
                      caps.maximum.stack == 34 && caps.maximum.peak == 35);
                if (caps.rows.size() == 1) {
                    const auto &configuration = caps.rows[0];
                    check_node(configuration.combination);
                    CHECK(configuration.kernel == "k" && configuration.backend == "b");
                    CHECK(configuration.values == signal_contracts::ValueDomain::FiniteOnly);
                    CHECK(configuration.resources.plan == 21 && configuration.resources.payload == 22 &&
                          configuration.resources.scratch == 23 && configuration.resources.stack == 24 &&
                          configuration.resources.peak == 25);
                }
            }
            CHECK(m.extensions.size() == (has_extension ? 1U : 0U));
            if (has_extension && m.extensions.size() == 1) {
                CHECK(m.extensions[0].tag == 1024 && m.extensions[0].bytes.size() == 5);
                CHECK(std::equal(m.extensions[0].bytes.begin(), m.extensions[0].bytes.end(), extension_bytes.begin(),
                                 extension_bytes.end()));
            }
        };
        // Owned decode subtrees must survive reuse of both incoming bytes and staging.
        wire.fill(0);
        stage.fill(std::byte{0});
        check_reply(decoded, true);
        encoded = encode(c, message);
        Message without_extension = message;
        without_extension.extensions = {};
        tracking = true;
        const auto bare = c.measure(without_extension, limits);
        tracking = false;
        CHECK(bare);
        constexpr std::array<std::uint8_t, 12> protobuf_extension{0x4a, 0x0a, 0x08, 0x80, 0x08, 0x12,
                                                                  0x05, 1,    2,    3,    4,    5};
        if (c.id() == CodecId::Protobuf) {
            CHECK(encoded.produced >= protobuf_extension.size());
            CHECK(bare.required_wire + protobuf_extension.size() == encoded.produced);
            if (encoded.produced >= protobuf_extension.size())
                CHECK(std::equal(protobuf_extension.begin(), protobuf_extension.end(),
                                 wire.begin() + encoded.produced - protobuf_extension.size()));
        }
        // Protobuf permits omission of the trailing repeated extension field.
        // All other proper prefixes cut a required field or a declared container.
        for (std::size_t n = 0; n < encoded.produced; ++n) {
            arena.fill(std::byte{0xa5});
            Message prefix;
            prefix.client = 66;
            prefix.session = 98;
            prefix.epoch = 99;
            prefix.request = 77;
            prefix.expected_revision = 97;
            std::get<Command>(prefix.body).id = CommandId::Stop;
            const auto result = decode(c, n, prefix);
            const bool valid_prefix = c.id() == CodecId::Protobuf && n + protobuf_extension.size() == encoded.produced;
            CHECK(bool(result) == valid_prefix);
            if (valid_prefix) {
                CHECK(result.consumed == n && result.produced == 0);
                check_reply(prefix, false);
            } else {
                CHECK(result.consumed == 0 && result.produced == 0);
                CHECK(prefix.client == 66 && prefix.request == 77 && std::holds_alternative<Command>(prefix.body));
                CHECK(prefix.schema_version == 4 && prefix.session == 98 && prefix.epoch == 99 &&
                      prefix.expected_revision == 97 && prefix.extensions.empty());
                if (const auto *command = std::get_if<Command>(&prefix.body)) {
                    CHECK(command->id == CommandId::Stop && !command->graph && !command->parameters &&
                          command->graph_id.empty() && command->token == 0 && command->graph_revision == 0 &&
                          command->node == -1 && command->enabled);
                }
                CHECK(std::all_of(arena.begin(), arena.end(), [](auto b) { return b == std::byte{0xa5}; }));
            }
        }
        auto bound = limits;
        bound.arena_bytes = measured.required_arena - 1;
        tracking = true;
        CHECK(c.measure(message, bound).status == Status::Limit);
        tracking = false;
        arena.fill(std::byte{0xa5});
        decoded.request = 77;
        tracking = true;
        auto short_arena = c.decode(std::span(wire).first(encoded.produced),
                                    std::span(arena).first(measured.required_arena - 1), stage, decoded, limits);
        auto short_stage = c.decode(std::span(wire).first(encoded.produced), arena,
                                    std::span(stage).first(measured.required_arena - 1), decoded, limits);
        tracking = false;
        CHECK(short_arena.status == Status::BufferTooSmall && short_stage.status == Status::BufferTooSmall);
        CHECK(short_arena.consumed == 0 && short_stage.consumed == 0 && decoded.request == 77);
        CHECK(std::all_of(arena.begin(), arena.end(), [](auto b) { return b == std::byte{0xa5}; }));
        wire.fill(0xcc);
        tracking = true;
        auto short_output = c.encode(message, std::span(wire).first(measured.required_wire - 1), scratch, limits);
        auto short_scratch = c.encode(message, wire, std::span(scratch).first(measured.required_scratch - 1), limits);
        tracking = false;
        CHECK(short_output.status == Status::BufferTooSmall && short_output.consumed == 0 &&
              short_output.produced == 0);
        CHECK(short_output.required_wire == measured.required_wire &&
              short_output.required_scratch == measured.required_scratch);
        CHECK(short_scratch.status == Status::BufferTooSmall && short_scratch.produced == 0);
        CHECK(std::all_of(wire.begin(), wire.end(), [](auto b) { return b == 0xcc; }));
    }
}
void contracts_and_literals(const Codec &c) {
    using namespace signal_contracts;
    std::array<std::uint32_t, 2> channels{7, 13};
    std::array<Literal, 10> values{INT64_MIN,
                                   INT64_MAX,
                                   UINT64_MAX,
                                   std::uint64_t{9007199254740993ULL},
                                   std::int64_t{-9007199254740993LL},
                                   std::bit_cast<float>(0x80000000U),
                                   std::bit_cast<float>(1U),
                                   std::bit_cast<float>(0x7fc01234U),
                                   std::bit_cast<double>(0x8000000000000000ULL),
                                   std::bit_cast<double>(0x7ff8000000001234ULL)};
    std::array<Parameter, 10> params{};
    const std::string_view keys[]{"a", "b", "c", "d", "e", "f", "g", "h", "i", "j"};
    for (std::size_t i = 0; i < values.size(); ++i)
        params[i] = {keys[i], values[i]};
    for (auto desc : dtype_descriptors())
        for (auto rank : std::array{Rank::Scalar, Rank::Vector, Rank::Matrix}) {
            NumericContract contract;
            contract.dtype = desc.type;
            contract.shape.rank = rank;
            contract.semantic = Semantic::Numeric;
            contract.representation.encoding = StorageEncoding::FixedLittleEndian;
            contract.channel_order = {channels.data(), rank == Rank::Scalar ? 1U : 2U};
            contract.clock.domain = "sample-clock";
            if (rank != Rank::Scalar) {
                contract.shape.axes[0] = Axis::Declared;
                contract.shape.extents[0] = {1, 4};
                contract.coordinates[0].mode = CoordinateMode::Regular;
                contract.coordinates[0].step = {1, 4};
            }
            if (rank == Rank::Matrix) {
                contract.shape.axes[1] = Axis::Declared;
                contract.shape.extents[1] = {2, 3};
                contract.representation.layout = ChannelLayout::Interleaved;
            }
            std::array<Port, 1> ports{Port{0, domain::PortDirection::Output, contract}};
            std::array<Node, 1> nodes{Node{42, "NetworkInput", EndpointParameters{params}, {}, ports, true}};
            Graph g{"graph", {}, nodes, {}};
            g.versions.document = bdsp_protocol::document_version;
            g.versions.control = bdsp_protocol::control_version;
            g.versions.data = bdsp_protocol::processing_data_version;
            Message m;
            Command cmd;
            cmd.id = CommandId::Prepare;
            cmd.graph = g;
            m.body = cmd;
            auto e = encode(c, m);
            CHECK(e);
            if (!e)
                continue;
            Message out;
            auto d = decode(c, e.produced, out);
            CHECK(d);
            if (!d)
                continue;
            wire.fill(0);
            stage.fill(std::byte{0});
            const auto &dg = *std::get<Command>(out.body).graph;
            CHECK(dg.graph_id == "graph");
            CHECK(dg.nodes.size() == 1);
            CHECK(dg.versions.document == bdsp_protocol::document_version);
            CHECK(dg.nodes[0].id == 42);
            CHECK(same_contract(dg.nodes[0].outputs[0].contract, contract));
            const auto &pp = std::get<EndpointParameters>(dg.nodes[0].parameters).values;
            CHECK(pp.size() == 10);
            for (std::size_t i = 0; i < pp.size(); ++i)
                CHECK(same_literal(std::get<Literal>(pp[i].value), values[i]));
            // Exact arena exhaustion, including owned descriptors/text/channel IDs.
            e = encode(c, m);
            out.request = 77;
            arena.fill(std::byte{0xa5});
            tracking = true;
            auto short_result =
                c.decode({wire.data(), e.produced}, {arena.data(), d.required_arena - 1}, stage, out, limits);
            tracking = false;
            CHECK(short_result.status == bdsp_protocol::numeric_control::Status::BufferTooSmall);
            CHECK(short_result.required_arena == d.required_arena);
            CHECK(out.request == 77);
            CHECK(arena.back() == std::byte{0xa5});
            CHECK(arena.front() == std::byte{0xa5});
        }
    // Explicit packed24 and valid24-in32, distinct placements and endian.
    for (auto enc : std::array{StorageEncoding::PackedSigned24LE, StorageEncoding::PackedSigned24BE,
                               StorageEncoding::FixedLittleEndian, StorageEncoding::FixedBigEndian}) {
        NumericContract n;
        n.dtype = DType::Int32;
        n.semantic = Semantic::PCM;
        n.channel_order = {channels.data(), 1};
        n.representation.encoding = enc;
        n.representation.valid_bits = 24;
        if (enc == StorageEncoding::FixedBigEndian)
            n.representation.placement = BitPlacement::MostSignificant;
        Port p{0, domain::PortDirection::Output, n};
        Node node{1, "NetworkInput", EndpointParameters{}, {}, {&p, 1}};
        Graph g{"pcm", {}, {&node, 1}, {}};
        Command cmd;
        cmd.id = CommandId::Prepare;
        cmd.graph = g;
        Message m;
        m.body = cmd;
        auto e = encode(c, m);
        CHECK(e);
        Message out;
        const auto decoded=decode(c,e.produced,out);CHECK(decoded);if(!decoded)continue;
        CHECK(same_contract(std::get<Command>(out.body).graph->nodes[0].outputs[0].contract, n));
    }
    StructuredContract sc{"custom/schema", 7, "opaque-v7", {8, 256}};
    Port p{17, domain::PortDirection::Output, sc};
    ConversionPolicy policy;
    policy.scaling.gain.numerator = UINT64_MAX;
    policy.nan_replacement = std::bit_cast<double>(0x7ff8000000004321ULL);
    Node n{3, "Converter", domain::ConverterProcessingParameters{policy}, {}, {&p, 1}};
    Graph g{"structured", {}, {&n, 1}, {}};
    Command cmd;
    cmd.id = CommandId::Prepare;
    cmd.graph = g;
    Message m;
    m.body = cmd;
    auto e = encode(c, m);
    CHECK(e);
    Message out;
    CHECK(decode(c, e.produced, out));
    auto &result =
        std::get<domain::ConverterProcessingParameters>(std::get<Command>(out.body).graph->nodes[0].parameters).policy;
    CHECK(same_literal(result.scaling.gain.numerator, policy.scaling.gain.numerator));
    CHECK(same_literal(result.nan_replacement, policy.nan_replacement));
    CHECK(same_contract(std::get<Command>(out.body).graph->nodes[0].outputs[0].contract, sc));
    g.versions.graph = 2;
    cmd.graph = g;
    m.body = cmd;
    CHECK(encode(c, m).status == bdsp_protocol::numeric_control::Status::UnsupportedVersion);
    // Unknown optional extension bytes are owned, including NUL/non-UTF8.
    std::array<std::uint8_t, 4> bytes{0, 255, 1, 2};
    Extension ext{1024, bytes};
    m = {};
    m.extensions = {&ext, 1};
    e = encode(c, m);
    CHECK(e);
    CHECK(decode(c, e.produced, out));
    wire.fill(0);
    stage.fill(std::byte{0});
    CHECK(out.extensions[0].bytes[1] == 255);
    ext.tag = 0x8000;
    CHECK(encode(c, m).status == bdsp_protocol::numeric_control::Status::UnsupportedField);
    std::array<Extension, 2> duplicates{Extension{1024, bytes}, Extension{1024, bytes}};
    m.extensions = duplicates;
    CHECK(!encode(c, m));
    params[1].key = params[0].key;
    n.parameters = EndpointParameters{params};
    g.versions.graph = 5;
    g.nodes = {&n, 1};
    cmd.graph = g;
    m = {};
    m.body = cmd;
    CHECK(!encode(c, m));
}
void check_wled_peak_parameters(const Message& message,std::span<const Parameter> expected) {
    const auto& command=std::get<Command>(message.body);
    const auto values=std::get<TransformParameters>(*command.parameters).values;
    CHECK(values.size()==expected.size());
    if(values.size()!=expected.size())return;
    for(std::size_t index=0;index<values.size();++index) {
        CHECK(values[index].key==expected[index].key);
        CHECK(same_literal(std::get<Literal>(values[index].value),std::get<Literal>(expected[index].value)));
    }
}
void wled_peak_parameters_roundtrip(const Codec& codec) {
    const std::array<Parameter,3> parameters{Parameter{"gain",Literal{1.f}},Parameter{"peak_hz",Literal{131.25f}},Parameter{"magnitude",Literal{.75f}}};
    Command command;command.id=CommandId::SetParameters;command.node=13;command.parameters=TransformParameters{parameters};
    Message message;message.body=command;const auto encoded=encode(codec,message);CHECK(encoded);
    if(!encoded)return;
    Message restored;CHECK(decode(codec,encoded.produced,restored));check_wled_peak_parameters(restored,parameters);
}
void single_stream_parameters(const Codec& codec) {
    wled_peak_parameters_roundtrip(codec);
    std::array<signal_contracts::OscillatorParameters,2> tones{};
    tones[0]={9007199254740993ULL,true,440.25,0.75,-0.25,0.125,0.5};
    tones[1]={UINT64_MAX,false,220.5,-0.125,1.25,0.0,2.0};
    std::array<domain::MixerInputWeight,2> weights{{{3,0.25},{9,0.75}}};
    std::array<Parameters,3> values{GeneratorParameters{tones},ClockParameters{44100,"exact-clock"},MixerParameters{domain::MixerMode::WeightedRMS,weights,125}};
    for(const auto& parameters:values) {
        Node node{17,"typed",parameters,{},{},true};Graph graph;graph.nodes={&node,1};
        Command command;command.id=CommandId::Prepare;command.graph=graph;Message message;message.body=command;
        auto encoded=encode(codec,message);CHECK(encoded);Message restored;CHECK(decode(codec,encoded.produced,restored));
        const auto& result=std::get<Command>(restored.body).graph->nodes[0];CHECK(result.parameters.index()==parameters.index());
        if(const auto* generator=std::get_if<GeneratorParameters>(&result.parameters)) {
            CHECK(generator->oscillators.size()==2);
            for(std::size_t i=0;i<2;++i){const auto& a=generator->oscillators[i];const auto& b=tones[i];CHECK(a.id==b.id&&a.enabled==b.enabled&&a.frequency_hz==b.frequency_hz&&a.amplitude==b.amplitude&&a.phase_radians==b.phase_radians&&a.attack_seconds==b.attack_seconds&&a.release_seconds==b.release_seconds);}
        }
        if(const auto* clock=std::get_if<ClockParameters>(&result.parameters))CHECK(clock->sample_rate==44100&&clock->domain=="exact-clock");
        if(const auto* mixer=std::get_if<MixerParameters>(&result.parameters)){CHECK(mixer->mode==domain::MixerMode::WeightedRMS&&mixer->fifo_milliseconds==125&&mixer->weights.size()==2);for(std::size_t i=0;i<2;++i)CHECK(mixer->weights[i].port==weights[i].port&&mixer->weights[i].weight==weights[i].weight);}
    }
}
void binding_regression(const Codec& codec) {
    const std::array<domain::ConfigurationBinding,2> bindings{{{1,2},{3,4,domain::ConfigurationRole::FrequencyGrid,domain::ConfigurationRole::FrequencyGrid}}};
    Graph graph;graph.graph_id="bindings";graph.configuration_bindings=bindings;
    Command command;command.id=CommandId::Prepare;command.graph=graph;Message message;message.body=command;
    const auto encoded=encode(codec,message);CHECK(encoded);Message restored;CHECK(decode(codec,encoded.produced,restored));
    const auto& copy=std::get<Command>(restored.body).graph->configuration_bindings;CHECK(copy.size()==2);for(std::size_t i=0;i<copy.size();++i)CHECK(copy[i]==bindings[i]);
    domain::RouteProcessingParameters route;route.outputs=3;route.channels_per_stream=2;route.selected=2;
    command.graph.reset();command.id=CommandId::SetParameters;command.node=1;command.parameters=route;message.body=command;
    const auto routed=encode(codec,message);CHECK(routed);CHECK(decode(codec,routed.produced,restored));const auto& result=std::get<domain::RouteProcessingParameters>(*std::get<Command>(restored.body).parameters);CHECK(result.outputs==3 && result.channels_per_stream==2 && result.selected==2);
    const std::array<domain::MixerInputWeight,1> weights{{{0,1.0}}};MixerParameters mixer;mixer.mode=domain::MixerMode::WeightedMean;mixer.weights=weights;mixer.consumption=domain::MixerConsumption::SourceAligned;
    command.parameters=mixer;message.body=command;const auto mixer_bytes=encode(codec,message);CHECK(mixer_bytes);CHECK(decode(codec,mixer_bytes.produced,restored));CHECK(std::get<MixerParameters>(*std::get<Command>(restored.body).parameters).consumption==domain::MixerConsumption::SourceAligned);

}
void interval_regression(const Codec& codec) {
    Graph graph;graph.graph_id="interval";graph.execution_interval={1,125};Command command;command.id=CommandId::Prepare;command.graph=graph;Message message;message.body=command;
    auto encoded=encode(codec,message);CHECK(encoded);Message restored;CHECK(decode(codec,encoded.produced,restored));
    const auto interval=std::get<Command>(restored.body).graph->execution_interval;CHECK(interval.numerator==1&&interval.denominator==125);
    for(const signal_contracts::Ratio invalid:std::array{signal_contracts::Ratio{0,1},signal_contracts::Ratio{1,0},signal_contracts::Ratio{2,120},signal_contracts::Ratio{61,1}}) {
        std::get<Command>(message.body).graph->execution_interval=invalid;CHECK(!encode(codec,message));
    }
}
void messages(const Codec &c) {
    for (auto id : std::array{CommandId::Capabilities, CommandId::State, CommandId::Apply, CommandId::SetParameters,
                              CommandId::Start, CommandId::Stop, CommandId::Persist, CommandId::Release}) {
        Command cmd;
        cmd.id = id;
        if (id == CommandId::Apply) {
            cmd.graph_id = "g";
            cmd.token = UINT64_MAX;
        }
        if (id == CommandId::SetParameters) {
            cmd.node = 0;
            cmd.parameters = domain::HistoryProcessingParameters{UINT64_MAX};
        }
        if (id == CommandId::Persist)
            cmd.graph_revision = UINT64_MAX;
        if (id == CommandId::Release)
            cmd.token = UINT64_MAX;
        Message m;
        m.body = cmd;
        auto e = encode(c, m);
        if(id==CommandId::Start||id==CommandId::Stop){CHECK(!e);continue;}
        CHECK(e);
        Message out;
        CHECK(decode(c, e.produced, out));
        CHECK(std::get<Command>(out.body).id == id);
    }
    Node n{0, "Gate", domain::RouteProcessingParameters{1, 0, true}, {}, {}};
    CapabilityRow row{
        n, "implemented-gate", "test-backend", signal_contracts::ValueDomain::AnyIeee, {UINT64_MAX, 1, 2, 3, 4}};
    Reply r;
    r.command = CommandId::Capabilities;
    r.state.revision = UINT64_MAX;
    r.diagnostic.node = 0;
    r.diagnostic.port = 0;
    r.diagnostic.edge = 0;
    r.capabilities = Capabilities{4, bdsp_protocol::numeric_data::PayloadVersion, 3, {&row, 1}, {1, 2, 3, 4, 5}};
    Message m;
    m.request = 123;
    m.body = r;
    auto e = encode(c, m);
    CHECK(e);
    Message out;
    CHECK(decode(c, e.produced, out));
    const auto &reply = std::get<Reply>(out.body);
    CHECK(reply.diagnostic.node.has_value());
    CHECK(reply.diagnostic.node == 0);
    CHECK(reply.capabilities->rows[0].resources.plan == UINT64_MAX);
    CHECK(reply.capabilities->rows[0].backend == "test-backend");
    auto bound = limits;
    bound.capability_rows = 0;
    CHECK(encode(c, m, bound).status == bdsp_protocol::numeric_control::Status::Limit);
    std::string_view invalid{"\xc0\x80", 2};
    r.diagnostic.parameter = invalid;
    m.body = r;
    CHECK(!encode(c, m));
}
void alternatives_and_limits(const Codec &c) {
    using namespace signal_contracts;
    std::uint32_t selected[]{17, 23};
    std::array<Parameter, 2> named{Parameter{"flag", true}, Parameter{"name", std::string_view{"exact"}}};
    std::array<Parameters, 6> alternatives{EndpointParameters{named},
                                           TransformParameters{named},
                                           domain::ConverterProcessingParameters{},
                                           domain::RouteProcessingParameters{8, 7, false},
                                           ReductionParameters{domain::NumericReduction::RMS,
                                                               domain::ReductionChannels::SelectedChannels,
                                                               domain::ReductionRows::LatestHistoryRow, selected,
                                                               PrecisionLoss::AllowDeclared, Rounding::TowardZero},
                                           domain::HistoryProcessingParameters{UINT64_MAX}};
    for (const auto &alternative : alternatives) {
        Command cmd;
        cmd.id = CommandId::SetParameters;
        cmd.node = INT32_MAX;
        cmd.parameters = alternative;
        Message m;
        m.body = cmd;
        auto e = encode(c, m);
        CHECK(e);
        Message out;
        auto d = decode(c, e.produced, out);
        CHECK(d);
        if (!d)
            continue;
        auto &p = *std::get<Command>(out.body).parameters;
        CHECK(p.index() == alternative.index());
        if (auto v = std::get_if<TransformParameters>(&p)) {
            CHECK(std::get<bool>(v->values[0].value));
            CHECK(std::get<std::string_view>(v->values[1].value) == "exact");
        }
        if (auto v = std::get_if<ReductionParameters>(&p)) {
            CHECK(v->selected_channels[1] == 23);
            CHECK(v->rows == domain::ReductionRows::LatestHistoryRow);
        }
        arena.fill(std::byte{0x5a});
        out.request = 71;
        tracking = true;
        auto short_stage = c.decode({wire.data(), e.produced}, arena,
                                    {stage.data(), d.required_scratch ? d.required_scratch - 1 : 0}, out, limits);
        tracking = false;
        if (d.required_scratch) {
            CHECK(short_stage.status == bdsp_protocol::numeric_control::Status::BufferTooSmall);
            CHECK(out.request == 71);
            CHECK(arena[0] == std::byte{0x5a});
        }
        auto bound = limits;
        bound.arena_bytes = 0;
        if (d.required_arena)
            CHECK(!decode(c, e.produced, out, bound));
    }
    for (auto mode : std::array{ScaleMode::Identity, ScaleMode::IntegerAffine, ScaleMode::DyadicAffine,
                                ScaleMode::PcmNormalize, ScaleMode::PcmDenormalize, ScaleMode::Binary64Affine}) {
        ConversionPolicy p;
        p.scaling.mode = mode;
        p.rounding = Rounding::TowardZero;
        p.overflow = Overflow::Saturate;
        p.precision_loss = PrecisionLoss::AllowDeclared;
        p.nonfinite = NonFinite::ClampFinite;
        p.scaling.gain = {std::uint64_t{UINT64_MAX}, -63};
        p.scaling.offset = {std::int64_t{INT64_MIN}, 63};
        p.nan_replacement = std::bit_cast<float>(0x80000000U);
        p.negative_inf_replacement = std::int64_t{-1};
        p.positive_inf_replacement = std::uint64_t{1};
        Command cmd;
        cmd.id = CommandId::SetParameters;
        cmd.parameters = domain::ConverterProcessingParameters{p};
        Message m;
        m.body = cmd;
        auto e = encode(c, m);
        CHECK(e);
        Message out;
        CHECK(decode(c, e.produced, out));
        const auto &q = std::get<domain::ConverterProcessingParameters>(*std::get<Command>(out.body).parameters).policy;
        CHECK(q.scaling.mode == mode);
        CHECK(q.scaling.gain.exponent2 == -63);
        CHECK(q.scaling.offset.exponent2 == 63);
        CHECK(same_literal(q.scaling.gain.numerator, p.scaling.gain.numerator));
        CHECK(same_literal(q.scaling.offset.numerator, p.scaling.offset.numerator));
        CHECK(same_literal(q.nan_replacement, p.nan_replacement));
    }
    Reply reply;
    reply.diagnostic.node = INT32_MIN;
    reply.diagnostic.port = UINT16_MAX;
    reply.diagnostic.edge = UINT64_MAX;
    Message m;
    m.body = reply;
    auto e = encode(c, m);
    CHECK(e);
    Message out;
    CHECK(decode(c, e.produced, out));
    CHECK(std::get<Reply>(out.body).diagnostic.node == INT32_MIN);
    CHECK(std::get<Reply>(out.body).diagnostic.port == UINT16_MAX);
    CHECK(encode(c, Message{}, device_limits()));
    CHECK(encode(c, Message{}, desktop_limits()));
    std::array<std::uint8_t, 4> bytes{};
    std::array<Extension, 2> ext{Extension{1024, bytes}, Extension{1025, bytes}};
    m = {};
    m.extensions = ext;
    auto bound = limits;
    bound.extension_bytes = 7;
    CHECK(encode(c, m, bound).status == bdsp_protocol::numeric_control::Status::Limit);
    bound = limits;
    bound.records = 1;
    CHECK(encode(c, m, bound).status == bdsp_protocol::numeric_control::Status::Limit);
    auto e2 = encode(c, m);
    CHECK(e2);
    arena.fill(std::byte{0x5a});
    CHECK(decode(c, e2.produced, out, bound).status == bdsp_protocol::numeric_control::Status::Limit);
    CHECK(arena[0] == std::byte{0x5a});
    bound = limits;
    bound.depth = 33;
    CHECK(encode(c, Message{}, bound).status == bdsp_protocol::numeric_control::Status::InvalidArgument);
    CHECK(!c.decode({}, arena, stage, out, limits));
    e = encode(c, Message{});
    CHECK(e);
    tracking = true;
    auto zero_arena = c.decode({wire.data(), e.produced}, {}, {}, out, limits);
    tracking = false;
    CHECK(zero_arena);
}
void literal_goldens(const Codec &c) {
    const std::array<Literal, 4> values{std::int64_t{INT64_MIN}, std::uint64_t{UINT64_MAX},
                                        std::bit_cast<float>(0x80000000U),
                                        std::bit_cast<double>(0x8000000000000000ULL)};
    const std::uint8_t cb0[]{0xa2, 1, 1, 2, 0x3b, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const std::uint8_t cb1[]{0xa2, 1, 2, 3, 0x1b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const std::uint8_t cb2[]{0xa2, 1, 3, 4, 0x1a, 0x80, 0, 0, 0};
    const std::uint8_t cb3[]{0xa2, 1, 4, 5, 0x1b, 0x80, 0, 0, 0, 0, 0, 0, 0};
    const std::uint8_t pb0[]{8, 1, 16, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1};
    const std::uint8_t pb1[]{8, 2, 24, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1};
    const std::uint8_t pb2[]{8, 3, 37, 0, 0, 0, 0x80};
    const std::uint8_t pb3[]{8, 4, 41, 0, 0, 0, 0, 0, 0, 0, 0x80};
    const std::array<std::span<const std::uint8_t>, 4> cb{cb0, cb1, cb2, cb3}, pb{pb0, pb1, pb2, pb3};
    for (std::size_t i = 0; i < values.size(); ++i) {
        Parameter p{"value", values[i]};
        Command cmd;
        cmd.id = CommandId::SetParameters;
        cmd.parameters = EndpointParameters{{&p, 1}};
        Message m;
        m.body = cmd;
        auto e = encode(c, m);
        CHECK(e);
        auto expected = c.id() == CodecId::Cbor ? cb[i] : pb[i];
        CHECK(std::search(wire.begin(), wire.begin() + e.produced, expected.begin(), expected.end()) !=
              wire.begin() + e.produced);
    }
}
void codec_mask_regression(const Codec &c) {
    Node combination{0, "Gate", domain::RouteProcessingParameters{1, 0, true}, {}, {}};
    CapabilityRow row{combination, "gate", "test", signal_contracts::ValueDomain::FiniteOnly, {}};
    for (std::uint32_t mask : {0U, 1U, 2U, 3U, 4U, 5U, 8U}) {
        const bool valid = mask >= 1 && mask <= 3;
        Reply reply;
        reply.command = CommandId::Capabilities;
        reply.capabilities = Capabilities{4, bdsp_protocol::numeric_data::PayloadVersion, mask, {&row, 1}, {}};
        Message message;
        message.body = reply;
        auto validated = validate(message, limits);
        CHECK(bool(validated) == valid);
        tracking = true;
        auto measured = c.encode(message, {}, {}, limits);
        tracking = false;
        CHECK(measured.status == (valid ? Status::BufferTooSmall : Status::UnsupportedVersion));
        CHECK(measured.produced == 0);
        CHECK(valid ? measured.required_wire > 0 : measured.required_wire == 0);
        wire.fill(0xa5);
        const auto untouched_wire = wire;
        auto encoded = encode(c, message);
        CHECK(bool(encoded) == valid);
        if (!valid) {
            CHECK(encoded.produced == 0);
            CHECK(wire == untouched_wire);
        }

        // Produce a structurally complete wire fixture with a legal mask, then
        // mutate just its capability scalar. No invalid-encode bootstrap needed.
        reply.capabilities->codec_mask = 1;
        message.body = reply;
        auto fixture = encode(c, message);
        CHECK(fixture);
        const std::uint8_t cb[]{0xa5, 1, 4, 2, 6, 3, 1};
        const std::uint8_t pb[]{8, 4, 16, 6, 24, 1};
        auto needle = c.id() == CodecId::Cbor ? std::span<const std::uint8_t>{cb} : std::span<const std::uint8_t>{pb};
        auto end = wire.begin() + fixture.produced;
        auto match = std::search(wire.begin(), end, needle.begin(), needle.end());
        CHECK(match != end);
        if (match == end)
            continue;
        CHECK(std::search(match + 1, end, needle.begin(), needle.end()) == end);
        *(match + needle.size() - 1) = static_cast<std::uint8_t>(mask);
        Message output;
        output.request = 77;
        Command sentinel;
        sentinel.id = CommandId::Stop;
        output.body = sentinel;
        arena.fill(std::byte{0x5a});
        const auto untouched_arena = arena;
        auto decoded = decode(c, fixture.produced, output);
        CHECK(bool(decoded) == valid);
        if (valid)
            CHECK(std::get<Reply>(output.body).capabilities->codec_mask == mask);
        else {
            CHECK(decoded.status == Status::UnsupportedVersion);
            CHECK(decoded.consumed == 0);
            CHECK(output.request == 77);
            CHECK(std::holds_alternative<Command>(output.body));
            if (auto preserved = std::get_if<Command>(&output.body))
                CHECK(preserved->id == CommandId::Stop);
            CHECK(arena == untouched_arena);
        }
    }
}
#ifdef NUMERIC_CONTROL_MAPPER_TESTS
void projection_binding_bounds(control::numeric::GraphProjection& projection,const domain::NumericProcessingGraph& graph) {
    auto oversized=graph;oversized.configuration_bindings.resize(limits.edges+1);
    const auto* retained_nodes=projection.wire().nodes.data();
    const auto retained_identity=projection.wire().graph_id;
    const auto before=allocations;tracking=true;
    const auto rejected=projection.assign(oversized,limits);
    tracking=false;
    CHECK(rejected.status==Status::Limit);
    CHECK(allocations==before);
    CHECK(projection.wire().nodes.data()==retained_nodes && projection.wire().graph_id==retained_identity);
}
#endif
void native_descriptor_regression(const Codec &c) {
    namespace sc = signal_contracts;
    std::array<std::uint32_t, 1> channels{7};
    // Native buffered PCM, explicit coefficient generator and FFT descriptors.
    // WindowFunction consumes scalar N and produces coefficients; it does not transform PCM.
    sc::NumericContract pcm;
    pcm.dtype = sc::DType::Float32;
    pcm.semantic = sc::Semantic::PCM;
    pcm.quantity = sc::Quantity::Amplitude;
    pcm.unit = sc::NumericUnit::NormalizedAmplitude;
    pcm.shape.rank = sc::Rank::Vector;
    pcm.shape.axes = {sc::Axis::Sample, sc::Axis::None};
    pcm.shape.extents = {sc::ExtentBounds{1, 4096}, sc::ExtentBounds{1, 1}};
    pcm.channel_order = channels;
    pcm.clock = {"audio.test", {1, 48000}, 48000};
    auto win = pcm;
    win.semantic = sc::Semantic::Window;
    win.shape.extents[0] = {512, 512};
    auto length = pcm;
    length.dtype = sc::DType::UInt32;
    length.semantic = sc::Semantic::Scalar;
    length.quantity = sc::Quantity::Unspecified;
    length.unit = sc::NumericUnit::Values;
    length.shape = {};
    auto coefficients = win;
    coefficients.semantic = sc::Semantic::Numeric;
    coefficients.quantity = sc::Quantity::Unspecified;
    coefficients.unit = sc::NumericUnit::Values;
    coefficients.representation = {};
    coefficients.representation.alignment = alignof(float);
    auto spectrum = win;
    spectrum.semantic = sc::Semantic::SpectrumMagnitude;
    spectrum.quantity = sc::Quantity::SpectrumMagnitude;
    spectrum.unit = sc::NumericUnit::Values;
    spectrum.shape.axes[0] = sc::Axis::Bin;
    spectrum.shape.extents[0] = {256, 256};
    spectrum.coordinates[0] = {sc::CoordinateMode::Regular, sc::NumericUnit::Hertz, {0, 1}, {48000, 512}};
    const std::array<sc::NumericContract, 3> input_contracts{pcm, length, win};
    const std::array<sc::NumericContract, 3> output_contracts{win, coefficients, spectrum};
    domain::NumericProcessingGraph owned;
    owned.graph_id = "typed.dsp.native";
    const std::string_view kinds[]{"Window", "WindowFunction", "StereoFFT"};
    for (std::size_t i = 0; i < 3; ++i) {
        domain::ConfiguredProcessingNode n;
        n.id = static_cast<domain::NodeId>(i + 2);
        n.kind = kinds[i];
        n.parameters = domain::TransformProcessingParameters{};
        if (i < 2)
            std::get<domain::TransformProcessingParameters>(n.parameters)
                .values.push_back({"size", Literal{std::uint64_t{512}}});
        if (i == 0)
            std::get<domain::TransformProcessingParameters>(n.parameters)
                .values.push_back({"input_rate", Literal{std::uint64_t{48000}}});
        for (bool input : {true, false}) {
            domain::OwnedProcessingContract contract;
            CHECK(domain::own_processing_contract((input ? input_contracts[i] : output_contracts[i]), limits.frame, 65536, contract) ==
                  sc::Status::ok);
            (input ? n.inputs : n.outputs)
                .push_back(
                    {0, input ? domain::PortDirection::Input : domain::PortDirection::Output, std::move(contract)});
        }
        owned.nodes.push_back(std::move(n));
    }
    owned.edges = {{2, 4, 0, 0}};
    domain::ProcessingAdmissionLimits admission{128, 256, 512, 512, 256, 65536, limits.frame};
    for (const auto &n : owned.nodes) {
        domain::ResolvedProcessingNode resolved;
        CHECK(domain::resolve_processing_node(n, admission, resolved));
    }
    std::array<Port, 3> inputs{}, outputs{};
    std::array<Node, 3> nodes{};
    Parameter size{"size", Literal{std::uint64_t{512}}};
    Parameter input_rate{"input_rate", Literal{std::uint64_t{48000}}};
    std::array<Parameter, 2> window_parameters{size, input_rate};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto &n = owned.nodes[i];
        inputs[i] = {0, domain::PortDirection::Input, n.inputs[0].contract.view()};
        outputs[i] = {0, domain::PortDirection::Output, n.outputs[0].contract.view()};
        nodes[i] = {n.id,
                    n.kind,
                    TransformParameters{i == 0 ? std::span<const Parameter>{window_parameters}
                                               : i == 1 ? std::span<const Parameter>{&size, 1}
                                                        : std::span<const Parameter>{}},
                    {&inputs[i], 1},
                    {&outputs[i], 1},
                    true};
    }
    const GraphVersions envelope{bdsp_protocol::document_version,owned.versions.graph,owned.versions.processing,
        bdsp_protocol::control_version,bdsp_protocol::processing_data_version};
    Graph g{owned.graph_id, envelope, nodes, owned.edges};
#ifdef NUMERIC_CONTROL_MAPPER_TESTS
    control::numeric::GraphProjection projection;
    auto projected = projection.assign(owned, limits);
    CHECK(projected);
    if (projected)
        g = projection.wire();
    if (projected)
        projection_binding_bounds(projection,owned);
#endif
    for (bool applied : {false, true}) {
        Message m;
        if (applied) {
            Reply r;
            r.command = CommandId::Persist;
            r.state.revision = 17;
            r.state.applied = g;
            m.body = r;
        } else {
            Command cmd;
            cmd.id = CommandId::Prepare;
            cmd.graph = g;
            m.body = cmd;
        }
        auto e = encode(c, m);
        CHECK(e);
        if (!e)
            continue;
        Message out;
        auto d = decode(c, e.produced, out);
        CHECK(d);
        if (!d)
            continue;
        const auto &restored = applied ? *std::get<Reply>(out.body).state.applied : *std::get<Command>(out.body).graph;
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(sc::same_contract(restored.nodes[i].inputs[0].contract, input_contracts[i]));
            CHECK(sc::same_contract(restored.nodes[i].outputs[0].contract, output_contracts[i]));
            CHECK(std::get<sc::NumericContract>(restored.nodes[i].outputs[0].contract).representation.encoding ==
                  sc::StorageEncoding::Native);
        }
#ifdef NUMERIC_CONTROL_MAPPER_TESTS
        domain::NumericProcessingGraph cold_owned;
        CHECK(control::numeric::own_graph(restored, limits, cold_owned));
        wire.fill(0);
        stage.fill(std::byte{0});
        arena.fill(std::byte{0});
        for (std::size_t i = 0; i < 3; ++i)
            CHECK(sc::same_contract(cold_owned.nodes[i].outputs[0].contract.view(), output_contracts[i]));
#endif
        // Failed encode/decode may not publish a partial Native graph.
        wire.fill(0xa5);
        const auto before = wire;
        tracking = true;
        auto short_output = c.encode(m, {wire.data(), 1}, scratch, limits);
        tracking = false;
        CHECK(short_output.status == Status::BufferTooSmall);
        CHECK(wire == before);
        CHECK(short_output.produced == 0);
        e = encode(c, m);
        arena.fill(std::byte{0x5a});
        const auto before_arena = arena;
        out.request = 77;
        tracking = true;
        auto short_arena =
            c.decode({wire.data(), e.produced}, {arena.data(), d.required_arena - 1}, stage, out, limits);
        tracking = false;
        CHECK(short_arena.status == Status::BufferTooSmall);
        CHECK(short_arena.consumed == 0);
        CHECK(out.request == 77);
        CHECK(arena == before_arena);
    }
    // Actual T advertised Native scalar capabilities use the same control grammar.
    sc::NumericContract scalar;
    scalar.dtype = sc::DType::UInt64;
    scalar.channel_order = {channels.data(), 1};
    scalar.clock.domain = "exact.clock";
    Port source{0, domain::PortDirection::Output, scalar}, sink{0, domain::PortDirection::Input, scalar};
    std::array<CapabilityRow, 2> rows{CapabilityRow{Node{1, "NetworkInput", EndpointParameters{}, {}, {&source, 1}},
                                                    "source",
                                                    "test",
                                                    sc::ValueDomain::FiniteOnly,
                                                    {}},
                                      CapabilityRow{Node{2, "NetworkOutput", EndpointParameters{}, {&sink, 1}, {}},
                                                    "sink",
                                                    "test",
                                                    sc::ValueDomain::FiniteOnly,
                                                    {}}};
    Reply advertised;
    advertised.command = CommandId::Capabilities;
    advertised.capabilities = Capabilities{4, bdsp_protocol::numeric_data::PayloadVersion, 3, rows, {}};
    Message advertisement;
    advertisement.body = advertised;
    auto adv = encode(c, advertisement);
    CHECK(adv);
    Message caps_out;
    CHECK(decode(c, adv.produced, caps_out));
    CHECK(sc::same_contract(std::get<Reply>(caps_out.body).capabilities->rows[0].combination.outputs[0].contract,
                            scalar));

    // CONTROL Native descriptors never authorize Native DATA bytes.
    std::uint8_t data_id = 123;
    CHECK(!bdsp_protocol::numeric_data::symbol_id(sc::StorageEncoding::Native, data_id));
    CHECK(data_id == 123);
#ifdef NUMERIC_CONTROL_MAPPER_TESTS
    namespace nd = bdsp_protocol::numeric_data;
    std::array<std::byte, 8> payload{};
    std::array<sc::PlaneLayout, 1> planes{sc::PlaneLayout{0, {8, 0}}};
    nd::Message data;
    data.endpoint = {"native-data-refusal", 1, nd::Direction::Output, 0};
    data.envelope = {1, 2, 3, 4, 5, 6, 0};
    data.stamp.stream_id = "stream";
    data.stamp.sequence = 1;
    data.contract = scalar;
    data.numeric.storage = payload;
    data.numeric.planes = planes;
    auto fixed_scalar = scalar;
    fixed_scalar.representation.encoding = sc::StorageEncoding::FixedLittleEndian;
    data.contract = fixed_scalar;
    CHECK(nd::measure(data, nd::desktop_limits()));
    data.contract = scalar;
    CHECK(!nd::measure(data, nd::desktop_limits()));
    arena.fill(std::byte{0x5a});
    const auto data_before = arena;
    tracking = true;
    auto refused = nd::encode(data, arena, stage, nd::desktop_limits());
    tracking = false;
    CHECK(!refused);
    CHECK(refused.produced == 0);
    CHECK(arena == data_before);
#endif
    for (auto encoding : std::array{sc::StorageEncoding::Native, sc::StorageEncoding::FixedLittleEndian,
                                    sc::StorageEncoding::FixedBigEndian, sc::StorageEncoding::PackedSigned24LE,
                                    sc::StorageEncoding::PackedSigned24BE}) {
        sc::NumericContract n;
        n.dtype = sc::DType::Int32;
        n.channel_order = {channels.data(), 1};
        n.representation.encoding = encoding;
        if (encoding == sc::StorageEncoding::PackedSigned24LE || encoding == sc::StorageEncoding::PackedSigned24BE)
            n.representation.valid_bits = 24;
        Port p{0, domain::PortDirection::Output, n};
        Node node{1, "NetworkInput", EndpointParameters{}, {}, {&p, 1}};
        Graph one{"all-control-encodings", {}, {&node, 1}, {}};
        Command cmd;
        cmd.id = CommandId::Prepare;
        cmd.graph = one;
        Message m;
        m.body = cmd;
        auto e = encode(c, m);
        CHECK(e);
        if (!e)
            continue;
        Message out;
        CHECK(decode(c, e.produced, out));
        CHECK(sc::same_contract(std::get<Command>(out.body).graph->nodes[0].outputs[0].contract, n));
        const std::uint8_t cb[]{0xa6, 1, 1, 2, 0, 3, 1, 4, 1, 5, 0, 6, 1};
        const std::uint8_t pb[]{8, 1, 16, 0, 24, 1, 32, 1, 40, 0, 48, 1};
        if (encoding == sc::StorageEncoding::FixedLittleEndian) {
            auto needle =
                c.id() == CodecId::Cbor ? std::span<const std::uint8_t>{cb} : std::span<const std::uint8_t>{pb};
            auto end = wire.begin() + e.produced;
            auto found = std::search(wire.begin(), end, needle.begin(), needle.end());
            CHECK(found != end);
            if (found != end) {
                *(found + (c.id() == CodecId::Cbor ? 2 : 1)) = 5;
                arena.fill(std::byte{0x5a});
                const auto untouched = arena;
                out.request = 91;
                auto bad = decode(c, e.produced, out);
                CHECK(!bad);
                CHECK(bad.consumed == 0);
                CHECK(out.request == 91);
                CHECK(arena == untouched);
            }
        }
    }
}
} // namespace
extern "C" void *__real_malloc(std::size_t);
extern "C" void *__real_calloc(std::size_t, std::size_t);
extern "C" void *__real_realloc(void *, std::size_t);
extern "C" void *__wrap_malloc(std::size_t n) {
    if (tracking)
        ++c_allocations;
    return __real_malloc(n);
}
extern "C" void *__wrap_calloc(std::size_t n, std::size_t w) {
    if (tracking)
        ++c_allocations;
    return __real_calloc(n, w);
}
extern "C" void *__wrap_realloc(void *p, std::size_t n) {
    if (tracking)
        ++c_allocations;
    return __real_realloc(p, n);
}
void *operator new(std::size_t n) {
    if (tracking)
        ++allocations;
    auto *p = std::malloc(n);
    if (!p)
        std::abort();
    return p;
}
void *operator new[](std::size_t n) {
    return ::operator new(n);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}
void diagnostic_reason_roundtrip(const Codec& codec) {
    Reply reply;reply.error=Error::Validation;reply.diagnostic.node=70;reply.diagnostic.parameter="fft_size";
    Message message;
    for(unsigned reason=0;reason<=unsigned(domain::ProcessingReason::ProfileEncoding);++reason){
        reply.diagnostic.reason=domain::ProcessingReason(reason);message.body=reply;
        const auto encoded=encode(codec,message);CHECK(encoded);
        Message restored;CHECK(decode(codec,encoded.produced,restored));
        const auto& copy=std::get<Reply>(restored.body).diagnostic;
        CHECK(copy.reason==reply.diagnostic.reason);CHECK(copy.parameter=="fft_size");CHECK(copy.node==70);
    }
    reply.diagnostic.reason=domain::ProcessingReason(255);message.body=reply;
    CHECK(!encode(codec,message));
}

void graph_boundary_versions(const Codec& codec) {
    Graph graph;graph.graph_id="boundary";
    for(auto member:std::array{&GraphVersions::document,&GraphVersions::graph,&GraphVersions::processing,&GraphVersions::control,&GraphVersions::data}){
        graph.versions={};graph.versions.*member=99;
        Command command;command.id=CommandId::Prepare;command.graph=graph;
        Message message;message.body=command;
        CHECK(encode(codec,message).status==Status::UnsupportedVersion);
#ifdef NUMERIC_CONTROL_MAPPER_TESTS
        domain::NumericProcessingGraph output;output.graph_id="retained";
        CHECK(control::numeric::own_graph(graph,limits,output).status==Status::UnsupportedVersion);
        CHECK(output.graph_id=="retained");
#endif
    }
}

void boundary_versions_tests() {
    for(auto boundary:{bdsp_protocol::ProcessingBoundary::Document,bdsp_protocol::ProcessingBoundary::Control,bdsp_protocol::ProcessingBoundary::Data}){
        const auto current=boundary==bdsp_protocol::ProcessingBoundary::Data?5u:4u;
        CHECK(bdsp_protocol::processing_boundary_version_supported(boundary,current)==signal_contracts::Status::ok);
        for(auto edition:{0u,1u,2u,99u})CHECK(bdsp_protocol::processing_boundary_version_supported(boundary,edition)==signal_contracts::Status::unsupported_version);
        if(boundary==bdsp_protocol::ProcessingBoundary::Data)for(const auto edition:{3u,4u,6u})CHECK(bdsp_protocol::processing_boundary_version_supported(boundary,edition)==signal_contracts::Status::unsupported_version);
    }
    CHECK(bdsp_protocol::processing_boundary_version_supported(static_cast<bdsp_protocol::ProcessingBoundary>(99),2)==signal_contracts::Status::invalid_argument);
}
void native_cases(const Codec& c,const Codec& p) {
    for (const Codec *codec : std::array<const Codec *, 2>{&c, &p}) {
        native_descriptor_regression(*codec);
        basic(*codec);
        literal_goldens(*codec);
        codec_mask_regression(*codec);
    }
}
void full_cases(const Codec& c,const Codec& p) {
    CHECK(negotiate(4, 4, 3, 3) == CodecId::Cbor);
    CHECK(negotiate(4, 4, 2, 3) == CodecId::Protobuf);
    CHECK(!negotiate(4, 3, 3, 3));
    CHECK(!negotiate(4, 4, 1, 2));
    for (const Codec *codec : std::array<const Codec *, 2>{&c, &p}) {
        basic(*codec);
        preflight_regression(*codec);
        contracts_and_literals(*codec);graph_boundary_versions(*codec);diagnostic_reason_roundtrip(*codec);
        messages(*codec);binding_regression(*codec);interval_regression(*codec);single_stream_parameters(*codec);
        alternatives_and_limits(*codec);
        literal_goldens(*codec);
        codec_mask_regression(*codec);
        native_descriptor_regression(*codec);
    }
}
int report(const char* label) {
    CHECK(allocations == 0);
    CHECK(c_allocations == 0);
    std::printf("%s checks=%u failures=%u allocations C++=%zu C=%zu\n",
                label,checks,failures,allocations,c_allocations);
    return failures ? 1 : 0;
}
int main(int argc, char **argv) {
    boundary_versions_tests();
    CborCodec c;
    ProtobufCodec p;
    if (argc == 2 && std::strcmp(argv[1], "--native") == 0) {
        native_cases(c,p);
        return report("native-descriptor");
    }
    if (argc == 2 && std::strcmp(argv[1], "--codec-mask") == 0) {
        codec_mask_regression(c);
        codec_mask_regression(p);
        return report("codec-mask");
    }
    full_cases(c,p);
    report("numeric control");
    std::printf(
        "sizeof Node=%zu Port=%zu Parameter=%zu CapabilityRow=%zu; device arena+stage=%zu desktop arena+stage=%zu\n",
        sizeof(Node), sizeof(Port), sizeof(Parameter), sizeof(CapabilityRow), 2 * device_limits().arena_bytes,
        2 * desktop_limits().arena_bytes);
    return failures ? 1 : 0;
}
