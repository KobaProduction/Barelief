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
    {NodeKind::ChannelComparison,"ChannelComparison",NodeRole::Processor}
};
static_assert(std::size(nodes)==node_kind_count);
bool ports_defined(const NodeDefinition& n) noexcept {
    bool known=false;for(const auto& d:nodes)if(d.kind==n.kind){known=true;break;}
    if(!known)return false;
    if(is_view(n.kind) && std::holds_alternative<NoParameters>(n.parameters))return true;
    if(n.parameters.index()!=default_parameters(n.kind).index())return false;
    if(const auto* p=std::get_if<RoutingParameters>(&n.parameters))
        return signal_contracts::frame_descriptor(p->data_type) &&
            ((n.kind!=NodeKind::Splitter && n.kind!=NodeKind::Switch) || (p->outputs>=2 && p->outputs<=8));
    if(const auto* p=std::get_if<ReductionParameters>(&n.parameters))
        return signal_contracts::frame_descriptor(p->data_type) && p->data_type!=FrameType::WledPacket;
    if(const auto* p=std::get_if<NetworkParameters>(&n.parameters))return signal_contracts::frame_descriptor(p->data_type);
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
    case NodeKind::Spectrogram: return HistoryParameters{};
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
NodeRole node_role(NodeKind k) noexcept {
    const auto* d=node_descriptor(k);return d?d->role:NodeRole::Unknown;
}
std::uint16_t input_ports(const NodeDefinition& n) noexcept { return !ports_defined(n) || node_role(n.kind)==NodeRole::Source?0:(n.kind==NodeKind::InputSelector || n.kind==NodeKind::Mixer || n.kind==NodeKind::ChannelComparison)?2:1; }
std::uint16_t output_ports(const NodeDefinition& n) noexcept {
    if(!ports_defined(n))return 0;
    if(node_role(n.kind)==NodeRole::Sink || n.kind==NodeKind::SampleClock)return 0;
    if(n.kind==NodeKind::Spectrogram)return 2;
    if(n.kind==NodeKind::Splitter || n.kind==NodeKind::Switch) {
        const auto *p=std::get_if<RoutingParameters>(&n.parameters);return p?p->outputs:0;
    }
    return 1;
}
std::optional<FrameType> input_type(const NodeDefinition& n, std::uint16_t port) noexcept {
    if(port>=input_ports(n))return {};
    switch(n.kind) {
    case NodeKind::ChannelComparison: case NodeKind::Mixer: case NodeKind::PcmResampler: case NodeKind::InputSelector: case NodeKind::Window: case NodeKind::AudioView: return FrameType::PCM;
    case NodeKind::WindowFunction: case NodeKind::WindowView: return FrameType::Window;
    case NodeKind::StereoFFT: case NodeKind::WindowedView: return FrameType::Windowed;
    case NodeKind::BarkWeighting: case NodeKind::SpectrumMerge: case NodeKind::FFTView: return FrameType::StereoSpectrum;
    case NodeKind::SpectrumGain: case NodeKind::Mel: return FrameType::Spectrum;
    case NodeKind::Spectrogram: case NodeKind::MelView: case NodeKind::WLED: return FrameType::Mel;
    case NodeKind::SpectrogramView: return FrameType::History;
    case NodeKind::ScalarView: return FrameType::Scalar;
    case NodeKind::WLEDEncode: return FrameType::Mel;
    case NodeKind::WLEDOutput: return FrameType::WledPacket;
    case NodeKind::Splitter: case NodeKind::Switch: case NodeKind::Gate:
        if(const auto *p=std::get_if<RoutingParameters>(&n.parameters))return p->data_type;
        return {};
    case NodeKind::ScalarReduce:
        if(const auto *p=std::get_if<ReductionParameters>(&n.parameters))return p->data_type;
        return {};
    case NodeKind::NetworkOutput:
        if (const auto* p=std::get_if<NetworkParameters>(&n.parameters)) return p->data_type;
        return {};
    default: return {};
    }
}
std::optional<FrameType> output_type(const NodeDefinition& n, std::uint16_t port) noexcept {
    if(port>=output_ports(n))return {};
    switch(n.kind) {
    case NodeKind::SignalGenerator: case NodeKind::Mixer: case NodeKind::PcmResampler: case NodeKind::InputSelector: case NodeKind::AudioSource: case NodeKind::TestSignal: return FrameType::PCM;
    case NodeKind::Window: return FrameType::Window;
    case NodeKind::WindowFunction: return FrameType::Windowed;
    case NodeKind::StereoFFT: case NodeKind::BarkWeighting: return FrameType::StereoSpectrum;
    case NodeKind::SpectrumMerge: case NodeKind::SpectrumGain: return FrameType::Spectrum;
    case NodeKind::Mel: return FrameType::Mel;
    case NodeKind::Spectrogram: return port==0?FrameType::History:FrameType::Scalar;
    case NodeKind::ChannelComparison: case NodeKind::ScalarReduce: return FrameType::Scalar;
    case NodeKind::WLEDEncode: return FrameType::WledPacket;
    case NodeKind::Splitter: case NodeKind::Switch: case NodeKind::Gate:
        if(const auto *p=std::get_if<RoutingParameters>(&n.parameters))return p->data_type;
        return {};
    case NodeKind::NetworkInput:
        if (const auto* p=std::get_if<NetworkParameters>(&n.parameters)) return p->data_type;
        return {};
    default: return {};
    }
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
    auto fill=[&](auto storage,PortDirection direction,std::size_t count) {
        for(std::size_t i=0;i<count;++i) {
            auto type=direction==PortDirection::Input?input_type(n,std::uint16_t(i)):output_type(n,std::uint16_t(i));
            const auto* d=type?frame_descriptor(*type):nullptr;
            if(!d)return false;
            auto quantity=d->quantity;auto unit=d->unit;
            if(direction==PortDirection::Output && n.kind==NodeKind::Spectrogram && i==1) {
                quantity=Quantity::FilledFrameCount;unit=Unit::Frames;
            } else if(direction==PortDirection::Output && n.kind==NodeKind::ScalarReduce) {
                const auto* source=frame_descriptor(std::get<ReductionParameters>(n.parameters).data_type);
                quantity=source->quantity;unit=source->unit;
            }
            storage[i]={{n.id,std::uint16_t(i)},direction,*type,quantity,unit};
        }
        return true;
    };
    if(!fill(ins,PortDirection::Input,result.required_inputs) || !fill(outs,PortDirection::Output,result.required_outputs))return Status::invalid_argument;
    result.inputs=ins.first(result.required_inputs);result.outputs=outs.first(result.required_outputs);
    return Status::ok;
}

std::span<const ParameterDescriptor> parameter_descriptors(NodeKind k) noexcept {
    using C=ParameterClassification;using V=ParameterValueKind;
    static constexpr ParameterDescriptor source[]{{"source",C::Processing,V::Integer,0,1,0,{}},{"sample_rate",C::Processing,V::Integer,1000,192000,48000,{}}};
    static constexpr ParameterDescriptor resampler[]{{"output_rate",C::Processing,V::Integer,8000,192000,48000,{}}};
    static constexpr ParameterDescriptor comparison[]{
        {"window_samples",C::Processing,V::Integer,1,65536,512,{}},
        {"silence_threshold",C::Processing,V::Number,0,1,1e-12,{}}};
    static constexpr ParameterDescriptor selector[]{
        {"selected_input",C::Processing,V::Integer,0,65535,0,{}},
        {"channels_per_stream",C::Processing,V::Integer,1,32,1,{}}};
    static constexpr ParameterDescriptor extract[]{
        {"channel_index",C::Processing,V::Integer,0,65535,0,{}},
        {"index",C::Processing,V::Integer,0,2147483647,0,{}},
        {"column",C::Processing,V::Integer,0,2147483647,0,{}}};
    static constexpr ParameterDescriptor merge[]{{"merge",C::Processing,V::Integer,0,2,2,{}}};
    static constexpr ParameterDescriptor gain[]{{"gain",C::Processing,V::Number,0,100,1,{}}};
    static constexpr ParameterDescriptor mel[]{{"bands",C::Processing,V::Integer,1,50,50,{}}};
    static constexpr ParameterDescriptor history[]{{"history_size",C::Processing,V::Integer,2,2000,400,{}}};
    static constexpr ParameterDescriptor view[]{{"channel",C::Presentation,V::Integer,0,2,0,{}},{"merge",C::Presentation,V::Integer,0,2,2,{}}};
    static constexpr ParameterDescriptor window[]{{"size",C::Processing,V::Integer,512,4096,4096,{}}};
    static constexpr ParameterDescriptor function[]{{"function",C::Processing,V::WindowFunction,0,0,0,"hann"}};
    static constexpr ParameterDescriptor signal[]{
        {"tones",C::Processing,V::Integer,5,10,8,{}},{"sample_rate",C::Processing,V::Integer,8000,192000,48000,{}},
        {"minimum_hz",C::Processing,V::Number,10,96000,73,{}},{"maximum_hz",C::Processing,V::Number,10,96000,13901,{}},
        {"amplitude",C::Processing,V::Number,0,1,.4,{}},{"period_seconds",C::Processing,V::Number,.1,3600,6,{}},
        {"sweep_depth",C::Processing,V::Number,0,.95,.18,{}}};
    static constexpr ParameterDescriptor wled_output[]{
        {"destination",C::Processing,V::String,0,0,0,"255.255.255.255"},
        {"port",C::Processing,V::Integer,1,65535,11988,{}},
        {"interface",C::Processing,V::String,0,0,0,"0.0.0.0"},
        {"cadence_ms",C::Processing,V::Integer,1,60000,17,{}}};
    static constexpr ParameterDescriptor network[]{
        {"data_type",C::Processing,V::FrameType,0,0,0,"mel"},
        {"provider_uuid",C::ApplicationAdapter,V::String,0,0,0,{}},{"stream_id",C::ApplicationAdapter,V::String,0,0,0,{}},
        {"host",C::ApplicationAdapter,V::String,0,0,0,{}},{"port",C::ApplicationAdapter,V::Integer,1,65535,8214,{}}};
    static constexpr ParameterDescriptor split[]{
        {"data_type",C::Processing,V::FrameType,0,0,0,"mel"},{"outputs",C::Processing,V::Integer,2,8,2,{}}};
    static constexpr ParameterDescriptor route[]{
        {"data_type",C::Processing,V::FrameType,0,0,0,"mel"},{"outputs",C::Processing,V::Integer,2,8,2,{}},
        {"selected",C::Processing,V::Integer,0,7,0,{}}};
    static constexpr ParameterDescriptor gate[]{{"data_type",C::Processing,V::FrameType,0,0,0,"mel"}};
    static constexpr ParameterDescriptor reduce[]{
        {"data_type",C::Processing,V::FrameType,0,0,0,"mel"},{"operation",C::Processing,V::Integer,0,2,0,{}}};
    switch(k) {
    case NodeKind::AudioSource:return source;
    case NodeKind::PcmResampler:return resampler;
    case NodeKind::InputSelector:return selector;
    case NodeKind::ChannelComparison:return comparison;
    case NodeKind::IndexExtract:return extract;
    case NodeKind::SpectrumMerge:return merge;
    case NodeKind::SpectrumGain:case NodeKind::WLED:case NodeKind::WLEDEncode:return gain;
    case NodeKind::Mel:return mel;
    case NodeKind::Spectrogram:return history;
    case NodeKind::FFTView:return view;
    case NodeKind::Window:return window;
    case NodeKind::WindowFunction:return function;
    case NodeKind::TestSignal:return signal;
    case NodeKind::WLEDOutput:return wled_output;
    case NodeKind::NetworkInput:case NodeKind::NetworkOutput:return network;
    case NodeKind::Splitter:return split;
    case NodeKind::Switch:return route;
    case NodeKind::Gate:return gate;
    case NodeKind::ScalarReduce:return reduce;
    default:return {};
    }
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
