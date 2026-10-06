#include <dsp_kernels/dsp.h>
#include <bdsp_protocol/stream_codec.h>
#include <domain/graph_validation.h>
#include <application/graph_lifecycle.h>
#include <array>
#include <cmath>
int main() {
    std::array<float,512> input{}; std::array<float,256> spectrum{};
    input[0]=1;
    dsp_kernels::FFT fft;
    if(!fft.prepare(input.size()) || !fft.process(input,spectrum)) return 1;
    if(!std::isfinite(spectrum[0]) || spectrum[0]<=0) return 2;
    signal_contracts::FrameView frame;
    frame.type=signal_contracts::FrameType::Spectrum;frame.metadata.sequence=1;
    frame.metadata.sample_rate=48000;frame.values=spectrum;
    std::array<std::uint8_t,2048> bytes{};
    auto encoded=bdsp_protocol::encode(frame,bytes);
    if(!encoded) return 3;
    std::array<float,256> decoded{};signal_contracts::FrameView view;
    auto status=bdsp_protocol::decode(std::span(bytes).first(encoded.written_bytes),decoded,{},view);
    if(!status || view.values.size()!=spectrum.size() || view.values[0]!=spectrum[0]) return 4;
    domain::GraphDefinition graph;
    graph.nodes.push_back({0,domain::NodeKind::AudioSource,domain::AudioSourceParameters{},true});
    if(!domain::validate_graph(graph,domain::desktop_capabilities())) return 5;
    if(!application::MemoryRequirements{1,2,3}.fits(6)) return 6;
    return 0;
}
