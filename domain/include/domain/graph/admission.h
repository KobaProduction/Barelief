#pragma once
#include <domain/graph/processing_model.h>

namespace domain {
struct PreparationValueProvider;
struct ProcessingAdmissionLimits {
    std::size_t max_nodes, max_edges, max_ports_per_node, max_parameters_per_node,
        max_text_bytes, max_owned_metadata_bytes;
    signal_contracts::FrameLimits frame;
};
enum class ProcessingError { None, UnsupportedVersion, Limit, InvalidNode, UnknownKind,
    InvalidParameters, InvalidPort, InvalidContract, IncompatiblePorts, MultipleInputs,
    MissingEndpoint, Cycle, UnsupportedCombination, ResourceLimit, InsufficientWorkspace, PreparationValueUnavailable, PreparationTypeMismatch, FrontendMismatch, PreparationValueInvalid, PreparationValueOutOfRange };
enum class ProcessingReason : std::uint8_t {
    None=0, FftSize=1, SampleRate=2, SpectrumLayout=3, SpectrumRepresentation=4,
    WindowFunction=5, CoherentGain=6, PowerGain=7, Frontend=8, Resampler=9,
    GainCount=10, GainValue=11, ProfileEncoding=12
};

struct ProcessingDiagnostic {
    ProcessingError error = ProcessingError::None;
    NodeId node = -1;
    PortId port = 0;
    std::optional<std::size_t> edge;
    ProcessingReason reason=ProcessingReason::None;
    // Local producers use immutable static parameter keys; asynchronous adapters copy them.
    std::string_view parameter;
    explicit operator bool() const noexcept { return error == ProcessingError::None; }
};
struct ProcessingResources { std::size_t plan_bytes = 0, payload_bytes = 0, scratch_bytes = 0, stack_bytes = 0; };
struct ProcessingResourceProfile { ProcessingResources maximum; std::size_t retained_working_bytes = 0, max_peak_bytes = 0; };
struct ProcessingSupport { bool supported = false; ProcessingResources resources; };
// Pure capability port: adapter queries implemented combinations, including active policies.
// No individual dtype/NodeKind inventory or Cartesian product authorizes a node.
struct ProcessingGeometryLimits {
    std::uint32_t min_fft_size=0,max_fft_size=0;
    std::uint32_t min_sample_rate=0,max_sample_rate=0;
};
struct NumericDeviceCapabilities {
    using Query = ProcessingSupport (*)(const void*, const ResolvedProcessingNode&,
        const ProcessingResourceProfile&) noexcept;
    const void* context = nullptr;
    Query query = nullptr;
    ProcessingResourceProfile profile;
    using GeometryQuery=ProcessingGeometryLimits (*)(const void*,std::string_view) noexcept;
    GeometryQuery geometry=nullptr;
};
struct ProcessingGraphAdmission { std::size_t node_count = 0; ProcessingResources resources; std::size_t peak_bytes = 0; };
// Canonical graph/processing editions only; external fields do not gate local admission.
signal_contracts::Status processing_versions_supported(const ProcessingVersions&) noexcept;
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
struct RoutingAlias {
    NodeId source=-1,node=-1; PortId source_port=0,port=0;
    PortDirection direction=PortDirection::Output;bool include_input=true;NodeKind kind=NodeKind::Reroute;
};
// After dtype conversion lowering: remove identity routing points from execution,
// retaining endpoint aliases for observation. Output arguments publish atomically.
ProcessingDiagnostic lower_routing_points(const NumericProcessingGraph&,const ProcessingAdmissionLimits&,
    NumericProcessingGraph&,std::vector<RoutingAlias>&);
// Compile explicitly declared auxiliary values into literal producers with endpoint aliases.
// No produced frame metadata or synthetic stream edge is read to compute these values.
ProcessingDiagnostic lower_numeric_metadata_outputs(const NumericProcessingGraph&,const ProcessingAdmissionLimits&,
    NumericProcessingGraph&,std::vector<RoutingAlias>&,const PreparationValueProvider* provider=nullptr);
ProcessingDiagnostic resolve_configuration_bindings(const NumericProcessingGraph&,const ProcessingAdmissionLimits&,NumericProcessingGraph&);
bool configuration_output_supported(const ConfiguredProcessingNode&,ConfigurationRole) noexcept;
bool configuration_input_supported(const ConfiguredProcessingNode&,ConfigurationRole) noexcept;
bool has_configuration_binding(const NumericProcessingGraph&,NodeId,ConfigurationRole) noexcept;


// Same full owning DTO footprint used by graph admission; no topology/kernel work.
signal_contracts::Status processing_graph_owned_bytes(const NumericProcessingGraph&,const ProcessingAdmissionLimits&,std::size_t&) noexcept;
// Workspace >= 2*node count, disjoint from inputs/out; scratch may change on refusal.
// Admission/out unchanged on error. Input snapshot is immutable throughout the call.
// No working graph, token or executor is accessed. Missing input is a legal disconnected draft.
ProcessingDiagnostic admit_processing_graph(const NumericProcessingGraph&, const ProcessingAdmissionLimits&,
    const NumericDeviceCapabilities&, std::span<std::size_t> workspace, ProcessingGraphAdmission&) noexcept;
}
