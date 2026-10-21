#include "processing_admission.h"
#include <utility>
namespace domain {
using namespace signal_contracts;
namespace {
bool same_clock(const ClockContract& a,const ClockContract& b) noexcept {
    return a.sample_rate==b.sample_rate && a.domain==b.domain && a.tick_seconds==b.tick_seconds;
}
std::optional<ClockContract> referenced_clock(const ConfiguredProcessingNode& source) noexcept {
    if(const auto* clock=std::get_if<SampleClockProcessingParameters>(&source.parameters))return ClockContract{clock->domain,{1,clock->sample_rate},clock->sample_rate};
    if(source.outputs.empty() || !source.outputs.front().contract)return {};
    const auto output=source.outputs.front().contract.view();const auto* numeric=std::get_if<NumericContract>(&output);
    return numeric?std::optional<ClockContract>{numeric->clock}:std::nullopt;
}
std::optional<ClockContract> execution_clock(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& source) noexcept {
    if(source.kind=="SampleClock" && std::holds_alternative<SampleClockProcessingParameters>(source.parameters))return referenced_clock(source);
    if(source.kind=="SignalGenerator" && !has_configuration_binding(graph,source.id,ConfigurationRole::SampleClock))return signal_generator_clock(source);
    return {};
}
bool clock_consumer(std::string_view kind) noexcept {
    return kind=="SignalGenerator" || kind=="Mixer" || kind=="PcmResampler";
}
ProcessingDiagnostic validate_execution_clock(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& source,const ConfiguredProcessingNode& target) noexcept {
    const auto expected=execution_clock(graph,source);
    if(!expected)return {source.kind=="SignalGenerator"?ProcessingError::InvalidContract:ProcessingError::InvalidParameters,target.id};
    for(const auto& port:target.outputs) {
        const auto contract=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);
        if(numeric && !same_clock(numeric->clock,*expected))return {ProcessingError::IncompatiblePorts,target.id,port.id};
    }
    return {};
}
ProcessingDiagnostic validate_view_clock(const ConfiguredProcessingNode& source,const ConfiguredProcessingNode& target) noexcept {
    if(target.inputs.empty())return {};
    const auto input=target.inputs.front().contract.view();const auto* numeric=std::get_if<NumericContract>(&input);
    const auto expected=referenced_clock(source);
    if(!numeric || !expected || !expected->sample_rate || !same_clock(numeric->clock,*expected))return {ProcessingError::IncompatiblePorts,target.id};
    return {};
}
std::size_t frequency_axis(const NumericContract& contract) noexcept {
    return contract.shape.rank==Rank::Matrix && contract.shape.axes[1]!=Axis::Component?1:0;
}
ProcessingDiagnostic validate_frequency_grid(const ConfiguredProcessingNode& source,const ConfiguredProcessingNode& target) noexcept {
    if(source.outputs.empty() || target.inputs.empty())return {ProcessingError::InvalidContract,target.id};
    const auto output=source.outputs.front().contract.view(),input=target.inputs.front().contract.view();
    const auto* from=std::get_if<NumericContract>(&output);const auto* to=std::get_if<NumericContract>(&input);
    if(!from || !to)return {ProcessingError::InvalidContract,target.id};
    const auto a=frequency_axis(*from),b=frequency_axis(*to);
    if(from->coordinates[a].unit!=NumericUnit::Hertz || from->coordinates[a].mode==CoordinateMode::None || from->coordinates[a]!=to->coordinates[b] || from->shape.extents[a]!=to->shape.extents[b])return {ProcessingError::IncompatiblePorts,target.id};
    return {};
}
bool duplicate_association(const NumericProcessingGraph& graph,std::size_t index) noexcept {
    const auto& binding=graph.configuration_bindings[index];
    for(std::size_t previous=0;previous<index;++previous)
        if(graph.configuration_bindings[previous].to==binding.to && graph.configuration_bindings[previous].input==binding.input)return true;
    return false;
}
ProcessingDiagnostic validate_association_clock(const NumericProcessingGraph& graph,const ConfigurationBinding& binding,
    const ConfiguredProcessingNode& source,const ConfiguredProcessingNode& target) noexcept {
    if(binding.input==ConfigurationRole::FrequencyGrid)return validate_frequency_grid(source,target);
    if(clock_consumer(target.kind))return validate_execution_clock(graph,source,target);
    const auto kind=node_kind(target.kind);
    return kind && is_view(*kind)?validate_view_clock(source,target):ProcessingDiagnostic{};
}
ProcessingDiagnostic validate_association(const NumericProcessingGraph& graph,std::size_t index) noexcept {
    const auto& binding=graph.configuration_bindings[index];
    const auto from=detail::admission_node(graph,binding.from),to=detail::admission_node(graph,binding.to);
    if(from==graph.nodes.size() || to==graph.nodes.size())return {ProcessingError::MissingEndpoint,binding.to};
    const auto& source=graph.nodes[from];const auto& target=graph.nodes[to];
    if(binding.output!=binding.input || !configuration_output_supported(source,binding.output) || !configuration_input_supported(target,binding.input) || binding.from==binding.to)return {ProcessingError::InvalidPort,binding.to};
    if(duplicate_association(graph,index))return {ProcessingError::MultipleInputs,binding.to};
    return validate_association_clock(graph,binding,source,target);
}
}
bool configuration_output_supported(const ConfiguredProcessingNode& node,ConfigurationRole role) noexcept {
    if(role==ConfigurationRole::SampleClock) {
        if(node.kind=="SampleClock")return true;
        const auto clock=referenced_clock(node);return clock && clock->sample_rate && !clock->domain.empty();
    }
    constexpr std::string_view producers[]{"RFFT","SpectrumProjection","FrequencyNormalizer","BroadbandFeatures","StereoFFT","Mel","Spectrogram","BarkWeighting","MatrixFFT","MatrixRow"};
    return role==ConfigurationRole::FrequencyGrid && std::find(std::begin(producers),std::end(producers),node.kind)!=std::end(producers);
}
bool configuration_input_supported(const ConfiguredProcessingNode& node,ConfigurationRole role) noexcept {
    const auto kind=node_kind(node.kind);const bool view=kind && is_view(*kind);
    if(role==ConfigurationRole::SampleClock)return clock_consumer(node.kind) || view;
    if(role!=ConfigurationRole::FrequencyGrid || !view)return false;
    switch(*kind) {
    case NodeKind::ScalarView:case NodeKind::ScalarMetricsView:case NodeKind::ScalarSignedBarView:
    case NodeKind::VectorLineView:case NodeKind::VectorPointView:case NodeKind::VectorBarView:case NodeKind::VectorPieView:
    case NodeKind::MatrixImageView:case NodeKind::MatrixSurfaceView:
    case NodeKind::AudioImageView:case NodeKind::AudioSurfaceView:return false;
    default:return true;
    }
}
bool has_configuration_binding(const NumericProcessingGraph& graph,NodeId node,ConfigurationRole role) noexcept {
    return std::any_of(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),[&](const auto& binding){return binding.to==node && binding.input==role;});
}
namespace detail {
ProcessingDiagnostic validate_graph_bindings(const NumericProcessingGraph& graph,const ProcessingAdmissionLimits& limits) noexcept {
    if(graph.edges.size()>limits.max_edges || graph.configuration_bindings.size()>limits.max_edges-graph.edges.size())return {ProcessingError::Limit};
    for(std::size_t index=0;index<graph.configuration_bindings.size();++index) {
        const auto valid=validate_association(graph,index);if(!valid)return valid;
    }
    return {};
}
}
ProcessingDiagnostic resolve_configuration_bindings(const NumericProcessingGraph& source,const ProcessingAdmissionLimits& limits,NumericProcessingGraph& out) {
    const auto valid=detail::validate_graph_bindings(source,limits);if(!valid)return valid;
    auto candidate=source;for(auto& node:candidate.nodes)node.clock_node=-1;
    for(const auto& binding:candidate.configuration_bindings)if(binding.input==ConfigurationRole::SampleClock) {
        auto& target=candidate.nodes[detail::admission_node(candidate,binding.to)];
        if(clock_consumer(target.kind))target.clock_node=binding.from;
    }
    out=std::move(candidate);return {};
}
}
