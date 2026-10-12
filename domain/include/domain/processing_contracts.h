#pragma once
#include <domain/graph_definition.h>
#include <signal_contracts/oscillator.h>
#include <signal_contracts/numeric_policy.h>

namespace domain {
using PortId = std::uint16_t;
// Existing endpoints retain their numeric identity. Names are descriptor metadata.
inline constexpr PortId processing_input = 0, processing_output = 0,
    history_output = 0, filled_count_output = 1;

// Immutable metadata snapshot. Copies share ownership; view() rebases borrowed fields.
// No payload, actual coordinates, DSP state or presentation fields belong here.
class OwnedProcessingContract {
public:
    OwnedProcessingContract() noexcept = default;
    OwnedProcessingContract(const OwnedProcessingContract&) noexcept;
    OwnedProcessingContract& operator=(const OwnedProcessingContract&) noexcept;
    OwnedProcessingContract(OwnedProcessingContract&&) noexcept;
    OwnedProcessingContract& operator=(OwnedProcessingContract&&) noexcept;
    ~OwnedProcessingContract();
    explicit operator bool() const noexcept { return storage_ != nullptr; }
    signal_contracts::DataContract view() const noexcept;
    std::size_t owned_bytes() const noexcept;
private:
    struct Storage;
    Storage* storage_ = nullptr;
    void release() noexcept;
    friend signal_contracts::Status own_processing_contract(const signal_contracts::DataContract&,
        const signal_contracts::FrameLimits&, std::size_t, OwnedProcessingContract&) noexcept;
};
// Preflight candidate + retained out allocation ledger, then nothrow allocation.
// Refusal leaves out intact. Other caller-retained snapshots are charged by their caller.
signal_contracts::Status own_processing_contract(const signal_contracts::DataContract&,
    const signal_contracts::FrameLimits&, std::size_t max_owned_bytes, OwnedProcessingContract&) noexcept;

struct ProcessingPort {
    PortId id = 0;
    PortDirection direction = PortDirection::Input;
    OwnedProcessingContract contract;
};
using ProcessingParameterValue = std::variant<signal_contracts::NumericLiteral, bool, std::string>;
struct ProcessingParameter { std::string key; ProcessingParameterValue value; };
struct EndpointProcessingParameters { std::vector<ProcessingParameter> values; };
struct TransformProcessingParameters { std::vector<ProcessingParameter> values; };
struct ConverterProcessingParameters { signal_contracts::ConversionPolicy policy; };
struct RouteProcessingParameters { std::uint16_t outputs = 2, selected = 0; bool open = true; std::uint16_t channels_per_stream = 1; };
enum class NumericReduction { MaxAbs, SignedMean, RMS };
enum class ReductionChannels { AllChannels, BothChannels, SelectedChannels };
enum class ReductionRows { WholeValue, LatestHistoryRow };
struct ReduceProcessingParameters {
    NumericReduction operation = NumericReduction::MaxAbs;
    ReductionChannels channels = ReductionChannels::AllChannels;
    ReductionRows rows = ReductionRows::WholeValue;
    std::vector<std::uint32_t> selected_channels;
    signal_contracts::PrecisionLoss precision_loss = signal_contracts::PrecisionLoss::RejectInexact;
    signal_contracts::Rounding rounding = signal_contracts::Rounding::NearestTiesEven;
};
struct HistoryProcessingParameters { std::uint64_t capacity = 0; };
struct SignalGeneratorProcessingParameters { std::vector<signal_contracts::OscillatorParameters> oscillators{{}}; bool operator==(const SignalGeneratorProcessingParameters&) const = default; };
struct SampleClockProcessingParameters { std::uint32_t sample_rate=48000; std::string domain="local"; bool operator==(const SampleClockProcessingParameters&) const = default; };
enum class MixerMode { WeightedSum, WeightedMean, WeightedRMS, MaxAbsolute };
enum class MixerConsumption { ClockedFifo, SourceAligned };
struct MixerInputWeight { PortId port=0; double weight=.5; bool operator==(const MixerInputWeight&) const = default; };
struct MixerProcessingParameters { MixerMode mode=MixerMode::WeightedSum; std::vector<MixerInputWeight> weights{{0,.5},{1,.5}}; std::uint32_t fifo_milliseconds=100; MixerConsumption consumption=MixerConsumption::ClockedFifo; bool operator==(const MixerProcessingParameters&) const = default; };
using ProcessingParameters = std::variant<EndpointProcessingParameters, TransformProcessingParameters,
    ConverterProcessingParameters, RouteProcessingParameters, ReduceProcessingParameters, HistoryProcessingParameters, SignalGeneratorProcessingParameters, SampleClockProcessingParameters, MixerProcessingParameters>;
struct ConfiguredProcessingNode {
    NodeId id = -1;
    std::string kind; // existing kind_id; explicit new "Converter"
    ProcessingParameters parameters = EndpointProcessingParameters{};
    std::vector<ProcessingPort> inputs, outputs;
    bool enabled = true;
    NodeId clock_node=-1; // Configuration dependency, never a numeric edge.
};
enum class ProcessingOperation { Source, Sink, Transform, Converter, Splitter, Switch, Gate, Reduce, History, WLEDEncoder, InputSelector, Extract, Mixer, Clock, ChannelComparison };
struct ResolvedProcessingNode {
    NodeId id = -1;
    std::string_view kind;
    NodeRole role = NodeRole::Unknown;
    ProcessingOperation operation = ProcessingOperation::Transform;
    std::span<const ProcessingPort> inputs, outputs;
    const ProcessingParameters* parameters = nullptr;
    bool enabled = true;
};
struct ProcessingVersions {
    std::uint32_t document = 4, graph = 5, processing = 4, control = 4, data = 5;
};
enum class ConfigurationRole : std::uint16_t { SampleClock = 0, FrequencyGrid = 1 };
struct ConfigurationBinding {
    NodeId from=-1, to=-1;
    ConfigurationRole output=ConfigurationRole::SampleClock, input=ConfigurationRole::SampleClock;
    bool operator==(const ConfigurationBinding&) const = default;
};
struct NumericProcessingGraph {
    std::string graph_id;
    ProcessingVersions versions;
    std::vector<ConfiguredProcessingNode> nodes;
    std::vector<Edge> edges;
    // Analysis deadlines in physical signal time, independent of presentation.
    signal_contracts::Ratio execution_interval{1,120};
    std::vector<ConfigurationBinding> configuration_bindings;
};
bool valid_execution_interval(signal_contracts::Ratio) noexcept;
struct ProcessingAdmissionLimits {
    std::size_t max_nodes, max_edges, max_ports_per_node, max_parameters_per_node,
        max_text_bytes, max_owned_metadata_bytes;
    signal_contracts::FrameLimits frame;
};
enum class ProcessingError { None, UnsupportedVersion, Limit, InvalidNode, UnknownKind,
    InvalidParameters, InvalidPort, InvalidContract, IncompatiblePorts, MultipleInputs,
    MissingEndpoint, Cycle, UnsupportedCombination, ResourceLimit, InsufficientWorkspace };
struct ProcessingDiagnostic {
    ProcessingError error = ProcessingError::None;
    NodeId node = -1;
    PortId port = 0;
    std::size_t edge = 0;
    explicit operator bool() const noexcept { return error == ProcessingError::None; }
};
// Canonical graph/processing editions only; external fields do not gate local admission.
signal_contracts::Status processing_versions_supported(const ProcessingVersions&) noexcept;
enum class ProcessingBoundary { Document, Control, Data };
// Adapter membership check BEFORE decoding/negotiation/delivery, never a local graph gate.
// An old/future edition requires its own explicit tested adapter or refusal.
signal_contracts::Status processing_boundary_version_supported(ProcessingBoundary,std::uint32_t) noexcept;
// One resolver for domain, runtime and editor adapters; publishes only on success.
ProcessingDiagnostic resolve_processing_node(const ConfiguredProcessingNode&,
    const ProcessingAdmissionLimits&, ResolvedProcessingNode&) noexcept;
bool same_processing_parameters(const ProcessingParameters&, const ProcessingParameters&) noexcept;
bool processing_ports_compatible(const ProcessingPort& output, const ProcessingPort& input) noexcept;
bool processing_ports_dtype_convertible(const ProcessingPort&,const ProcessingPort&) noexcept;
bool dtype_conversion_is_exact(signal_contracts::DType,signal_contracts::DType) noexcept;
signal_contracts::ConversionPolicy edge_conversion_policy(std::uint8_t) noexcept;
// Prepare-time lowering into canonical converters; original identities stay intact.
ProcessingDiagnostic lower_dtype_edges(const NumericProcessingGraph&,const ProcessingAdmissionLimits&,NumericProcessingGraph&);
ProcessingDiagnostic resolve_configuration_bindings(const NumericProcessingGraph&,const ProcessingAdmissionLimits&,NumericProcessingGraph&);
bool configuration_output_supported(const ConfiguredProcessingNode&,ConfigurationRole) noexcept;
bool configuration_input_supported(const ConfiguredProcessingNode&,ConfigurationRole) noexcept;
bool has_configuration_binding(const NumericProcessingGraph&,NodeId,ConfigurationRole) noexcept;
using ProcessingParameterView = std::variant<signal_contracts::NumericLiteral, bool, std::string_view>;
// Registered processing defaults are resolved here without rewriting persisted explicit values.
signal_contracts::Status resolve_processing_parameter(const ResolvedProcessingNode&,
    std::string_view key, ProcessingParameterView&) noexcept;

struct ProcessingResources { std::size_t plan_bytes = 0, payload_bytes = 0, scratch_bytes = 0, stack_bytes = 0; };
struct ProcessingResourceProfile { ProcessingResources maximum; std::size_t retained_working_bytes = 0, max_peak_bytes = 0; };
struct ProcessingSupport { bool supported = false; ProcessingResources resources; };
// Pure capability port: adapter queries implemented combinations, including active policies.
// No individual dtype/NodeKind inventory or Cartesian product authorizes a node.
struct NumericDeviceCapabilities {
    using Query = ProcessingSupport (*)(const void*, const ResolvedProcessingNode&,
        const ProcessingResourceProfile&) noexcept;
    const void* context = nullptr;
    Query query = nullptr;
    ProcessingResourceProfile profile;
};
struct ProcessingGraphAdmission { std::size_t node_count = 0; ProcessingResources resources; std::size_t peak_bytes = 0; };
// Same full owning DTO footprint used by graph admission; no topology/kernel work.
signal_contracts::Status processing_graph_owned_bytes(const NumericProcessingGraph&,const ProcessingAdmissionLimits&,std::size_t&) noexcept;
// Workspace >= 2*node count, disjoint from inputs/out; scratch may change on refusal.
// Admission/out unchanged on error. Input snapshot is immutable throughout the call.
// No working graph, token or executor is accessed. Missing input is a legal disconnected draft.
ProcessingDiagnostic admit_processing_graph(const NumericProcessingGraph&, const ProcessingAdmissionLimits&,
    const NumericDeviceCapabilities&, std::span<std::size_t> workspace, ProcessingGraphAdmission&) noexcept;
} // namespace domain
