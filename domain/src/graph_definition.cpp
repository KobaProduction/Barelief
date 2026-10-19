#include <domain/graph_definition.h>
#include <array>
#include <limits>
namespace domain {
namespace {
constexpr NodeDescriptor nodes[]{
    {NodeKind::AudioSource,"AudioSource",NodeRole::Source},
    {NodeKind::StereoFFT,"StereoFFT",NodeRole::Processor},
    {NodeKind::SpectrumMerge,"SpectrumMerge",NodeRole::Processor},
    {NodeKind::SpectrumGain,"SpectrumGain",NodeRole::Processor},
    {NodeKind::Mel,"Mel",NodeRole::Processor},
    {NodeKind::Spectrogram,"Spectrogram",NodeRole::Processor},
    {NodeKind::FFTView,"FFTView",NodeRole::Sink,true},
    {NodeKind::MelView,"MelView",NodeRole::Sink,true},
    {NodeKind::SpectrogramView,"SpectrogramView",NodeRole::Sink,true},
    {NodeKind::AudioView,"AudioView",NodeRole::Sink,true},
    {NodeKind::WLED,"WLED",NodeRole::Sink},
    {NodeKind::TestSignal,"TestSignal",NodeRole::Source},
    {NodeKind::Window,"Window",NodeRole::Processor},
    {NodeKind::WindowFunction,"WindowFunction",NodeRole::Processor},
    {NodeKind::BarkWeighting,"BarkWeighting",NodeRole::Processor},
    {NodeKind::WindowView,"WindowView",NodeRole::Sink,true},
    {NodeKind::WindowedView,"WindowedView",NodeRole::Sink,true},
    {NodeKind::NetworkInput,"NetworkInput",NodeRole::Source},
    {NodeKind::NetworkOutput,"NetworkOutput",NodeRole::Sink},
    {NodeKind::Splitter,"Splitter",NodeRole::Processor},
    {NodeKind::Switch,"Switch",NodeRole::Processor},
    {NodeKind::Gate,"Gate",NodeRole::Processor},
    {NodeKind::ScalarReduce,"ScalarReduce",NodeRole::Processor},
    {NodeKind::ScalarView,"ScalarView",NodeRole::Sink,true},
    {NodeKind::WLEDEncode,"WLEDEncode",NodeRole::Processor},
    {NodeKind::WLEDOutput,"WLEDOutput",NodeRole::Sink},
    {NodeKind::InputSelector,"InputSelector",NodeRole::Processor},
    {NodeKind::PcmResampler,"PcmResampler",NodeRole::Processor},
    {NodeKind::IndexExtract,"IndexExtract",NodeRole::Processor},
    {NodeKind::SignalGenerator,"SignalGenerator",NodeRole::Source},
    {NodeKind::SampleClock,"SampleClock",NodeRole::Source},
    {NodeKind::Mixer,"Mixer",NodeRole::Processor},
    {NodeKind::ChannelComparison,"ChannelComparison",NodeRole::Processor},
    {NodeKind::MatrixTranspose,"MatrixTranspose",NodeRole::Processor},
    {NodeKind::MatrixWindowFunction,"MatrixWindowFunction",NodeRole::Processor},
    {NodeKind::MatrixFFT,"MatrixFFT",NodeRole::Processor},
    {NodeKind::MatrixRow,"MatrixRow",NodeRole::Processor},
    {NodeKind::MatrixCenter,"MatrixCenter",NodeRole::Processor},
    {NodeKind::RFFT,"RFFT",NodeRole::Processor},
    {NodeKind::SpectrumProjection,"SpectrumProjection",NodeRole::Processor},
    {NodeKind::FrequencyNormalizer,"FrequencyNormalizer",NodeRole::Processor},
    {NodeKind::BroadbandFeatures,"BroadbandFeatures",NodeRole::Processor},
    {NodeKind::NoteSalience,"NoteSalience",NodeRole::Processor},
    {NodeKind::SpectralQuantize,"SpectralQuantize",NodeRole::Processor},
    {NodeKind::Fourier,"Fourier",NodeRole::Processor},
    {NodeKind::RealFourier,"RealFourier",NodeRole::Processor},
    {NodeKind::ComplexPolar,"ComplexPolar",NodeRole::Processor},
    {NodeKind::Reroute,"Reroute",NodeRole::Processor},
    {NodeKind::Fs,"Fs",NodeRole::Source},
    {NodeKind::UInt32Constant,"UInt32Constant",NodeRole::Source},
    {NodeKind::Number,"Number",NodeRole::Source},
    {NodeKind::ArrayFill,"ArrayFill",NodeRole::Processor},
    {NodeKind::ArrayMultiply,"ArrayMultiply",NodeRole::Processor},
    {NodeKind::MusicalSpectrogram,"MusicalSpectrogram",NodeRole::Processor},
    {NodeKind::AmplitudePhase,"AmplitudePhase",NodeRole::Processor},
    {NodeKind::WindowGain,"WindowGain",NodeRole::Processor},
    {NodeKind::ArrayLength,"ArrayLength",NodeRole::Processor},
    {NodeKind::SampleRate,"SampleRate",NodeRole::Processor},
    {NodeKind::FrequencyStep,"FrequencyStep",NodeRole::Processor},
    {NodeKind::BroadbandRow,"BroadbandRow",NodeRole::Processor},
    {NodeKind::NoteSpectrum,"NoteSpectrum",NodeRole::Processor},
    {NodeKind::ArrayConcat,"ArrayConcat",NodeRole::Processor},
    {NodeKind::PianoView,"PianoView",NodeRole::Sink,true},
    {NodeKind::SpectrumAmplitude,"SpectrumAmplitude",NodeRole::Processor},
    {NodeKind::SpectrumPeak,"SpectrumPeak",NodeRole::Processor},
    {NodeKind::ScalarMetricsView,"ScalarMetricsView",NodeRole::Sink,true},
    {NodeKind::ScalarSignedBarView,"ScalarSignedBarView",NodeRole::Sink,true},
    {NodeKind::VectorLineView,"VectorLineView",NodeRole::Sink,true},
    {NodeKind::VectorPointView,"VectorPointView",NodeRole::Sink,true},
    {NodeKind::VectorBarView,"VectorBarView",NodeRole::Sink,true},
    {NodeKind::VectorPieView,"VectorPieView",NodeRole::Sink,true},
    {NodeKind::MatrixImageView,"MatrixImageView",NodeRole::Sink,true},
    {NodeKind::MatrixSurfaceView,"MatrixSurfaceView",NodeRole::Sink,true},
    {NodeKind::AudioLineView,"AudioLineView",NodeRole::Sink,true},
    {NodeKind::AudioPointView,"AudioPointView",NodeRole::Sink,true},
    {NodeKind::AudioBarView,"AudioBarView",NodeRole::Sink,true},
    {NodeKind::AudioPieView,"AudioPieView",NodeRole::Sink,true},
    {NodeKind::AudioImageView,"AudioImageView",NodeRole::Sink,true},
    {NodeKind::AudioSurfaceView,"AudioSurfaceView",NodeRole::Sink,true},
    {NodeKind::NumberAccumulator,"NumberAccumulator",NodeRole::Processor}
};
static_assert(std::size(nodes)==node_kind_count);
bool configured_ports_valid(const NodeDefinition& node) noexcept {
    if(const auto* routing=std::get_if<RoutingParameters>(&node.parameters))
        return signal_contracts::frame_descriptor(routing->data_type) &&
            ((node.kind!=NodeKind::Splitter && node.kind!=NodeKind::Switch) ||
             (routing->outputs>=2 && routing->outputs<=8));
    if(const auto* reduction=std::get_if<ReductionParameters>(&node.parameters))
        return signal_contracts::frame_descriptor(reduction->data_type) && reduction->data_type!=FrameType::WledPacket;
    if(const auto* network=std::get_if<NetworkParameters>(&node.parameters))
        return signal_contracts::frame_descriptor(network->data_type);
    return true;
}
bool ports_defined(const NodeDefinition& n) noexcept {
    bool known=false;for(const auto& d:nodes)if(d.kind==n.kind){known=true;break;}
    if(!known)return false;
    if(is_view(n.kind) && std::holds_alternative<NoParameters>(n.parameters))return true;
    if(n.parameters.index()!=default_parameters(n.kind).index())return false;
    return configured_ports_valid(n);
}
std::optional<FrameType> configured_input_type(const NodeDefinition& node) noexcept {
    if(const auto* routing=std::get_if<RoutingParameters>(&node.parameters))return routing->data_type;
    if(const auto* reduction=std::get_if<ReductionParameters>(&node.parameters))return reduction->data_type;
    if(const auto* network=std::get_if<NetworkParameters>(&node.parameters))return network->data_type;
    return {};
}
std::optional<FrameType> configured_output_type(const NodeDefinition& node) noexcept {
    if(const auto* routing=std::get_if<RoutingParameters>(&node.parameters))return routing->data_type;
    if(const auto* network=std::get_if<NetworkParameters>(&node.parameters))return network->data_type;
    return {};
}
std::optional<FrameType> audio_input_type(NodeKind kind) noexcept {
    switch(kind) {
    case NodeKind::ChannelComparison: case NodeKind::Mixer: case NodeKind::PcmResampler:
    case NodeKind::InputSelector: case NodeKind::Window: case NodeKind::AudioView: return FrameType::PCM;
    case NodeKind::WindowFunction: case NodeKind::WindowView: return FrameType::Window;
    case NodeKind::StereoFFT: case NodeKind::WindowedView: return FrameType::Windowed;
    case NodeKind::BarkWeighting: case NodeKind::SpectrumMerge: case NodeKind::FFTView: return FrameType::StereoSpectrum;
    default: return {};
    }
}
std::optional<FrameType> analysis_input_type(NodeKind kind,std::uint16_t port) noexcept {
    switch(kind) {
    case NodeKind::SpectrumGain: case NodeKind::Mel: case NodeKind::SpectrumPeak: return FrameType::Spectrum;
    case NodeKind::Spectrogram: case NodeKind::MelView: case NodeKind::WLED: return FrameType::Mel;
    case NodeKind::NumberAccumulator:return FrameType::Scalar;
    case NodeKind::SpectrogramView: return FrameType::History;
    case NodeKind::ScalarView: return FrameType::Scalar;
    case NodeKind::WLEDEncode: return port==0?FrameType::Mel:FrameType::Scalar;
    case NodeKind::WLEDOutput: return FrameType::WledPacket;
    default: return {};
    }
}
std::optional<FrameType> audio_output_type(NodeKind kind) noexcept {
    switch(kind) {
    case NodeKind::SignalGenerator: case NodeKind::Mixer: case NodeKind::PcmResampler:
    case NodeKind::InputSelector: case NodeKind::AudioSource: case NodeKind::TestSignal: return FrameType::PCM;
    case NodeKind::Window: return FrameType::Window;
    case NodeKind::WindowFunction: return FrameType::Windowed;
    case NodeKind::StereoFFT: case NodeKind::BarkWeighting: return FrameType::StereoSpectrum;
    default: return {};
    }
}
std::optional<FrameType> analysis_output_type(NodeKind kind,std::uint16_t port) noexcept {
    switch(kind) {
    case NodeKind::SpectrumMerge: case NodeKind::SpectrumGain: return FrameType::Spectrum;
    case NodeKind::Mel: return FrameType::Mel;
    case NodeKind::Spectrogram: case NodeKind::NumberAccumulator: return port==0?FrameType::History:FrameType::Scalar;
    case NodeKind::SpectrumPeak: case NodeKind::ChannelComparison: case NodeKind::ScalarReduce: return FrameType::Scalar;
    case NodeKind::WLEDEncode: return FrameType::WledPacket;
    default: return {};
    }
}
bool fill_port_descriptor(const NodeDefinition& node,PortDirection direction,std::size_t index,
                          PortDescriptor& port) noexcept {
    using namespace signal_contracts;
    const auto type=direction==PortDirection::Input?input_type(node,std::uint16_t(index)):
        output_type(node,std::uint16_t(index));
    const auto* descriptor=type?frame_descriptor(*type):nullptr;
    if(!descriptor)return false;
    auto quantity=descriptor->quantity;
    auto unit=descriptor->unit;
    if(direction==PortDirection::Output && (node.kind==NodeKind::Spectrogram || node.kind==NodeKind::NumberAccumulator) && index==1) {
        quantity=Quantity::FilledFrameCount;unit=Unit::Frames;
    }else if(direction==PortDirection::Output && node.kind==NodeKind::ScalarReduce) {
        const auto* source=frame_descriptor(std::get<ReductionParameters>(node.parameters).data_type);
        quantity=source->quantity;unit=source->unit;
    }
    port={{node.id,std::uint16_t(index)},direction,*type,quantity,unit};
    return true;
}

bool fill_port_descriptors(const NodeDefinition& node,std::span<PortDescriptor> storage,
                           PortDirection direction,std::size_t count) noexcept {
    for(std::size_t index=0;index<count;++index) {
        if(!fill_port_descriptor(node,direction,index,storage[index]))return false;
    }
    return true;
}
}
std::span<const NodeDescriptor> node_descriptors() noexcept { return nodes; }
const NodeDescriptor* node_descriptor(NodeKind k) noexcept {
    for(const auto& d:nodes) if(d.kind==k) return &d;
    return nullptr;
}
std::string_view kind_id(NodeKind kind) noexcept {
    const auto* d=node_descriptor(kind);return d?d->id:std::string_view{};
}
std::optional<NodeKind> node_kind(std::string_view id) noexcept {
    for (const auto& d:nodes) if (d.id==id) return d.kind;
    return {};
}
NodeParameters default_parameters(NodeKind kind) {
    switch(kind) {
    case NodeKind::AudioSource: return AudioSourceParameters{};
    case NodeKind::SpectrumMerge: return MergeParameters{};
    case NodeKind::SpectrumGain: case NodeKind::WLED: case NodeKind::WLEDEncode: return GainParameters{};
    case NodeKind::Mel: return MelParameters{};
    case NodeKind::Spectrogram: case NodeKind::NumberAccumulator: return HistoryParameters{};
    case NodeKind::FFTView: return FFTViewParameters{};
    case NodeKind::TestSignal: return TestSignalParameters{};
    case NodeKind::Window: return WindowParameters{};
    case NodeKind::WindowFunction: return WindowFunctionParameters{};
    case NodeKind::NetworkInput: case NodeKind::NetworkOutput: return NetworkParameters{};
    case NodeKind::Splitter: case NodeKind::Switch: case NodeKind::Gate: return RoutingParameters{};
    case NodeKind::ScalarReduce: return ReductionParameters{};
    default: return NoParameters{};
    }
}
bool is_view(NodeKind k) noexcept {
    const auto* d=node_descriptor(k);return d && d->view;
}
bool is_scalar_view(NodeKind kind) noexcept {
    return kind==NodeKind::ScalarView || kind==NodeKind::ScalarMetricsView || kind==NodeKind::ScalarSignedBarView;
}
NodeRole node_role(NodeKind k) noexcept {
    const auto* d=node_descriptor(k);return d?d->role:NodeRole::Unknown;
}
std::uint16_t input_ports(const NodeDefinition& n) noexcept { return !ports_defined(n) || node_role(n.kind)==NodeRole::Source?0:n.kind==NodeKind::WLEDEncode?3:(n.kind==NodeKind::InputSelector || n.kind==NodeKind::Mixer || n.kind==NodeKind::ChannelComparison)?2:1; }
std::uint16_t output_ports(const NodeDefinition& n) noexcept {
    if(!ports_defined(n))return 0;
    if(node_role(n.kind)==NodeRole::Sink || n.kind==NodeKind::SampleClock)return 0;
    if(n.kind==NodeKind::SpectrumPeak)return 2;
    if(n.kind==NodeKind::Spectrogram || n.kind==NodeKind::NumberAccumulator)return 2;
    if(n.kind==NodeKind::Splitter || n.kind==NodeKind::Switch) {
        const auto *p=std::get_if<RoutingParameters>(&n.parameters);return p?p->outputs:0;
    }
    return 1;
}
std::optional<FrameType> input_type(const NodeDefinition& n, std::uint16_t port) noexcept {
    if(port>=input_ports(n))return {};
    if(const auto configured=configured_input_type(n))return configured;
    if(const auto audio=audio_input_type(n.kind))return audio;
    return analysis_input_type(n.kind,port);
}
std::optional<FrameType> output_type(const NodeDefinition& n, std::uint16_t port) noexcept {
    if(port>=output_ports(n))return {};
    if(const auto configured=configured_output_type(n))return configured;
    if(const auto audio=audio_output_type(n.kind))return audio;
    return analysis_output_type(n.kind,port);
}
NodeDefinition project_processing_node(const NodeDefinition& n) {
    auto projected=n;if(is_view(n.kind))projected.parameters=NoParameters{};
    return projected;
}
signal_contracts::Status describe_node(const NodeDefinition& n,std::span<PortDescriptor> ins,
    std::span<PortDescriptor> outs,ConfiguredNodeDescriptor& result) noexcept {
    using namespace signal_contracts;
    result={};
    if(!ports_defined(n) || n.id<0 || n.id==std::numeric_limits<NodeId>::max())return Status::invalid_argument;
    result.node=n.id;result.kind=n.kind;result.role=node_role(n.kind);
    result.required_inputs=input_ports(n);result.required_outputs=output_ports(n);
    if(ins.size()<result.required_inputs || outs.size()<result.required_outputs)return Status::buffer_too_small;
    if(!fill_port_descriptors(n,ins,PortDirection::Input,result.required_inputs) ||
       !fill_port_descriptors(n,outs,PortDirection::Output,result.required_outputs))return Status::invalid_argument;
    result.inputs=ins.first(result.required_inputs);result.outputs=outs.first(result.required_outputs);
    return Status::ok;
}

const ParameterDescriptor* parameter_descriptor(NodeKind k,std::string_view key) noexcept {
    static constexpr ParameterDescriptor enabled{"enabled",ParameterClassification::Processing,ParameterValueKind::Boolean,0,1,1,{}};
    if(!node_descriptor(k))return nullptr;
    if(key=="enabled")return &enabled;
    for(const auto& d:parameter_descriptors(k))if(d.key==key)return &d;
    return nullptr;
}
ParameterClassification parameter_classification(NodeKind k,std::string_view key) noexcept {
    const auto* d=parameter_descriptor(k,key);return d?d->classification:ParameterClassification::OpaqueExtension;
}
}
