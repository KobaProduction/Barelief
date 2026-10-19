#pragma once
#include <signal_contracts/frame.h>
#include <signal_contracts/window_policy.h>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace domain {
using FrameType = signal_contracts::FrameType;
using NodeId = std::int32_t; // Existing document IDs are preserved, never regenerated.
enum class NodeKind {
    AudioSource, StereoFFT, SpectrumMerge, SpectrumGain, Mel, Spectrogram,
    FFTView, MelView, SpectrogramView, AudioView, WLED, TestSignal, Window,
    WindowFunction, BarkWeighting, WindowView, WindowedView, NetworkInput, NetworkOutput,
    Splitter, Switch, Gate, ScalarReduce, ScalarView, WLEDEncode, WLEDOutput, InputSelector, PcmResampler, IndexExtract, SignalGenerator, SampleClock, Mixer, ChannelComparison, MatrixTranspose, MatrixWindowFunction, MatrixFFT, MatrixRow, MatrixCenter, RFFT, SpectrumProjection, FrequencyNormalizer, BroadbandFeatures, NoteSalience, SpectralQuantize, Fourier, RealFourier, ComplexPolar, Reroute, Fs, UInt32Constant, ArrayLength, SampleRate, SpectrumAxis, SpectrumAmplitude, AmplitudePhase, WindowGain, Number, ArrayFill, ArrayMultiply, MusicalSpectrogram, FrequencyStep, BroadbandRow, NoteSpectrum, ArrayConcat, PianoView, SpectrumPeak,
    ScalarMetricsView, ScalarSignedBarView,
    VectorLineView, VectorPointView, VectorBarView, VectorPieView,
    MatrixImageView, MatrixSurfaceView,
    AudioLineView, AudioPointView, AudioBarView, AudioPieView,
    AudioImageView, AudioSurfaceView,
    NumberAccumulator
};
enum class NodeRole { Source, Processor, Sink, Unknown };
NodeRole node_role(NodeKind) noexcept;
bool is_view(NodeKind) noexcept;
bool is_scalar_view(NodeKind) noexcept;
// Registered kinds; retired enum slots are not reused.
constexpr std::size_t node_kind_count=80;
std::string_view kind_id(NodeKind kind) noexcept;
std::optional<NodeKind> node_kind(std::string_view canonical_id) noexcept;
enum class AudioSource { SystemOutput = 0, Microphone = 1 };
enum class MergeMode { Mean = 0, Maximum = 1, RMS = 2 };
enum class Channel { Left = 0, Right = 1, Merged = 2 };
using WindowFunction=signal_contracts::WindowFunction;
struct NoParameters { bool operator==(const NoParameters&) const = default; };
struct AudioSourceParameters { AudioSource source = AudioSource::SystemOutput; bool operator==(const AudioSourceParameters&) const = default; };
struct MergeParameters { MergeMode merge = MergeMode::RMS; bool operator==(const MergeParameters&) const = default; };
struct GainParameters { double gain = 1; bool operator==(const GainParameters&) const = default; };
struct MelParameters { std::uint32_t bands = 50; bool operator==(const MelParameters&) const = default; };
struct HistoryParameters { std::uint32_t history_size = 400; bool operator==(const HistoryParameters&) const = default; };
struct FFTViewParameters { Channel channel = Channel::Left; MergeMode merge = MergeMode::RMS; bool operator==(const FFTViewParameters&) const = default; };
struct WindowParameters { std::uint32_t size = 4096; bool operator==(const WindowParameters&) const = default; };
struct WindowFunctionParameters { WindowFunction function = WindowFunction::Hann; bool operator==(const WindowFunctionParameters&) const = default; };
struct TestSignalParameters {
    std::uint32_t tones = 8, sample_rate = 48000;
    double minimum_hz = 73, maximum_hz = 13901, amplitude = .4;
    double period_seconds = 6, sweep_depth = .18;
    bool operator==(const TestSignalParameters&) const = default;
};
struct NetworkParameters {
    FrameType data_type = FrameType::Mel;
    std::string provider_uuid, stream_id, host;
    std::uint32_t port = 8214;
    bool operator==(const NetworkParameters&) const = default;
};
// Routing forwards the exact semantic type, including history ownership.
// Switch selects an output direction, not an arbitrary merge of inputs.
struct RoutingParameters {
    FrameType data_type=FrameType::Mel;
    std::uint16_t outputs=2, selected=0;
    bool operator==(const RoutingParameters&) const = default;
};
enum class Reduction { Maximum, Mean, RMS };
struct ReductionParameters {
    FrameType data_type=FrameType::Mel;
    Reduction operation=Reduction::Maximum;
    bool operator==(const ReductionParameters&) const = default;
};
using NodeParameters = std::variant<NoParameters, AudioSourceParameters, MergeParameters,
    GainParameters, MelParameters, HistoryParameters, FFTViewParameters, WindowParameters,
    WindowFunctionParameters, TestSignalParameters, NetworkParameters, RoutingParameters, ReductionParameters>;
NodeParameters default_parameters(NodeKind kind);
struct NodeDefinition {
    NodeId id = -1;
    NodeKind kind = NodeKind::AudioSource;
    NodeParameters parameters = AudioSourceParameters{};
    bool enabled = true;
    bool operator==(const NodeDefinition&) const = default;
};
struct Edge {
    NodeId from = -1, to = -1;
    std::uint16_t output_port = 0, input_port = 0;
    // Dtype-only delivery: 0 exact/reject, 1 nearest/reject overflow,
    // 2 toward-zero/reject overflow, 3 nearest/saturate. No wrap or scaling.
    std::uint8_t conversion = 0;
    bool operator==(const Edge&) const = default;
};
// Drafts may be invalid. Only validate_graph can authorize preparation.
// JSON extensions and layout belong to the document adapter, not this model.
struct GraphDefinition {
    std::string graph_id = "main";
    std::vector<NodeDefinition> nodes;
    std::vector<Edge> edges;
    std::uint32_t processing_version=1;
    bool operator==(const GraphDefinition&) const = default;
};
std::uint16_t input_ports(const NodeDefinition&) noexcept;
std::uint16_t output_ports(const NodeDefinition&) noexcept;
std::optional<FrameType> input_type(const NodeDefinition&, std::uint16_t port=0) noexcept;
std::optional<FrameType> output_type(const NodeDefinition&, std::uint16_t port=0) noexcept;
enum class PortDirection { Input, Output };
struct PortIdentity {
    NodeId node=-1; std::uint16_t port=0;
    bool operator==(const PortIdentity&) const = default;
};
struct PortDescriptor {
    PortIdentity identity;
    PortDirection direction=PortDirection::Input;
    FrameType type=FrameType::PCM;
    signal_contracts::Quantity quantity=signal_contracts::Quantity::Unspecified;
    signal_contracts::Unit unit=signal_contracts::Unit::Unspecified;
};
enum class ParameterClassification { Processing, Presentation, ApplicationAdapter, OpaqueExtension };
enum class ParameterValueKind { Boolean, Integer, Number, String, FrameType, WindowFunction, NumericArray };
struct ConfiguredProcessingNode;
struct NumericParameterBounds {double minimum=0,maximum=0;};
struct ParameterDescriptor {
    std::string_view key;
    ParameterClassification classification=ParameterClassification::Processing;
    ParameterValueKind value_kind=ParameterValueKind::Number;
    double minimum=0, maximum=0, default_number=0;
    std::string_view default_text;
    std::span<const float> default_array{};
    using NumericBoundsQuery=std::optional<NumericParameterBounds> (*)(const ConfiguredProcessingNode&) noexcept;
    NumericBoundsQuery numeric_bounds=nullptr;
};
struct NodeDescriptor {
    NodeKind kind; std::string_view id; NodeRole role;
    bool view=false;
};
std::span<const NodeDescriptor> node_descriptors() noexcept;
const NodeDescriptor* node_descriptor(NodeKind) noexcept;
std::span<const ParameterDescriptor> parameter_descriptors(NodeKind) noexcept;
const ParameterDescriptor* parameter_descriptor(NodeKind,std::string_view key) noexcept;
ParameterClassification parameter_classification(NodeKind,std::string_view key) noexcept;
bool parameters_valid(const NodeDefinition&) noexcept;
struct ParameterValidationResult {
    bool valid=false;
    std::string_view parameter_key; // static registered key, process lifetime
};
ParameterValidationResult validate_parameters(const NodeDefinition&) noexcept;
ParameterValidationResult validate_parameters(NodeKind,const NodeParameters&) noexcept;
NodeDefinition project_processing_node(const NodeDefinition&);
struct ConfiguredNodeDescriptor {
    NodeId node=-1; NodeKind kind=NodeKind::AudioSource; NodeRole role=NodeRole::Source;
    std::span<const PortDescriptor> inputs,outputs;
    std::size_t required_inputs=0,required_outputs=0;
};
// Caller storage owns spans; failure publishes no spans and never writes storage.
// Describes structurally valid ports even for an incomplete endpoint draft;
// parameters_valid/validate_graph independently authorize preparation.
signal_contracts::Status describe_node(const NodeDefinition&,std::span<PortDescriptor> inputs,
    std::span<PortDescriptor> outputs,ConfiguredNodeDescriptor&) noexcept;
}
