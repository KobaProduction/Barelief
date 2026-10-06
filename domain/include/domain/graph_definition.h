#pragma once
#include <signal_contracts/frame.h>
#include <cstdint>
#include <optional>
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
    WindowFunction, BarkWeighting, WindowView, WindowedView, NetworkInput, NetworkOutput
};
std::string_view kind_id(NodeKind kind) noexcept;
std::optional<NodeKind> node_kind(std::string_view canonical_id) noexcept;
enum class AudioSource { SystemOutput = 0, Microphone = 1 };
enum class MergeMode { Mean = 0, Maximum = 1, RMS = 2 };
enum class Channel { Left = 0, Right = 1, Merged = 2 };
enum class WindowFunction { Hann, Hamming, Blackman, Rectangular };
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
using NodeParameters = std::variant<NoParameters, AudioSourceParameters, MergeParameters,
    GainParameters, MelParameters, HistoryParameters, FFTViewParameters, WindowParameters,
    WindowFunctionParameters, TestSignalParameters, NetworkParameters>;
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
    bool operator==(const Edge&) const = default;
};
// Drafts may be invalid. Only validate_graph can authorize preparation.
// JSON extensions and layout belong to the document adapter, not this model.
struct GraphDefinition {
    std::string graph_id = "main";
    std::vector<NodeDefinition> nodes;
    std::vector<Edge> edges;
    bool operator==(const GraphDefinition&) const = default;
};
std::optional<FrameType> input_type(const NodeDefinition&) noexcept;
std::optional<FrameType> output_type(const NodeDefinition&) noexcept;
}
