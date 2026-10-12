#pragma once
#include <bdsp_protocol/numeric_control_limits.h>
#include <bdsp_protocol/numeric_data_message.h>
#include <domain/processing_contracts.h>
#include <optional>
namespace bdsp_protocol::numeric_control {
inline constexpr std::uint32_t SchemaVersion = 4;
// CONTROL describes processing storage, not payload bytes. Zero is the Native
// descriptor symbol; fixed encodings1..4 retain the shared DATA symbol IDs.
inline constexpr std::uint8_t NativeDescriptorEncoding = 0;
enum class CodecId { Cbor, Protobuf };
inline constexpr std::uint32_t CborMask = 1, ProtobufMask = 2;
std::optional<CodecId> negotiate(std::uint32_t local_version, std::uint32_t peer_version, std::uint32_t local_mask,
                                 std::uint32_t peer_mask) noexcept;
using Literal = signal_contracts::NumericLiteral;
using ParameterValue = std::variant<Literal, bool, std::string_view>;
struct Parameter {
    std::string_view key;
    ParameterValue value = Literal{std::int64_t{0}};
};
struct EndpointParameters {
    std::span<const Parameter> values;
};
struct TransformParameters {
    std::span<const Parameter> values;
};
struct ReductionParameters {
    domain::NumericReduction operation = domain::NumericReduction::MaxAbs;
    domain::ReductionChannels channels = domain::ReductionChannels::AllChannels;
    domain::ReductionRows rows = domain::ReductionRows::WholeValue;
    std::span<const std::uint32_t> selected_channels;
    signal_contracts::PrecisionLoss precision_loss = signal_contracts::PrecisionLoss::RejectInexact;
    signal_contracts::Rounding rounding = signal_contracts::Rounding::NearestTiesEven;
};
struct GeneratorParameters { std::span<const signal_contracts::OscillatorParameters> oscillators; };
struct ClockParameters { std::uint32_t sample_rate=48000; std::string_view domain="local"; };
struct MixerParameters { domain::MixerMode mode=domain::MixerMode::WeightedSum; std::span<const domain::MixerInputWeight> weights; std::uint32_t fifo_milliseconds=100; domain::MixerConsumption consumption=domain::MixerConsumption::ClockedFifo; };
using Parameters =
    std::variant<EndpointParameters, TransformParameters, domain::ConverterProcessingParameters,
                 domain::RouteProcessingParameters, ReductionParameters, domain::HistoryProcessingParameters, GeneratorParameters, ClockParameters, MixerParameters>;
struct Port {
    domain::PortId id = 0;
    domain::PortDirection direction = domain::PortDirection::Input;
    signal_contracts::DataContract contract;
};
struct Node {
    domain::NodeId id = -1;
    std::string_view kind;
    Parameters parameters;
    std::span<const Port> inputs, outputs;
    bool enabled = true;
};
struct Graph {
    std::string_view graph_id;
    domain::ProcessingVersions versions;
    std::span<const Node> nodes;
    std::span<const domain::Edge> edges;
    signal_contracts::Ratio execution_interval{1,120};
    std::span<const domain::ConfigurationBinding> configuration_bindings;
};
enum class CommandId { Capabilities, State, Prepare, Apply, SetParameters, Start, Stop, Persist, Release };
struct Command {
    CommandId id = CommandId::State;
    std::optional<Graph> graph;
    std::optional<Parameters> parameters;
    std::string_view graph_id;
    std::uint64_t token = 0, graph_revision = 0;
    std::int32_t node = -1;
    bool enabled = true;
};
struct Resources {
    std::uint64_t plan = 0, payload = 0, scratch = 0, stack = 0, peak = 0;
};
// Each row is one actual configured combination, never a Cartesian inventory.
struct CapabilityRow {
    Node combination;
    std::string_view kernel, backend;
    signal_contracts::ValueDomain values = signal_contracts::ValueDomain::FiniteOnly;
    Resources resources;
};
struct Capabilities {
    std::uint32_t control_version = 4, data_version = 5, codec_mask = 3;
    std::span<const CapabilityRow> rows;
    Resources maximum;
};
enum class Error { None, Conflict, Validation, Unsupported, Limits, Busy, Expired, Foreign, Internal };
enum class Lifecycle { Empty, Prepared, Stopped, Running };
struct Diagnostic {
    std::optional<std::int32_t> node;
    std::optional<std::uint16_t> port;
    std::optional<std::uint64_t> edge;
    std::string_view parameter;
};
struct State {
    Lifecycle lifecycle = Lifecycle::Empty;
    bool run_intent = false;
    std::uint64_t revision = 0, token = 0;
    std::optional<Graph> applied;
    Resources resources;
};
struct Reply {
    CommandId command = CommandId::State;
    Error error = Error::None;
    Diagnostic diagnostic;
    State state;
    std::optional<Capabilities> capabilities;
};
// Explicit extension namespace only; tags1024..32767. High mandatory bit refuses.
struct Extension {
    std::uint32_t tag = 0;
    std::span<const std::uint8_t> bytes;
};
struct Message {
    std::uint32_t schema_version = SchemaVersion;
    std::uint64_t client = 0, session = 0, epoch = 0, request = 0, expected_revision = 0;
    std::variant<Command, Reply> body = Command{};
    std::span<const Extension> extensions;
};
// All variable fields borrowed; decoded views refer only to caller destination arena.
// Deep construction of domain::NumericProcessingGraph is an allocating cold adapter
// responsibility outside this codec, followed by the canonical domain resolver/admission.
// Structural transport validation only. The canonical domain resolver and actual
// capability admission remain mandatory in the cold adapter before preparation.
// Success also reports canonical decode arena and aggregate text footprints.
Result validate(const Message &, const Limits &) noexcept;
} // namespace bdsp_protocol::numeric_control
