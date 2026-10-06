#include <domain/graph_definition.h>
#include <array>
namespace domain {
namespace {
constexpr std::array<std::string_view,19> ids{
    "AudioSource", "StereoFFT", "SpectrumMerge", "SpectrumGain", "Mel", "Spectrogram",
    "FFTView", "MelView", "SpectrogramView", "AudioView", "WLED", "TestSignal", "Window",
    "WindowFunction", "BarkWeighting", "WindowView", "WindowedView", "NetworkInput", "NetworkOutput"};
}
std::string_view kind_id(NodeKind kind) noexcept {
    const auto i = static_cast<std::size_t>(kind);
    return i < ids.size() ? ids[i] : std::string_view{};
}
std::optional<NodeKind> node_kind(std::string_view id) noexcept {
    for (std::size_t i=0; i<ids.size(); ++i) if (ids[i]==id) return static_cast<NodeKind>(i);
    return {};
}
NodeParameters default_parameters(NodeKind kind) {
    switch(kind) {
    case NodeKind::AudioSource: return AudioSourceParameters{};
    case NodeKind::SpectrumMerge: return MergeParameters{};
    case NodeKind::SpectrumGain: case NodeKind::WLED: return GainParameters{};
    case NodeKind::Mel: return MelParameters{};
    case NodeKind::Spectrogram: return HistoryParameters{};
    case NodeKind::FFTView: return FFTViewParameters{};
    case NodeKind::TestSignal: return TestSignalParameters{};
    case NodeKind::Window: return WindowParameters{};
    case NodeKind::WindowFunction: return WindowFunctionParameters{};
    case NodeKind::NetworkInput: case NodeKind::NetworkOutput: return NetworkParameters{};
    default: return NoParameters{};
    }
}
std::optional<FrameType> input_type(const NodeDefinition& n) noexcept {
    switch(n.kind) {
    case NodeKind::Window: case NodeKind::AudioView: return FrameType::PCM;
    case NodeKind::WindowFunction: case NodeKind::WindowView: return FrameType::Window;
    case NodeKind::StereoFFT: case NodeKind::WindowedView: return FrameType::Windowed;
    case NodeKind::BarkWeighting: case NodeKind::SpectrumMerge: case NodeKind::FFTView: return FrameType::StereoSpectrum;
    case NodeKind::SpectrumGain: case NodeKind::Mel: return FrameType::Spectrum;
    case NodeKind::Spectrogram: case NodeKind::MelView: case NodeKind::WLED: return FrameType::Mel;
    case NodeKind::SpectrogramView: return FrameType::History;
    case NodeKind::NetworkOutput:
        if (const auto* p=std::get_if<NetworkParameters>(&n.parameters)) return p->data_type;
        return {};
    default: return {};
    }
}
std::optional<FrameType> output_type(const NodeDefinition& n) noexcept {
    switch(n.kind) {
    case NodeKind::AudioSource: case NodeKind::TestSignal: return FrameType::PCM;
    case NodeKind::Window: return FrameType::Window;
    case NodeKind::WindowFunction: return FrameType::Windowed;
    case NodeKind::StereoFFT: case NodeKind::BarkWeighting: return FrameType::StereoSpectrum;
    case NodeKind::SpectrumMerge: case NodeKind::SpectrumGain: return FrameType::Spectrum;
    case NodeKind::Mel: return FrameType::Mel;
    case NodeKind::Spectrogram: return FrameType::History;
    case NodeKind::NetworkInput:
        if (const auto* p=std::get_if<NetworkParameters>(&n.parameters)) return p->data_type;
        return {};
    default: return {};
    }
}
}
