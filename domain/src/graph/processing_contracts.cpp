#include <domain/processing_contracts.h>
#include "node_validation.h"
#include "processing_admission.h"
#include <algorithm>
#include <charconv>
#include <numeric>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
namespace domain {
using namespace signal_contracts;
bool valid_execution_interval(Ratio interval) noexcept {
    if(interval.numerator<=0 || !interval.denominator)return false;
    const auto n=std::uint64_t(interval.numerator),d=interval.denominator;
    // Practical portable bound: 1 ms through 60 s. Bounds avoid multiplication.
    return std::gcd(n,d)==1 && n>=d/1000+(d%1000!=0) &&
        (n/d<60 || (n/d==60 && n%d==0));
}
namespace {
bool add(std::size_t& a,std::size_t b) noexcept {
    if(b>std::numeric_limits<std::size_t>::max()-a)return false;
    a+=b;return true;
}


bool policy_equal(const ConversionPolicy& a,const ConversionPolicy& b) noexcept {
    return a.rounding==b.rounding && a.overflow==b.overflow && a.precision_loss==b.precision_loss && a.nonfinite==b.nonfinite &&
        a.scaling.mode==b.scaling.mode && a.scaling.gain.exponent2==b.scaling.gain.exponent2 && a.scaling.offset.exponent2==b.scaling.offset.exponent2 &&
        literal_equal(a.scaling.gain.numerator,b.scaling.gain.numerator) && literal_equal(a.scaling.offset.numerator,b.scaling.offset.numerator) &&
        literal_equal(a.nan_replacement,b.nan_replacement) && literal_equal(a.negative_inf_replacement,b.negative_inf_replacement) && literal_equal(a.positive_inf_replacement,b.positive_inf_replacement);
}
bool parameter_equal(const ProcessingParameterValue& a,const ProcessingParameterValue& b) noexcept {
    if(a.valueless_by_exception() || b.valueless_by_exception() || a.index()!=b.index())return false;
    if(const auto* x=std::get_if<NumericLiteral>(&a))return literal_equal(*x,std::get<NumericLiteral>(b));
    if(const auto* x=std::get_if<bool>(&a))return *x==std::get<bool>(b);
    if(const auto* x=std::get_if<NumericArray>(&a))return *x==std::get<NumericArray>(b);
    return std::get<std::string>(a)==std::get<std::string>(b);
}
const ProcessingParameter* unique_parameter(std::span<const ProcessingParameter> parameters,std::string_view key) noexcept {
    const ProcessingParameter* found=nullptr;
    for(const auto& parameter:parameters)if(parameter.key==key){if(found)return nullptr;found=&parameter;}
    return found;
}
bool parameters_equal(const std::vector<ProcessingParameter>& a,const std::vector<ProcessingParameter>& b) noexcept {
    if(a.size()!=b.size())return false;
    for(const auto& parameter:a) {
        const auto* found=unique_parameter(b,parameter.key);
        if(!found || !parameter_equal(parameter.value,found->value) || !unique_parameter(a,parameter.key))return false;
    }
    return true;
}
template<class T> bool typed_parameters_equal(const T& a,const T& b) noexcept {return a==b;}
bool typed_parameters_equal(const EndpointProcessingParameters& a,const EndpointProcessingParameters& b) noexcept {return parameters_equal(a.values,b.values);}
bool typed_parameters_equal(const TransformProcessingParameters& a,const TransformProcessingParameters& b) noexcept {return parameters_equal(a.values,b.values);}
bool typed_parameters_equal(const ConverterProcessingParameters& a,const ConverterProcessingParameters& b) noexcept {return policy_equal(a.policy,b.policy);}
bool typed_parameters_equal(const HistoryProcessingParameters& a,const HistoryProcessingParameters& b) noexcept {return a.capacity==b.capacity;}
bool typed_parameters_equal(const RouteProcessingParameters& a,const RouteProcessingParameters& b) noexcept {
    return a.outputs==b.outputs && a.selected==b.selected && a.open==b.open && a.channels_per_stream==b.channels_per_stream;
}
bool typed_parameters_equal(const ReduceProcessingParameters& a,const ReduceProcessingParameters& b) noexcept {
    return a.operation==b.operation && a.channels==b.channels && a.rows==b.rows && a.selected_channels==b.selected_channels && a.precision_loss==b.precision_loss && a.rounding==b.rounding;
}


std::optional<ProcessingOperation> numerical_operation(NodeKind k) noexcept {
    if(k==NodeKind::Fourier || k==NodeKind::RealFourier)return ProcessingOperation::Fourier;
    if(k==NodeKind::ComplexPolar || k==NodeKind::AmplitudePhase)return ProcessingOperation::ComplexPolar;
    if(k==NodeKind::ArrayFill)return ProcessingOperation::ArrayFill;
    if(k==NodeKind::ArrayMultiply)return ProcessingOperation::ArrayProduct;
    if(k==NodeKind::ArrayConcat)return ProcessingOperation::ArrayConcat;
    if(k==NodeKind::ArrayLength || k==NodeKind::SampleRate || k==NodeKind::WindowGain || k==NodeKind::FrequencyStep)return ProcessingOperation::Metadata;
    return {};
}
std::optional<ProcessingOperation> signal_operation(NodeKind k) noexcept {
    if(k==NodeKind::SpectrumPeak)return ProcessingOperation::SpectrumPeak;
    if(k==NodeKind::SampleClock)return ProcessingOperation::Clock;
    if(k==NodeKind::Mixer)return ProcessingOperation::Mixer;
    if(k==NodeKind::ChannelComparison)return ProcessingOperation::ChannelComparison;
    if(k==NodeKind::RFFT || k==NodeKind::SpectrumProjection || k==NodeKind::FrequencyNormalizer ||
       k==NodeKind::BroadbandFeatures || k==NodeKind::NoteSalience || k==NodeKind::MusicalSpectrogram || k==NodeKind::BroadbandRow || k==NodeKind::NoteSpectrum || k==NodeKind::SpectralQuantize)return ProcessingOperation::MusicTransform;
    return {};
}
std::optional<ProcessingOperation> routing_operation(NodeKind k) noexcept {
    switch(k) {
    case NodeKind::Splitter:return ProcessingOperation::Splitter;
    case NodeKind::Switch:return ProcessingOperation::Switch;
    case NodeKind::InputSelector:return ProcessingOperation::InputSelector;
    case NodeKind::Gate:return ProcessingOperation::Gate;
    case NodeKind::Reroute:return ProcessingOperation::Gate;
    default:return {};
    }
}
ProcessingOperation scalar_operation(NodeKind k) noexcept {
    switch(k) {
    case NodeKind::ScalarReduce:return ProcessingOperation::Reduce;
    case NodeKind::IndexExtract:return ProcessingOperation::Extract;
    case NodeKind::Spectrogram:case NodeKind::NumberAccumulator:return ProcessingOperation::History;
    case NodeKind::WLEDEncode:return ProcessingOperation::WLEDEncoder;
    default:return ProcessingOperation::Transform;
    }
}
ProcessingOperation matrix_operation(NodeKind k) noexcept {
    switch(k) {
    case NodeKind::MatrixTranspose:return ProcessingOperation::MatrixTranspose;
    case NodeKind::MatrixWindowFunction:return ProcessingOperation::MatrixWindowFunction;
    case NodeKind::MatrixFFT:return ProcessingOperation::MatrixFFT;
    case NodeKind::MatrixRow:return ProcessingOperation::MatrixRow;
    case NodeKind::MatrixCenter:return ProcessingOperation::MatrixCenter;
    default:return scalar_operation(k);
    }
}
ProcessingOperation operation(NodeKind k,NodeRole role) noexcept {
    if(const auto numeric=numerical_operation(k))return *numeric;
    if(const auto signal=signal_operation(k))return *signal;
    if(role==NodeRole::Source)return ProcessingOperation::Source;
    if(role==NodeRole::Sink)return ProcessingOperation::Sink;
    if(const auto routing=routing_operation(k))return *routing;
    return matrix_operation(k);
}

const ProcessingPort* find_port(std::span<const ProcessingPort> ports,PortId id) noexcept {
    for(const auto& p:ports)if(p.id==id)return &p;
    return nullptr;
}
std::size_t node_index(const NumericProcessingGraph& g,NodeId id) noexcept {
    for(std::size_t i=0;i<g.nodes.size();++i)if(g.nodes[i].id==id)return i;
    return g.nodes.size();
}

}
Status processing_versions_supported(const ProcessingVersions& v) noexcept {
    return v.graph==5 && v.processing==4?Status::ok:Status::unsupported_version;
}
bool same_processing_parameters(const ProcessingParameters& a,const ProcessingParameters& b) noexcept {
    if(a.valueless_by_exception() || b.valueless_by_exception() || a.index()!=b.index())return false;
    return std::visit([&](const auto& value){using T=std::decay_t<decltype(value)>;return typed_parameters_equal(value,std::get<T>(b));},a);
}
bool processing_ports_compatible(const ProcessingPort& output,const ProcessingPort& input) noexcept {
    return output.direction==PortDirection::Output && input.direction==PortDirection::Input && output.contract && input.contract && same_physical_contract(output.contract.view(),input.contract.view());
}
bool processing_ports_dtype_convertible(const ProcessingPort& output,const ProcessingPort& input) noexcept {
    if(output.direction!=PortDirection::Output || input.direction!=PortDirection::Input || !output.contract || !input.contract)return false;
    auto a=output.contract.view(),b=input.contract.view();
    auto* x=std::get_if<NumericContract>(&a);const auto* y=std::get_if<NumericContract>(&b);
    if(!x || !y || x->representation.encoding!=StorageEncoding::Native || y->representation.encoding!=StorageEncoding::Native ||
       x->representation.valid_bits || y->representation.valid_bits)return false;
    x->dtype=y->dtype;return same_physical_contract(a,b);
}
ConversionPolicy edge_conversion_policy(std::uint8_t mode) noexcept {
    ConversionPolicy p;
    if(mode)p.precision_loss=PrecisionLoss::AllowDeclared;
    if(mode==2)p.rounding=Rounding::TowardZero;
    if(mode==3)p.overflow=Overflow::Saturate;
    return p;
}
bool dtype_conversion_is_exact(DType from,DType to) noexcept {
    const auto* a=dtype_descriptor(from);const auto* b=dtype_descriptor(to);if(!a || !b)return false;
    if(from==to)return true;
    if(a->ieee)return b->ieee && b->bytes>a->bytes;
    if(b->ieee)return a->bytes*8-(a->signed_value?1:0)<=(to==DType::Float32?24:53);
    if(a->signed_value && !b->signed_value)return false;
    return a->signed_value==b->signed_value?b->bytes>=a->bytes:b->bytes>a->bytes;
}
namespace {
struct EdgePorts {const ProcessingPort* from=nullptr;const ProcessingPort* to=nullptr;};
ProcessingDiagnostic lookup_edge_ports(const NumericProcessingGraph& graph,const Edge& edge,std::size_t index,ProcessingError missing_port,EdgePorts& ports,std::optional<NodeId> missing_endpoint_node) {
    const auto from=node_index(graph,edge.from),to=node_index(graph,edge.to);
    if(from==graph.nodes.size() || to==graph.nodes.size())return {ProcessingError::MissingEndpoint,missing_endpoint_node.value_or(-1),missing_endpoint_node?edge.input_port:PortId{0},index};
    ports.from=find_port(graph.nodes[from].outputs,edge.output_port);ports.to=find_port(graph.nodes[to].inputs,edge.input_port);
    return ports.from && ports.to?ProcessingDiagnostic{}:ProcessingDiagnostic{missing_port,edge.to,edge.input_port,index};
}
ProcessingDiagnostic internal_node_id(const NumericProcessingGraph& graph,NodeId& id) {
    id=std::numeric_limits<NodeId>::max();
    while(std::any_of(graph.nodes.begin(),graph.nodes.end(),[id](const auto& node){return node.id==id;})) {
        if(id==0)return {ProcessingError::Limit};
        --id;
    }
    return {};
}
ConfiguredProcessingNode converter_node(NodeId id,const Edge& edge,EdgePorts ports) {
    ConfiguredProcessingNode node;node.id=id;node.kind="Converter";
    node.parameters=ConverterProcessingParameters{edge_conversion_policy(edge.conversion)};
    node.inputs.push_back({0,PortDirection::Input,ports.from->contract});
    node.outputs.push_back({0,PortDirection::Output,ports.to->contract});return node;
}
struct DtypeLowering {
    const NumericProcessingGraph& source;const ProcessingAdmissionLimits& limits;NumericProcessingGraph candidate;
    ProcessingDiagnostic append_converter(const Edge& edge,EdgePorts ports) {
        if(candidate.nodes.size()>=limits.max_nodes || candidate.edges.size()+2>limits.max_edges)return {ProcessingError::Limit};
        NodeId id;const auto valid=internal_node_id(candidate,id);if(!valid)return valid;
        candidate.nodes.push_back(converter_node(id,edge,ports));
        candidate.edges.push_back({edge.from,id,edge.output_port,0});candidate.edges.push_back({id,edge.to,0,edge.input_port});return {};
    }
    void append_direct(const Edge& edge) {
        auto copy=edge;copy.conversion=0;candidate.edges.push_back(copy);
    }
    ProcessingDiagnostic edge(std::size_t index) {
        const auto& value=source.edges[index];
        if(value.conversion>3)return {ProcessingError::InvalidParameters,value.to,value.input_port,index};
        EdgePorts ports;const auto valid=lookup_edge_ports(source,value,index,ProcessingError::MissingEndpoint,ports,std::nullopt);if(!valid)return valid;
        if(processing_ports_compatible(*ports.from,*ports.to)){append_direct(value);return {};}
        if(!processing_ports_dtype_convertible(*ports.from,*ports.to))return {ProcessingError::IncompatiblePorts,value.to,value.input_port,index};
        return append_converter(value,ports);
    }
    std::vector<std::size_t> ordered_edges() const {
        std::vector<std::size_t> order(source.edges.size());std::iota(order.begin(),order.end(),0);
        std::sort(order.begin(),order.end(),[&](auto x,auto y){const auto& a=source.edges[x];const auto& b=source.edges[y];return std::tie(a.to,a.input_port,a.from,a.output_port)<std::tie(b.to,b.input_port,b.from,b.output_port);});
        return order;
    }
    ProcessingDiagnostic lower() {
        const auto valid=resolve_configuration_bindings(source,limits,candidate);if(!valid)return valid;
        candidate.edges.clear();
        for(const auto index:ordered_edges()){const auto converted=edge(index);if(!converted)return converted;}
        if(candidate.nodes.size()==source.nodes.size())candidate.edges=source.edges;
        return {};
    }
};
bool producer_port_exists(const NumericProcessingGraph& graph,NodeId node,PortId output) {
    const auto index=node_index(graph,node);
    return index<graph.nodes.size() && find_port(graph.nodes[index].outputs,output);
}
ProcessingDiagnostic reroute_incoming(const NumericProcessingGraph& graph,NodeId target,const Edge*& incoming) {
    for(const auto& edge:graph.edges)if(edge.to==target) {
        if(edge.input_port!=0)return {ProcessingError::InvalidPort,target,edge.input_port};
        if(incoming)return {ProcessingError::MultipleInputs,target};
        incoming=&edge;
    }
    return {};
}
ProcessingDiagnostic follow_reroute(const NumericProcessingGraph& graph,RoutingAlias& alias,bool& disconnected) {
    const Edge* incoming=nullptr;const auto valid=reroute_incoming(graph,alias.source,incoming);if(!valid)return valid;
    if(!incoming){alias.source=-1;disconnected=true;return {};}
    if(incoming->conversion)return {ProcessingError::UnsupportedCombination,alias.source};
    alias.source=incoming->from;alias.source_port=incoming->output_port;
    return producer_port_exists(graph,alias.source,alias.source_port)?ProcessingDiagnostic{}:ProcessingDiagnostic{ProcessingError::MissingEndpoint,alias.source,alias.source_port};
}
ProcessingDiagnostic resolve_reroute_alias(const NumericProcessingGraph& graph,const ConfiguredProcessingNode& node,RoutingAlias& alias) {
    alias={node.id,node.id,0};
    for(std::size_t depth=0;;++depth) {
        if(depth>=graph.nodes.size())return {ProcessingError::Cycle,node.id};
        const auto at=node_index(graph,alias.source);if(at==graph.nodes.size())return {ProcessingError::MissingEndpoint,alias.source};
        if(graph.nodes[at].kind!="Reroute")return {};
        bool disconnected=false;const auto valid=follow_reroute(graph,alias,disconnected);if(!valid)return valid;
        if(disconnected)return {};
    }
}
struct RoutingLowering {
    const NumericProcessingGraph& source;const ProcessingAdmissionLimits& limits;
    NumericProcessingGraph candidate;std::vector<RoutingAlias> aliases;
    ProcessingDiagnostic identities() const {
        for(std::size_t i=0;i<source.nodes.size();++i)for(std::size_t j=0;j<i;++j)
            if(source.nodes[i].id==source.nodes[j].id)return {ProcessingError::InvalidNode,source.nodes[i].id};
        return {};
    }
    ProcessingDiagnostic edges() const {
        for(std::size_t index=0;index<source.edges.size();++index) {
            const auto& edge=source.edges[index];EdgePorts ports;
            const auto valid=lookup_edge_ports(source,edge,index,ProcessingError::InvalidPort,ports,edge.to);if(!valid)return valid;
            if(!processing_ports_compatible(*ports.from,*ports.to))return {ProcessingError::IncompatiblePorts,edge.to,edge.input_port,index};
        }
        return {};
    }
    ProcessingDiagnostic resolve_aliases() {
        for(const auto& node:source.nodes)if(node.kind=="Reroute") {
            ResolvedProcessingNode resolved;auto valid=resolve_processing_node(node,limits,resolved);if(!valid)return valid;
            RoutingAlias alias;valid=resolve_reroute_alias(source,node,alias);if(!valid)return valid;
            aliases.push_back(alias);
        }
        return {};
    }
    Edge rerouted(Edge edge) const {
        for(const auto& alias:aliases)if(alias.node==edge.from){edge.from=alias.source;edge.output_port=alias.source_port;break;}
        return edge;
    }
    ProcessingDiagnostic rewrite_edges() {
        for(const auto& edge:source.edges) {
            const auto target=node_index(source,edge.to);if(target==source.nodes.size())return {ProcessingError::MissingEndpoint,edge.to};
            if(source.nodes[target].kind=="Reroute")continue;
            const auto rewritten=rerouted(edge);if(rewritten.from>=0)candidate.edges.push_back(rewritten);
        }
        return {};
    }
    ProcessingDiagnostic configuration() const {
        for(const auto& binding:source.configuration_bindings)for(const auto& alias:aliases)
            if(binding.from==alias.node || binding.to==alias.node)return {ProcessingError::UnsupportedCombination,alias.node};
        return {};
    }
    ProcessingDiagnostic rewrite() {
        candidate=source;candidate.edges.clear();
        const auto valid=rewrite_edges();if(!valid)return valid;
        const auto bindings=configuration();if(!bindings)return bindings;
        std::erase_if(candidate.nodes,[](const auto& node){return node.kind=="Reroute";});return {};
    }
    ProcessingDiagnostic lower() {
        auto valid=identities();if(!valid)return valid;
        valid=edges();if(!valid)return valid;
        valid=resolve_aliases();if(!valid)return valid;
        return rewrite();
    }
};
}
ProcessingDiagnostic lower_dtype_edges(const NumericProcessingGraph& source,const ProcessingAdmissionLimits& limits,NumericProcessingGraph& out) {
    if(source.nodes.size()>limits.max_nodes || source.edges.size()>limits.max_edges)return {ProcessingError::Limit};
    std::size_t footprint=0;
    if(processing_graph_owned_bytes(source,limits,footprint)!=Status::ok)return {ProcessingError::Limit};
    DtypeLowering lowering{source,limits,{}};
    const auto valid=lowering.lower();if(!valid)return valid;
    out=std::move(lowering.candidate);return {};
}
ProcessingDiagnostic lower_routing_points(const NumericProcessingGraph& source,const ProcessingAdmissionLimits& limits,
    NumericProcessingGraph& out,std::vector<RoutingAlias>& aliases) {
    if(source.nodes.size()>limits.max_nodes || source.edges.size()>limits.max_edges)return {ProcessingError::Limit};
    RoutingLowering lowering{source,limits,{},{}};
    const auto valid=lowering.lower();if(!valid)return valid;
    out=std::move(lowering.candidate);aliases=std::move(lowering.aliases);return {};
}
namespace {
struct NodeResolution {
    const ConfiguredProcessingNode& configured;
    const ProcessingAdmissionLimits& limits;
    std::span<const ProcessingPort> inputs,outputs;
    std::optional<NodeKind> kind;
    NodeRole role=NodeRole::Processor;
    ProcessingOperation operation=ProcessingOperation::Converter;
    std::size_t input_count=1,output_count=1;
    ProcessingDiagnostic error(ProcessingError code,PortId port=0) const {return {code,configured.id,port};}
};
ProcessingDiagnostic inspect_parameter_port(const NodeResolution& state,const ProcessingPort& port) {
    const auto key=numeric_parameter_key(state.configured.kind,port.id);
    const auto data=port.contract.view();const auto* scalar=std::get_if<NumericContract>(&data);
    const auto dtype=port.id==coherent_gain_input || port.id==power_gain_input || port.id==frequency_step_input?DType::Float64:DType::UInt32;
    if(key.empty() || port.direction!=PortDirection::Input || !scalar || scalar->dtype!=dtype || scalar->shape.rank!=Rank::Scalar || scalar->channel_order.size()!=1)return state.error(ProcessingError::InvalidPort,port.id);
    if(scalar->representation.encoding!=StorageEncoding::Native || scalar->representation.normalization!=Normalization::None || validate_contract(data,state.limits.frame)!=Status::ok)return state.error(ProcessingError::InvalidPort,port.id);
    return {};
}
bool has_port_id(std::span<const ProcessingPort> ports,PortId id) {
    return std::any_of(ports.begin(),ports.end(),[=](const auto& port){return port.id==id;});
}
ProcessingDiagnostic inspect_parameter_ports(NodeResolution& state) {
    state.inputs=state.configured.inputs;state.outputs=data_outputs(state.configured);
    const auto first=std::find_if(state.inputs.begin(),state.inputs.end(),[&](const auto& port){return !numeric_parameter_key(state.configured.kind,port.id).empty();});
    if(first==state.inputs.end())return {};
    for(auto current=first;current!=state.inputs.end();++current) {
        const auto result=inspect_parameter_port(state,*current);if(!result)return result;
        if(has_port_id(std::span(first,current),current->id))return state.error(ProcessingError::InvalidPort,current->id);
    }
    state.inputs=state.inputs.first(std::size_t(first-state.inputs.begin()));return {};
}
ProcessingDiagnostic inspect_auxiliary_ports(const NodeResolution& state) {
    const auto& configured=state.configured;
    for(const auto& port:std::span<const ProcessingPort>(configured.outputs).subspan(state.outputs.size())) {
        const auto data=port.contract.view();const auto* scalar=std::get_if<NumericContract>(&data);
        const auto dtype=has_window_gain_output(configured.kind)?DType::Float64:DType::UInt32;
        if(port.direction!=PortDirection::Output || !scalar || scalar->dtype!=dtype || scalar->shape.rank!=Rank::Scalar || scalar->channel_order.size()!=1)return state.error(ProcessingError::InvalidPort,port.id);
        if(validate_contract(data,state.limits.frame)!=Status::ok)return state.error(ProcessingError::InvalidContract,port.id);
    }
    return {};
}
ProcessingDiagnostic inspect_owned_port(const NodeResolution& state,const ProcessingPort& port,PortDirection direction,std::size_t& metadata) {
        if(port.direction!=direction || !port.contract)return state.error(ProcessingError::InvalidPort,port.id);
        const auto data=port.contract.view();
        if(validate_contract(data,state.limits.frame)!=Status::ok)return state.error(ProcessingError::InvalidContract,port.id);
        if(const auto* numeric=std::get_if<NumericContract>(&data);numeric && numeric->channel_order.size()!=1)return state.error(ProcessingError::InvalidContract,port.id);
        if(!add(metadata,port.contract.owned_bytes()) || metadata>state.limits.max_owned_metadata_bytes)return state.error(ProcessingError::Limit,port.id);
    return {};
}
ProcessingDiagnostic inspect_port_list(const NodeResolution& state,std::span<const ProcessingPort> ports,PortDirection direction,std::size_t& metadata) {
    for(std::size_t i=0;i<ports.size();++i) {
        const auto& port=ports[i];
        if(port.direction!=direction || !port.contract)return state.error(ProcessingError::InvalidPort,port.id);
        if(has_port_id(ports.first(i),port.id))return state.error(ProcessingError::InvalidPort,port.id);
        const auto valid=inspect_owned_port(state,port,direction,metadata);if(!valid)return valid;
    }
    return {};
}
ProcessingDiagnostic inspect_kind(NodeResolution& state) {
    const auto& node=state.configured;
    if(node.id<0)return state.error(ProcessingError::InvalidNode);
    if(node.kind.empty() || node.kind.size()>state.limits.max_text_bytes)return state.error(ProcessingError::Limit);
    state.kind=node_kind(node.kind);
    if(node.kind!="Converter") {
        if(!state.kind)return state.error(ProcessingError::UnknownKind);
        state.role=node_role(*state.kind);state.operation=operation(*state.kind,state.role);
    }
    return {};
}
ProcessingDiagnostic inspect_identity(NodeResolution& state) {
    const auto identity=inspect_kind(state);if(!identity)return identity;
    const auto& node=state.configured;
    if(node.parameters.valueless_by_exception())return state.error(ProcessingError::InvalidParameters);
    std::size_t footprint=0;
    if(!detail::processing_node_owned_bytes(node,state.limits,footprint) || node.inputs.size()>state.limits.max_ports_per_node || node.outputs.size()>state.limits.max_ports_per_node)return state.error(ProcessingError::Limit);
    return {};
}
ProcessingDiagnostic inspect_ports(NodeResolution& state) {
    auto result=inspect_parameter_ports(state);if(!result)return result;
    result=inspect_auxiliary_ports(state);if(!result)return result;
    std::size_t metadata=0;
    result=inspect_port_list(state,state.inputs,PortDirection::Input,metadata);if(!result)return result;
    return inspect_port_list(state,state.configured.outputs,PortDirection::Output,metadata);
}
ProcessingDiagnostic route_arity(NodeResolution& state) {
    const auto* route=std::get_if<RouteProcessingParameters>(&state.configured.parameters);
    if(state.kind==NodeKind::Reroute && (!route || !route->open || route->outputs!=1 || route->selected || !state.configured.enabled))return state.error(ProcessingError::InvalidParameters);
    if(!route || (state.operation==ProcessingOperation::Gate?route->outputs!=1:(route->outputs<min_route_outputs || route->outputs>max_route_outputs)) || route->selected>=route->outputs)return state.error(ProcessingError::InvalidParameters);
    if(route->channels_per_stream<1 || route->channels_per_stream>max_route_channels || (state.operation!=ProcessingOperation::Switch && route->channels_per_stream!=1))return state.error(ProcessingError::InvalidParameters);
    state.input_count=route->channels_per_stream;state.output_count=std::size_t(route->outputs)*route->channels_per_stream;
    return {};
}
void paired_arity(NodeResolution& state) {
    const auto op=state.operation;
    if(op==ProcessingOperation::WLEDEncoder)state.input_count=3;
    if(op==ProcessingOperation::SpectrumPeak)state.output_count=2;
    if(op==ProcessingOperation::Fourier){state.input_count=state.kind==NodeKind::Fourier?2:1;state.output_count=4;}
    if(op==ProcessingOperation::ComplexPolar){state.input_count=2;state.output_count=2;}
}
void fixed_arity(NodeResolution& state) {
    const auto op=state.operation;
    state.input_count=state.role==NodeRole::Source?0:1;state.output_count=state.role==NodeRole::Sink?0:1;
    if(op==ProcessingOperation::Clock){state.input_count=state.output_count=0;return;}
    if(op==ProcessingOperation::ChannelComparison || op==ProcessingOperation::ArrayConcat)state.input_count=2;
    paired_arity(state);
    if(state.kind==NodeKind::MusicalSpectrogram || state.kind==NodeKind::NoteSpectrum)state.output_count=4;
    if(op==ProcessingOperation::History)state.output_count=2;
}
ProcessingDiagnostic selector_arity(NodeResolution& state) {
    const bool scalar=state.kind && is_scalar_view(*state.kind);
    if(state.operation!=ProcessingOperation::InputSelector && !scalar)return {};
    state.input_count=state.inputs.size();
    if(!state.input_count || state.input_count>(scalar?8:65536))return state.error(ProcessingError::InvalidPort);
    if(state.operation==ProcessingOperation::InputSelector)state.output_count=state.outputs.size();
    return {};
}
ProcessingDiagnostic variable_arity(NodeResolution& state) {
    const auto op=state.operation;
    if(op==ProcessingOperation::Mixer || op==ProcessingOperation::ArrayProduct) {
        state.input_count=state.inputs.size();
        if(state.input_count<(op==ProcessingOperation::Mixer?1:2) || state.input_count>max_variadic_inputs)return state.error(ProcessingError::InvalidPort);
    }
    if(state.kind==NodeKind::AudioSource){state.output_count=state.outputs.size();if(!state.output_count)return state.error(ProcessingError::InvalidPort);}
    if(op==ProcessingOperation::Splitter || op==ProcessingOperation::Switch || op==ProcessingOperation::Gate)return route_arity(state);
    return selector_arity(state);
}
bool sparse_input_ports(const NodeResolution& state) {
    return state.operation==ProcessingOperation::InputSelector || state.operation==ProcessingOperation::Mixer || state.operation==ProcessingOperation::ArrayProduct || (state.kind && is_scalar_view(*state.kind));
}
ProcessingDiagnostic inspect_dense_ports(const NodeResolution& state) {
    if(!sparse_input_ports(state))for(std::size_t i=0;i<state.input_count;++i)if(!find_port(state.inputs,PortId(i)))return state.error(ProcessingError::InvalidPort,PortId(i));
    for(std::size_t i=0;i<state.output_count;++i)if(!find_port(state.outputs,PortId(i)))return state.error(ProcessingError::InvalidPort,PortId(i));
    return {};
}
ProcessingDiagnostic inspect_arity(NodeResolution& state) {
    fixed_arity(state);
    const auto result=variable_arity(state);if(!result)return result;
    if(state.inputs.size()!=state.input_count || state.outputs.size()!=state.output_count)return state.error(ProcessingError::InvalidPort);
    return inspect_dense_ports(state);
}
ProcessingDiagnostic inspect_operation(const NodeResolution& state) {
    const auto& node=state.configured;
    const detail::NodeValidationView view{node.id,node.kind,node.parameters,state.inputs,state.outputs,node.enabled,node.clock_node};
    DataContract input=NumericContract{},output=NumericContract{};
    if(state.input_count) {
        input=(sparse_input_ports(state)?&state.inputs.front():find_port(state.inputs,0))->contract.view();
    }
    if(state.output_count)output=find_port(state.outputs,0)->contract.view();
    const detail::OperationValidation context{node,state.limits,view,state.kind,state.role,state.operation,state.input_count,state.output_count,input,output,std::get_if<NumericContract>(&input),std::get_if<NumericContract>(&output)};
    return detail::validate_operation(context);
}
}
ProcessingDiagnostic resolve_processing_node(const ConfiguredProcessingNode& configured,const ProcessingAdmissionLimits& limits,ResolvedProcessingNode& out) noexcept {
    NodeResolution state{configured,limits};
    auto result=inspect_identity(state);if(!result)return result;
    result=inspect_ports(state);if(!result)return result;
    result=inspect_arity(state);if(!result)return result;
    result=inspect_operation(state);if(!result)return result;
    out={configured.id,configured.kind,state.role,state.operation,state.inputs,state.outputs,&configured.parameters,configured.enabled};return {};
}

}
