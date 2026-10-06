#include <dsp_kernels/dsp.h>
#include <dsp_core/dsp.h>
#include <bdsp_protocol/stream_codec.h>
#include <domain/graph_validation.h>
#include <application/graph_lifecycle.h>
#include <dataflow_runtime/runtime.h>
#include <embedded_support/profile.h>
#include <array>
#include <cmath>
int main() {
    const auto admission=embedded_support::admit(embedded_support::Profile{},10);
    if(admission.status!=signal_contracts::Status::ok) return 9;
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
    domain::GraphDefinition route;
    const std::array kinds{domain::NodeKind::NetworkInput,domain::NodeKind::Window,
        domain::NodeKind::WindowFunction,domain::NodeKind::StereoFFT,
        domain::NodeKind::SpectrumMerge,domain::NodeKind::Mel,domain::NodeKind::MelView};
    for(std::size_t i=0;i<kinds.size();++i) {
        route.nodes.push_back({int(i),kinds[i],domain::default_parameters(kinds[i]),true});
        if(i)route.edges.push_back({int(i-1),int(i)});
    }
    route.nodes[0].parameters=domain::NetworkParameters{signal_contracts::FrameType::PCM,"","sdk-source"};
    route.nodes[1].parameters=domain::WindowParameters{512};
    route.nodes[5].parameters=domain::MelParameters{8};
    std::array<dataflow_runtime::IngressSpec,1> ingress{{{0,{signal_contracts::FrameType::PCM,48000,0,0,0,512},512}}};
    std::shared_ptr<const dataflow_runtime::PreparedGraph> plan;
    std::unique_ptr<dataflow_runtime::RuntimeContext> context;
    if(dataflow_runtime::PreparedGraph::prepare(route,ingress,{},plan,context)!=signal_contracts::Status::ok) return 7;
    signal_contracts::FrameView pcm;pcm.type=signal_contracts::FrameType::PCM;
    pcm.metadata.sample_rate=48000;pcm.metadata.sequence=1;pcm.left=input;pcm.right=input;
    bool mel_received=false;
    const auto callback=[](void *user,const dataflow_runtime::OutputView &output){
        if(output.kind==domain::NodeKind::MelView && output.frame.values.size()==8)
            *static_cast<bool *>(user)=true;
    };
    // At 48 kHz the retained desktop cadence consumes 800 samples per frame.
    if(plan->process(0,pcm,*context,callback,&mel_received)!=signal_contracts::Status::ok) return 8;
    ++pcm.metadata.sequence;
    const auto processed=plan->process(0,pcm,*context,callback,&mel_received);
    if(processed!=signal_contracts::Status::ok || !mel_received) return 8;
    return 0;
}
