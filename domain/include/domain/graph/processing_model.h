#pragma once
#include <domain/graph_definition.h>
#include <domain/numeric/contract_snapshot.h>
#include <domain/numeric/values.h>
#include <signal_contracts/oscillator.h>
#include <cstddef>

namespace domain {
struct ProcessingAdmissionLimits;
using PortId = std::uint16_t;
inline constexpr std::uint16_t min_route_outputs=2,max_route_outputs=8,max_route_channels=32;
inline constexpr std::size_t max_variadic_inputs=128;
inline constexpr std::uint32_t min_sample_clock_rate=8000,max_sample_clock_rate=192000;
inline constexpr std::uint32_t min_mixer_fifo_ms=1,max_mixer_fifo_ms=1000;
// Existing endpoints retain their numeric identity. Names are descriptor metadata.
inline constexpr PortId processing_input = 0, processing_output = 0,
    history_output = 0, filled_count_output = 1;
inline constexpr PortId note_first_input=65525, note_first_output=3;
inline constexpr PortId frequency_step_input=65526;
inline constexpr PortId power_gain_output=65527, power_gain_input=65528;
inline constexpr PortId coherent_gain_output=65529, coherent_gain_input=65530, input_sample_rate_input=65531, target_sample_rate_input=65532, window_size_input=65533, sample_rate_output=65534;
bool has_sample_rate_output(std::string_view kind) noexcept;
inline constexpr PortId spectrum_first_bin_input=65524;
inline constexpr PortId wled_peak_hz_input=1, wled_magnitude_input=2;

struct ProcessingPort;
struct ConfiguredProcessingNode;
std::span<const ProcessingPort> data_inputs(const ConfiguredProcessingNode&) noexcept;
std::span<const ProcessingPort> data_outputs(const ConfiguredProcessingNode&) noexcept;
std::string_view numeric_parameter_key(std::string_view kind,PortId input) noexcept;

struct ProcessingPort {
    PortId id = 0;
    PortDirection direction = PortDirection::Input;
    OwnedProcessingContract contract;
};
struct ProcessingParameter { std::string key; ProcessingParameterValue value; };
struct EndpointProcessingParameters { std::vector<ProcessingParameter> values; };
struct TransformProcessingParameters { std::vector<ProcessingParameter> values; };
struct ConverterProcessingParameters { signal_contracts::ConversionPolicy policy; };
struct RouteProcessingParameters { std::uint16_t outputs = min_route_outputs, selected = 0; bool open = true; std::uint16_t channels_per_stream = 1; };
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
constexpr bool mixer_requires_nonnegative_weights(MixerMode mode) noexcept {
    return mode==MixerMode::WeightedMean || mode==MixerMode::WeightedRMS;
}
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
enum class ProcessingOperation { Source, Sink, Transform, Converter, Splitter, Switch, Gate, Reduce, History, WLEDEncoder, InputSelector, Extract, Mixer, Clock, ChannelComparison, MatrixTranspose, MatrixWindowFunction, MatrixFFT, MatrixRow, MatrixCenter, MusicTransform, Fourier, ComplexPolar, Metadata, Axis, ArrayFill, ArrayProduct, ArrayConcat, SpectrumPeak };
constexpr bool numeric_constant_kind(std::string_view kind) noexcept {return kind=="Fs" || kind=="UInt32Constant" || kind=="Number";}
constexpr bool paired_processing(ProcessingOperation op) noexcept {return op==ProcessingOperation::Fourier || op==ProcessingOperation::ComplexPolar || op==ProcessingOperation::ArrayProduct || op==ProcessingOperation::ArrayConcat;}
constexpr bool complex_processing(ProcessingOperation op) noexcept {return op==ProcessingOperation::Fourier || op==ProcessingOperation::ComplexPolar;}
struct ResolvedProcessingNode {
    NodeId id = -1;
    std::string_view kind;
    NodeRole role = NodeRole::Unknown;
    ProcessingOperation operation = ProcessingOperation::Transform;
    std::span<const ProcessingPort> inputs, outputs;
    const ProcessingParameters* parameters = nullptr;
    bool enabled = true;
};
// Cold state-continuity provenance, never a source of computational parameters.
// PreserveInput operations forward their declared input contributions; partition
// operations also contribute their own output-content boundary.
enum class ProcessingContentContinuity { PreserveInput, PartitionOutput };
ProcessingContentContinuity processing_output_continuity(const ResolvedProcessingNode&,PortId output) noexcept;
// Local declared semantics only. Physical contract compatibility is checked
// separately; graph topology and produced frame metadata are never inspected.
bool same_processing_output_content(const ResolvedProcessingNode& current,PortId current_output,
    const ResolvedProcessingNode& prior,PortId prior_output) noexcept;
// Registered numeric bounds, narrowed by the node's declared geometry when applicable.
// Unavailable/empty/invalid geometry has no contextual range.
std::optional<NumericParameterBounds> processing_parameter_numeric_bounds(const ConfiguredProcessingNode&,std::string_view) noexcept;
// A generator owns its declared output clock in the saved source configuration.
// This is read before execution; no produced/foreign frame or ancestry is inspected.
std::optional<signal_contracts::ClockContract> signal_generator_clock(const ConfiguredProcessingNode&) noexcept;
struct ProcessingVersions {
    std::uint32_t graph=5, processing=4;
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
signal_contracts::Status attach_numeric_parameter_ports(ConfiguredProcessingNode&,
    const ProcessingAdmissionLimits&) noexcept;
using ProcessingParameterView = std::variant<signal_contracts::NumericLiteral, bool, std::string_view, std::span<const float>>;
// Registered processing defaults are resolved here without rewriting persisted explicit values.
signal_contracts::Status resolve_processing_parameter(const ResolvedProcessingNode&,
    std::string_view key, ProcessingParameterView&) noexcept;
}
