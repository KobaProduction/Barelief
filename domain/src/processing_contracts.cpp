#include <domain/processing_contracts.h>
#include <algorithm>
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
bool mul(std::size_t a,std::size_t b,std::size_t& out) noexcept {
    if(a && b>std::numeric_limits<std::size_t>::max()/a)return false;
    out=a*b;return true;
}
bool clock_equal(const ClockContract& a,const ClockContract& b) noexcept {
    return a.domain==b.domain && a.tick_seconds==b.tick_seconds && a.sample_rate==b.sample_rate;
}
bool policy_structural(const ConversionPolicy& p) noexcept {
    return (p.rounding==Rounding::TowardZero || p.rounding==Rounding::NearestTiesEven) &&
        (p.overflow==Overflow::Reject || p.overflow==Overflow::Saturate) &&
        (p.precision_loss==PrecisionLoss::RejectInexact || p.precision_loss==PrecisionLoss::AllowDeclared) &&
        (p.nonfinite==NonFinite::Reject || p.nonfinite==NonFinite::ReplaceFinite || p.nonfinite==NonFinite::ClampFinite) &&
        (p.scaling.mode==ScaleMode::Identity || p.scaling.mode==ScaleMode::IntegerAffine || p.scaling.mode==ScaleMode::DyadicAffine ||
         p.scaling.mode==ScaleMode::PcmNormalize || p.scaling.mode==ScaleMode::PcmDenormalize || p.scaling.mode==ScaleMode::Binary64Affine) &&
        validate_literal(p.scaling.gain.numerator)==Status::ok && validate_literal(p.scaling.offset.numerator)==Status::ok &&
        validate_literal(p.nan_replacement)==Status::ok && validate_literal(p.negative_inf_replacement)==Status::ok && validate_literal(p.positive_inf_replacement)==Status::ok;
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
    return std::get<std::string>(a)==std::get<std::string>(b);
}
bool parameters_equal(const std::vector<ProcessingParameter>& a,const std::vector<ProcessingParameter>& b) noexcept {
    if(a.size()!=b.size())return false;
    for(const auto& x:a){const ProcessingParameter* found=nullptr;
        for(const auto& y:b)if(x.key==y.key){if(found)return false;found=&y;}
        if(!found || !parameter_equal(x.value,found->value))return false;}
    for(std::size_t i=0;i<a.size();++i)for(std::size_t j=0;j<i;++j)if(a[i].key==a[j].key)return false;
    return true;
}
bool parameter_value_valid(const ProcessingParameterValue& value,const ParameterDescriptor& d) noexcept {
    const auto* numeric=std::get_if<NumericLiteral>(&value);
    switch(d.value_kind) {
    case ParameterValueKind::Boolean:return std::holds_alternative<bool>(value);
    case ParameterValueKind::String:
        if(const auto* text=std::get_if<std::string>(&value))return !text->empty() && text->find('\0')==std::string::npos;
        return false;
    case ParameterValueKind::Integer:
        // Existing registered integer bounds are nonnegative and <=192000.
        if(!numeric)return false;
        if(const auto* v=std::get_if<std::uint64_t>(numeric))return *v>=std::uint64_t(d.minimum) && *v<=std::uint64_t(d.maximum);
        if(const auto* v=std::get_if<std::int64_t>(numeric))return *v>=std::int64_t(d.minimum) && *v<=std::int64_t(d.maximum);
        return false;
    case ParameterValueKind::Number:
        if(!numeric || validate_literal(*numeric)!=Status::ok)return false;
        // Compare integer literals in their own type, never narrowing through double.
        return std::visit([&](auto v){using T=decltype(v);
            if constexpr(std::is_floating_point_v<T>)return std::isfinite(v) && v>=d.minimum && v<=d.maximum;
            else if constexpr(std::is_signed_v<T>)return v>=0 && std::uint64_t(v)>=std::uint64_t(std::ceil(d.minimum)) && std::uint64_t(v)<=std::uint64_t(std::floor(d.maximum));
            else return v>=std::uint64_t(std::ceil(d.minimum)) && v<=std::uint64_t(std::floor(d.maximum));
        },*numeric);
    case ParameterValueKind::WindowFunction:
        if(const auto* s=std::get_if<std::string>(&value))return *s=="hann" || *s=="hamming" || *s=="blackman" || *s=="rectangular";
        return false;
    default:return false;
    }
}
bool parameter_fields_valid(NodeKind k,const std::vector<ProcessingParameter>& values,const ProcessingAdmissionLimits& limits) noexcept {
    if(values.size()>limits.max_parameters_per_node)return false;
    for(std::size_t i=0;i<values.size();++i){const auto& p=values[i];
        if(p.key.size()>limits.max_text_bytes || p.key=="enabled")return false;
        for(std::size_t j=0;j<i;++j)if(p.key==values[j].key)return false;
        const auto* d=parameter_descriptor(k,p.key);
        if(!d || d->classification!=ParameterClassification::Processing || !parameter_value_valid(p.value,*d))return false;
        if(const auto* s=std::get_if<std::string>(&p.value);s && s->size()>limits.max_text_bytes)return false;
    }return true;
}
ProcessingOperation operation(NodeKind k,NodeRole role) noexcept {
    if(role==NodeRole::Source)return ProcessingOperation::Source;
    if(role==NodeRole::Sink)return ProcessingOperation::Sink;
    switch(k) {
    case NodeKind::Splitter:return ProcessingOperation::Splitter;
    case NodeKind::Switch:return ProcessingOperation::Switch;
    case NodeKind::InputSelector:return ProcessingOperation::InputSelector;
    case NodeKind::Gate:return ProcessingOperation::Gate;
    case NodeKind::ScalarReduce:return ProcessingOperation::Reduce;
    case NodeKind::IndexExtract:return ProcessingOperation::Extract;
    case NodeKind::Spectrogram:return ProcessingOperation::History;
    case NodeKind::WLEDEncode:return ProcessingOperation::WLEDEncoder;
    default:return ProcessingOperation::Transform;
    }
}
bool wled_contract(const DataContract& c) noexcept {
    const auto* s=std::get_if<StructuredContract>(&c);
    return s && s->schema=="wled-audio-sync" && s->version==1 && s->encoding=="audio-sync-44-le" && s->payload_bytes==ExtentBounds{44,44};
}
const ProcessingPort* find_port(std::span<const ProcessingPort> ports,PortId id) noexcept {
    for(const auto& p:ports)if(p.id==id)return &p;
    return nullptr;
}
std::size_t node_index(const NumericProcessingGraph& g,NodeId id) noexcept {
    for(std::size_t i=0;i<g.nodes.size();++i)if(g.nodes[i].id==id)return i;
    return g.nodes.size();
}
bool resource_add(ProcessingResources& a,const ProcessingResources& b) noexcept {
    if(!add(a.plan_bytes,b.plan_bytes) || !add(a.payload_bytes,b.payload_bytes) || !add(a.scratch_bytes,b.scratch_bytes))return false;
    a.stack_bytes=std::max(a.stack_bytes,b.stack_bytes);return true;
}
bool within(const ProcessingResources& a,const ProcessingResources& max) noexcept {
    return a.plan_bytes<=max.plan_bytes && a.payload_bytes<=max.payload_bytes && a.scratch_bytes<=max.scratch_bytes && a.stack_bytes<=max.stack_bytes;
}
bool node_footprint(const ConfiguredProcessingNode& n,const ProcessingAdmissionLimits& limits,std::size_t& bytes) noexcept {
    bytes=sizeof(ConfiguredProcessingNode);std::size_t amount=0;
    const auto charge=[&](std::size_t count,std::size_t width){return mul(count,width,amount) && add(bytes,amount) && bytes<=limits.max_owned_metadata_bytes;};
    if(!charge(n.kind.capacity(),1) || !charge(1,1) || !charge(n.inputs.capacity(),sizeof(ProcessingPort)) || !charge(n.outputs.capacity(),sizeof(ProcessingPort)))return false;
    for(const auto* ports:{&n.inputs,&n.outputs})for(const auto& p:*ports)if(!charge(p.contract.owned_bytes(),1))return false;
    const std::vector<ProcessingParameter>* fields=nullptr;
    if(const auto* p=std::get_if<EndpointProcessingParameters>(&n.parameters))fields=&p->values;
    if(const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters))fields=&p->values;
    if(fields){
        if(fields->size()>limits.max_parameters_per_node || !charge(fields->capacity(),sizeof(ProcessingParameter)))return false;
        for(const auto& p:*fields){if(p.key.size()>limits.max_text_bytes || !charge(p.key.capacity(),1) || !charge(1,1))return false;
            if(const auto* s=std::get_if<std::string>(&p.value);s && (s->size()>limits.max_text_bytes || !charge(s->capacity(),1) || !charge(1,1)))return false;
        }
    }
    if(const auto* p=std::get_if<ReduceProcessingParameters>(&n.parameters))
        if(p->selected_channels.size()>limits.frame.max_planes || !charge(p->selected_channels.capacity(),sizeof(std::uint32_t)))return false;
    return true;
}
}
struct OwnedProcessingContract::Storage {
    std::atomic<std::size_t> references{1};DataContract value;
    std::unique_ptr<std::uint32_t[]> channels;std::unique_ptr<char[]> text;
    std::size_t channel_count=0,first_text=0,second_text=0,bytes=0;
};
OwnedProcessingContract::OwnedProcessingContract(const OwnedProcessingContract& other) noexcept:storage_(other.storage_) {
    if(storage_)storage_->references.fetch_add(1,std::memory_order_relaxed);
}
OwnedProcessingContract& OwnedProcessingContract::operator=(const OwnedProcessingContract& other) noexcept {
    if(this!=&other){OwnedProcessingContract candidate(other);std::swap(storage_,candidate.storage_);}return *this;
}
OwnedProcessingContract::OwnedProcessingContract(OwnedProcessingContract&& other) noexcept:storage_(std::exchange(other.storage_,nullptr)) {}
OwnedProcessingContract& OwnedProcessingContract::operator=(OwnedProcessingContract&& other) noexcept {
    if(this!=&other){release();storage_=std::exchange(other.storage_,nullptr);}return *this;
}
OwnedProcessingContract::~OwnedProcessingContract(){release();}
void OwnedProcessingContract::release() noexcept {
    if(storage_ && storage_->references.fetch_sub(1,std::memory_order_acq_rel)==1)delete storage_;
    storage_=nullptr;
}
DataContract OwnedProcessingContract::view() const noexcept {
    if(!storage_)return NumericContract{};
    auto value=storage_->value;
    if(auto* c=std::get_if<NumericContract>(&value)) {
        c->channel_order={storage_->channels.get(),storage_->channel_count};c->clock.domain={storage_->text.get(),storage_->first_text};
    }else{
        auto& s=std::get<StructuredContract>(value);s.schema={storage_->text.get(),storage_->first_text};
        s.encoding={storage_->text.get()+storage_->first_text,storage_->second_text};
    }return value;
}
std::size_t OwnedProcessingContract::owned_bytes() const noexcept {return storage_?storage_->bytes:0;}
Status own_processing_contract(const DataContract& source,const FrameLimits& limits,std::size_t maximum,OwnedProcessingContract& out) noexcept {
    const auto status=validate_contract(source,limits);if(status!=Status::ok)return status;
    std::size_t bytes=sizeof(OwnedProcessingContract::Storage),channel_bytes=0,text_bytes=0,channel_count=0,first=0,second=0;
    if(const auto* c=std::get_if<NumericContract>(&source)){channel_count=c->channel_order.size();first=c->clock.domain.size();}
    else{const auto& s=std::get<StructuredContract>(source);first=s.schema.size();second=s.encoding.size();}
    text_bytes=first;
    if(!add(text_bytes,second) || !mul(channel_count,sizeof(std::uint32_t),channel_bytes) || !add(bytes,channel_bytes) || !add(bytes,text_bytes))return Status::overflow;
    std::size_t peak=bytes;if(!add(peak,out.owned_bytes()))return Status::overflow;
    if(peak>maximum)return Status::buffer_too_small;
    std::unique_ptr<OwnedProcessingContract::Storage> storage(new(std::nothrow) OwnedProcessingContract::Storage);
    if(!storage)return Status::exhausted;
    if(channel_count){storage->channels.reset(new(std::nothrow) std::uint32_t[channel_count]);if(!storage->channels)return Status::exhausted;}
    if(text_bytes){storage->text.reset(new(std::nothrow) char[text_bytes]);if(!storage->text)return Status::exhausted;}
    storage->value=source;storage->bytes=bytes;storage->channel_count=channel_count;storage->first_text=first;storage->second_text=second;
    if(const auto* c=std::get_if<NumericContract>(&source)) {
        std::copy(c->channel_order.begin(),c->channel_order.end(),storage->channels.get());
        if(first)std::memcpy(storage->text.get(),c->clock.domain.data(),first);
        auto& target=std::get<NumericContract>(storage->value);target.channel_order={};target.clock.domain={};
    }else{
        const auto& s=std::get<StructuredContract>(source);
        if(first)std::memcpy(storage->text.get(),s.schema.data(),first);
        if(second)std::memcpy(storage->text.get()+first,s.encoding.data(),second);
        auto& target=std::get<StructuredContract>(storage->value);target.schema={};target.encoding={};
    }
    OwnedProcessingContract candidate;candidate.storage_=storage.release();out=std::move(candidate);return Status::ok;
}
Status processing_versions_supported(const ProcessingVersions& v) noexcept {
    return v.graph==3 && v.processing==2?Status::ok:Status::unsupported_version;
}
Status processing_boundary_version_supported(ProcessingBoundary boundary,std::uint32_t edition) noexcept {
    switch(boundary){
    case ProcessingBoundary::Document:return edition==2?Status::ok:Status::unsupported_version;
    case ProcessingBoundary::Control:return edition==2?Status::ok:Status::unsupported_version;
    case ProcessingBoundary::Data:return edition==4?Status::ok:Status::unsupported_version;
    }
    return Status::invalid_argument;
}
bool same_processing_parameters(const ProcessingParameters& a,const ProcessingParameters& b) noexcept {
    if(a.valueless_by_exception() || b.valueless_by_exception() || a.index()!=b.index())return false;
    return std::visit([&](const auto& x){using T=std::decay_t<decltype(x)>;const auto& y=std::get<T>(b);
        if constexpr(std::is_same_v<T,EndpointProcessingParameters> || std::is_same_v<T,TransformProcessingParameters>)return parameters_equal(x.values,y.values);
        else if constexpr(std::is_same_v<T,ConverterProcessingParameters>)return policy_equal(x.policy,y.policy);
        else if constexpr(std::is_same_v<T,RouteProcessingParameters>)return x.outputs==y.outputs && x.selected==y.selected && x.open==y.open;
        else if constexpr(std::is_same_v<T,HistoryProcessingParameters>)return x.capacity==y.capacity;
        else return x.operation==y.operation && x.channels==y.channels && x.rows==y.rows && x.selected_channels==y.selected_channels && x.precision_loss==y.precision_loss && x.rounding==y.rounding;
    },a);
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
ProcessingDiagnostic lower_dtype_edges(const NumericProcessingGraph& source,const ProcessingAdmissionLimits& limits,NumericProcessingGraph& out) {
    if(source.nodes.size()>limits.max_nodes || source.edges.size()>limits.max_edges)return {ProcessingError::Limit};
    std::size_t footprint=0;if(processing_graph_owned_bytes(source,limits,footprint)!=Status::ok)return {ProcessingError::Limit};
    NumericProcessingGraph candidate=source;candidate.edges.clear();
    std::vector<std::size_t> order(source.edges.size());std::iota(order.begin(),order.end(),0);
    std::sort(order.begin(),order.end(),[&](auto x,auto y){const auto& a=source.edges[x];const auto& b=source.edges[y];
        return std::tie(a.to,a.input_port,a.from,a.output_port)<std::tie(b.to,b.input_port,b.from,b.output_port);});
    for(const auto i:order){const auto& edge=source.edges[i];
        if(edge.conversion>3)return {ProcessingError::InvalidParameters,edge.to,edge.input_port,i};
        const auto from=node_index(source,edge.from),to=node_index(source,edge.to);
        if(from==source.nodes.size() || to==source.nodes.size())return {ProcessingError::MissingEndpoint,-1,0,i};
        const auto* a=find_port(source.nodes[from].outputs,edge.output_port);const auto* b=find_port(source.nodes[to].inputs,edge.input_port);
        if(!a || !b)return {ProcessingError::MissingEndpoint,edge.to,edge.input_port,i};
        if(processing_ports_compatible(*a,*b)){auto copy=edge;copy.conversion=0;candidate.edges.push_back(copy);continue;}
        if(!processing_ports_dtype_convertible(*a,*b))return {ProcessingError::IncompatiblePorts,edge.to,edge.input_port,i};
        if(candidate.nodes.size()>=limits.max_nodes || candidate.edges.size()+2>limits.max_edges)return {ProcessingError::Limit};
        // Edges are sorted by stable target port before assigning internal IDs.
        NodeId id=std::numeric_limits<NodeId>::max();
        while(std::any_of(candidate.nodes.begin(),candidate.nodes.end(),[id](const auto& n){return n.id==id;})){if(id==0)return {ProcessingError::Limit};--id;}
        ConfiguredProcessingNode converter;converter.id=id;converter.kind="Converter";
        converter.parameters=ConverterProcessingParameters{edge_conversion_policy(edge.conversion)};
        converter.inputs.push_back({0,PortDirection::Input,a->contract});converter.outputs.push_back({0,PortDirection::Output,b->contract});
        candidate.nodes.push_back(std::move(converter));
        candidate.edges.push_back({edge.from,id,edge.output_port,0});candidate.edges.push_back({id,edge.to,0,edge.input_port});
    }
    if(candidate.nodes.size()==source.nodes.size())candidate=source;
    out=std::move(candidate);return {};
}
Status resolve_processing_parameter(const ResolvedProcessingNode& n,std::string_view key,ProcessingParameterView& out) noexcept {
    const auto kind=node_kind(n.kind);if(!kind || !n.parameters)return Status::invalid_argument;
    const auto* descriptor=parameter_descriptor(*kind,key);
    if(!descriptor || descriptor->classification!=ParameterClassification::Processing || key=="enabled" || descriptor->value_kind==ParameterValueKind::FrameType)return Status::invalid_argument;
    const std::vector<ProcessingParameter>* fields=nullptr;
    if(const auto* p=std::get_if<EndpointProcessingParameters>(n.parameters))fields=&p->values;
    if(const auto* p=std::get_if<TransformProcessingParameters>(n.parameters))fields=&p->values;
    if(!fields)return Status::invalid_argument;
    for(const auto& p:*fields)if(p.key==key){
        if(const auto* v=std::get_if<NumericLiteral>(&p.value))out=*v;
        else if(const auto* v=std::get_if<bool>(&p.value))out=*v;
        else out=std::string_view(std::get<std::string>(p.value));
        return Status::ok;
    }
    switch(descriptor->value_kind) {
    case ParameterValueKind::Boolean:out=descriptor->default_number!=0;break;
    case ParameterValueKind::Integer:out=NumericLiteral{std::int64_t(descriptor->default_number)};break;
    case ParameterValueKind::Number:out=NumericLiteral{descriptor->default_number};break;
    case ParameterValueKind::WindowFunction:case ParameterValueKind::String:out=descriptor->default_text;break;
    default:return Status::invalid_argument;
    }return Status::ok;
}
namespace {
bool test_signal_rules(const ConfiguredProcessingNode& node) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Source,ProcessingOperation::Source,
        node.inputs,node.outputs,&node.parameters,node.enabled};
    const auto number=[&](std::string_view key,double& out){
        ProcessingParameterView field;
        if(resolve_processing_parameter(resolved,key,field)!=Status::ok)return false;
        const auto* value=std::get_if<NumericLiteral>(&field);if(!value)return false;
        // Per-field validation already bounded these values to <=192000 and finite.
        // Integer values in that range are exactly representable; the stored tag/bits remain intact.
        out=std::visit([](auto x){return static_cast<double>(x);},*value);return true;
    };
    double minimum=0,maximum=0,rate=0,sweep=0;
    return number("minimum_hz",minimum) && number("maximum_hz",maximum) &&
        number("sample_rate",rate) && number("sweep_depth",sweep) &&
        minimum<maximum && maximum*(1+sweep)<rate/2;
}
}
ProcessingDiagnostic resolve_processing_node(const ConfiguredProcessingNode& n,const ProcessingAdmissionLimits& limits,ResolvedProcessingNode& out) noexcept {
    const auto error=[&](ProcessingError e,PortId port=0){return ProcessingDiagnostic{e,n.id,port,0};};
    if(n.id<0)return error(ProcessingError::InvalidNode);
    if(n.kind.empty() || n.kind.size()>limits.max_text_bytes)return error(ProcessingError::Limit);
    auto k=node_kind(n.kind);NodeRole role=NodeRole::Processor;ProcessingOperation op=ProcessingOperation::Converter;
    if(n.kind!="Converter") {
        if(!k)return error(ProcessingError::UnknownKind);
        role=node_role(*k);op=operation(*k,role);
    }
    if(n.parameters.valueless_by_exception())return error(ProcessingError::InvalidParameters);
    if(n.inputs.size()>limits.max_ports_per_node || n.outputs.size()>limits.max_ports_per_node || n.outputs.size()>65536)return error(ProcessingError::Limit);
    std::size_t owned_footprint=0;if(!node_footprint(n,limits,owned_footprint))return error(ProcessingError::Limit);
    std::size_t metadata=0;
    for(auto direction:{PortDirection::Input,PortDirection::Output}){
        const auto& ports=direction==PortDirection::Input?n.inputs:n.outputs;
        for(std::size_t i=0;i<ports.size();++i){const auto& p=ports[i];
            if(p.direction!=direction || !p.contract)return error(ProcessingError::InvalidPort,p.id);
            for(std::size_t j=0;j<i;++j)if(p.id==ports[j].id)return error(ProcessingError::InvalidPort,p.id);
            if(validate_contract(p.contract.view(),limits.frame)!=Status::ok)return error(ProcessingError::InvalidContract,p.id);
            if(!add(metadata,p.contract.owned_bytes()) || metadata>limits.max_owned_metadata_bytes)return error(ProcessingError::Limit,p.id);
        }
    }
    std::size_t input_count=role==NodeRole::Source?0:1,output_count=role==NodeRole::Sink?0:1;
    if(op==ProcessingOperation::Splitter || op==ProcessingOperation::Switch || op==ProcessingOperation::Gate) {
        const auto* p=std::get_if<RouteProcessingParameters>(&n.parameters);
        if(!p || (op==ProcessingOperation::Gate?p->outputs!=1:(p->outputs<2 || p->outputs>8)) || p->selected>=p->outputs)return error(ProcessingError::InvalidParameters);
        output_count=p->outputs;
    }else if(op==ProcessingOperation::History)output_count=2;
    else if(op==ProcessingOperation::InputSelector || k==NodeKind::ScalarView) {
        input_count=n.inputs.size();
        if(!input_count || input_count>(k==NodeKind::ScalarView?8:65536))return error(ProcessingError::InvalidPort);
    }
    if(n.inputs.size()!=input_count || n.outputs.size()!=output_count)return error(ProcessingError::InvalidPort);
    for(std::size_t i=0;op!=ProcessingOperation::InputSelector && k!=NodeKind::ScalarView && i<input_count;++i)if(!find_port(n.inputs,PortId(i)))return error(ProcessingError::InvalidPort,PortId(i));
    for(std::size_t i=0;i<output_count;++i)if(!find_port(n.outputs,PortId(i)))return error(ProcessingError::InvalidPort,PortId(i));
    DataContract in=NumericContract{},result=NumericContract{};
    if(input_count)in=(op==ProcessingOperation::InputSelector || k==NodeKind::ScalarView?&n.inputs.front():find_port(n.inputs,0))->contract.view();
    if(output_count)result=find_port(n.outputs,0)->contract.view();
    const auto* input=std::get_if<NumericContract>(&in);const auto* output=std::get_if<NumericContract>(&result);
    switch(op) {
    case ProcessingOperation::Source:
    case ProcessingOperation::Sink: {
        const auto* p=std::get_if<EndpointProcessingParameters>(&n.parameters);
        if(!p || !k || !parameter_fields_valid(*k,p->values,limits))return error(ProcessingError::InvalidParameters);
        if(*k==NodeKind::TestSignal && !test_signal_rules(n))return error(ProcessingError::InvalidParameters);
        if(*k==NodeKind::WLEDOutput && !wled_contract(in))return error(ProcessingError::InvalidContract);
        // View kind selects presentation defaults, never a numeric data type.
        const bool numeric_endpoint=*k==NodeKind::AudioSource || *k==NodeKind::TestSignal ||
            *k==NodeKind::AudioView || *k==NodeKind::WindowView || *k==NodeKind::WindowedView ||
            *k==NodeKind::FFTView || *k==NodeKind::MelView || *k==NodeKind::SpectrogramView || *k==NodeKind::ScalarView;
        const auto* value=role==NodeRole::Source?output:input;
        if(numeric_endpoint && !value)return error(ProcessingError::InvalidContract);
        if(is_view(*k))for(const auto& port:n.inputs){const auto contract=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&contract);
            const auto rank=*k==NodeKind::ScalarView?Rank::Scalar:*k==NodeKind::SpectrogramView?Rank::Matrix:Rank::Vector;
            if(!numeric || numeric->shape.rank!=rank || (*k==NodeKind::ScalarView && numeric->channel_order.size()!=1))return error(ProcessingError::InvalidContract,port.id);}
        if(*k==NodeKind::AudioSource || (*k==NodeKind::TestSignal && output && output->clock.sample_rate)){
            const ResolvedProcessingNode configured{n.id,n.kind,role,op,n.inputs,n.outputs,&n.parameters,n.enabled};
            ProcessingParameterView rate;
            if(!output || resolve_processing_parameter(configured,"sample_rate",rate)!=Status::ok ||
                !std::holds_alternative<NumericLiteral>(rate) ||
                std::visit([](auto v){return double(v);},std::get<NumericLiteral>(rate))!=output->clock.sample_rate)
                return error(ProcessingError::IncompatiblePorts);
        }

        break;
    }
    case ProcessingOperation::InputSelector: {
        const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);
        if(!p || !parameter_fields_valid(*k,p->values,limits))return error(ProcessingError::InvalidParameters);
        if(!input || !output)return error(ProcessingError::InvalidContract);
        std::uint64_t selected=0;
        for(const auto& field:p->values)if(field.key=="selected_input")
            selected=std::visit([](auto v){return std::uint64_t(v);},std::get<NumericLiteral>(field.value));
        if(selected>65535 || !find_port(n.inputs,PortId(selected)))return error(ProcessingError::InvalidParameters);
        for(const auto& port:n.inputs)if(!same_physical_contract(in,port.contract.view()))
            return error(ProcessingError::IncompatiblePorts,port.id);
        if(!same_physical_contract(in,result))return error(ProcessingError::IncompatiblePorts);
        break;
    }
    case ProcessingOperation::Splitter:
    case ProcessingOperation::Switch:
    case ProcessingOperation::Gate:
        for(const auto& p:n.outputs)if(!same_physical_contract(in,p.contract.view()))return error(ProcessingError::IncompatiblePorts,p.id);
        break;
    case ProcessingOperation::Converter: {
        const auto* p=std::get_if<ConverterProcessingParameters>(&n.parameters);
        if(!p || !policy_structural(p->policy))return error(ProcessingError::InvalidParameters);
        if(!input || !output)return error(ProcessingError::InvalidContract);
        auto expected=*input;expected.semantic=output->semantic;expected.history_element=output->history_element;expected.dtype=output->dtype;expected.representation=output->representation;
        if(p->policy.scaling.mode==ScaleMode::PcmNormalize || p->policy.scaling.mode==ScaleMode::PcmDenormalize)expected.unit=output->unit;
        if(!same_physical_contract(expected,*output))return error(ProcessingError::IncompatiblePorts);
        break;
    }
    case ProcessingOperation::Extract: {
        const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);
        if(!p || !parameter_fields_valid(*k,p->values,limits) || !input || !output)return error(ProcessingError::InvalidParameters);
        const ResolvedProcessingNode configured{n.id,n.kind,role,op,n.inputs,n.outputs,&n.parameters,n.enabled};
        auto number=[&](const char* key){ProcessingParameterView value;resolve_processing_parameter(configured,key,value);return std::visit([](auto v){return std::uint64_t(v);},std::get<NumericLiteral>(value));};
        const auto channel=number("channel_index"),row=number("index"),column=number("column");
        if(input->shape.rank==Rank::Scalar || channel>=input->channel_order.size() || row>=input->shape.extents[0].maximum ||
            (input->shape.rank==Rank::Vector?column!=0:column>=input->shape.extents[1].maximum))return error(ProcessingError::InvalidParameters);
        auto expected=*input;expected.shape={};expected.coordinates={};expected.channel_order=input->channel_order.subspan(std::size_t(channel),1);
        if(!same_physical_contract(expected,*output))return error(ProcessingError::IncompatiblePorts);
        break;
    }
    case ProcessingOperation::Reduce: {
        const auto* p=std::get_if<ReduceProcessingParameters>(&n.parameters);
        if(!p || (p->operation!=NumericReduction::MaxAbs && p->operation!=NumericReduction::SignedMean && p->operation!=NumericReduction::RMS) ||
            (p->rows!=ReductionRows::WholeValue && p->rows!=ReductionRows::LatestHistoryRow) ||
            (p->channels!=ReductionChannels::AllChannels && p->channels!=ReductionChannels::BothChannels && p->channels!=ReductionChannels::SelectedChannels) ||
            (p->rounding!=Rounding::TowardZero && p->rounding!=Rounding::NearestTiesEven) ||
            (p->precision_loss!=PrecisionLoss::RejectInexact && p->precision_loss!=PrecisionLoss::AllowDeclared))return error(ProcessingError::InvalidParameters);
        if(!input || !output || output->shape.rank!=Rank::Scalar || output->channel_order.size()!=1 ||
            output->quantity!=input->quantity || output->unit!=input->unit || !clock_equal(output->clock,input->clock))return error(ProcessingError::InvalidContract);
        if(p->rows==ReductionRows::LatestHistoryRow && input->shape.rank!=Rank::Matrix)return error(ProcessingError::InvalidParameters);
        if(p->channels==ReductionChannels::BothChannels && input->channel_order.size()!=2)return error(ProcessingError::InvalidParameters);
        if(p->selected_channels.size()>limits.frame.max_planes || (p->channels==ReductionChannels::SelectedChannels?p->selected_channels.empty():!p->selected_channels.empty()))return error(ProcessingError::InvalidParameters);
        for(std::size_t i=0;i<p->selected_channels.size();++i){auto channel=p->selected_channels[i];
            if(std::find(input->channel_order.begin(),input->channel_order.end(),channel)==input->channel_order.end())return error(ProcessingError::InvalidParameters);
            for(std::size_t j=0;j<i;++j)if(channel==p->selected_channels[j])return error(ProcessingError::InvalidParameters);
        }
        if(p->operation!=NumericReduction::MaxAbs && output->dtype!=DType::Float32 && output->dtype!=DType::Float64)return error(ProcessingError::InvalidContract);
        break;
    }
    case ProcessingOperation::History: {
        const auto* p=std::get_if<HistoryProcessingParameters>(&n.parameters);
        if(!p || !p->capacity)return error(ProcessingError::InvalidParameters);
        if(!input || !output || input->shape.rank!=Rank::Vector || output->shape.rank!=Rank::Matrix)return error(ProcessingError::InvalidContract);
        // A retained matrix has one feature geometry for every row. Variable
        // widths must be resolved before accumulation, rather than leaving tails
        // from older rows visible in a full snapshot.
        if(input->shape.extents[0].minimum!=input->shape.extents[0].maximum)return error(ProcessingError::InvalidContract);
        // Local History receives potentially irregular source-clock timestamps.
        // Regular seconds would claim a cadence not guaranteed by processing.
        if(output->coordinates[0].mode==CoordinateMode::Regular && output->coordinates[0].unit==NumericUnit::Seconds)return error(ProcessingError::InvalidContract);
        auto expected=*input;expected.semantic=Semantic::History;expected.history_element=input->semantic;
        expected.shape.rank=Rank::Matrix;expected.shape.axes={Axis::Time,input->shape.axes[0]};
        expected.shape.extents={ExtentBounds{0,p->capacity},input->shape.extents[0]};expected.coordinates={output->coordinates[0],input->coordinates[0]};
        if(!same_physical_contract(expected,*output))return error(ProcessingError::IncompatiblePorts);
        auto count_value=find_port(n.outputs,filled_count_output)->contract.view();const auto* count=std::get_if<NumericContract>(&count_value);
        if(!count || count->dtype!=DType::UInt64 || count->shape.rank!=Rank::Scalar ||
            count->quantity!=Quantity::FilledFrameCount || count->unit!=NumericUnit::Frames || count->channel_order.size()!=1 ||
            count->representation.normalization!=Normalization::None || !clock_equal(count->clock,input->clock))return error(ProcessingError::InvalidContract,filled_count_output);
        break;
    }
    case ProcessingOperation::WLEDEncoder: {
        const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);
        if(!p || !k || !parameter_fields_valid(*k,p->values,limits))return error(ProcessingError::InvalidParameters);
        if(!input || !wled_contract(result))return error(ProcessingError::InvalidContract);
        break;
    }
    case ProcessingOperation::Transform: {
        const auto* p=std::get_if<TransformProcessingParameters>(&n.parameters);
        if(!p || !k || !parameter_fields_valid(*k,p->values,limits))return error(ProcessingError::InvalidParameters);
        if(!input || !output)return error(ProcessingError::InvalidContract);
        const bool unchanged=*k==NodeKind::WindowFunction || *k==NodeKind::SpectrumGain || *k==NodeKind::BarkWeighting;
        const bool input_shape=input->shape.rank==Rank::Vector || (*k==NodeKind::Window && input->shape.rank==Rank::Scalar);
        auto representation=input->representation;
        // FFT converts normalized time-domain PCM into unrestricted magnitudes.
        if(*k==NodeKind::StereoFFT && representation.normalization==Normalization::FloatUnit)
            representation.normalization=Normalization::None;
        if(!input_shape || output->shape.rank!=Rank::Vector ||
            input->dtype!=output->dtype || representation!=output->representation || (*k!=NodeKind::PcmResampler && !clock_equal(input->clock,output->clock)))return error(ProcessingError::IncompatiblePorts);
        if(*k!=NodeKind::SpectrumMerge && !std::equal(input->channel_order.begin(),input->channel_order.end(),output->channel_order.begin(),output->channel_order.end()))return error(ProcessingError::IncompatiblePorts);
        if(*k==NodeKind::PcmResampler){
            const ResolvedProcessingNode configured{n.id,n.kind,role,op,n.inputs,n.outputs,&n.parameters,n.enabled};
            ProcessingParameterView rate;
            if(resolve_processing_parameter(configured,"output_rate",rate)!=Status::ok || !std::holds_alternative<NumericLiteral>(rate) ||
                std::visit([](auto v){return double(v);},std::get<NumericLiteral>(rate))!=output->clock.sample_rate ||
                input->clock.domain!=output->clock.domain || input->clock.tick_seconds.numerator!=1 ||
                input->clock.tick_seconds.denominator!=input->clock.sample_rate || output->clock.tick_seconds.numerator!=1 ||
                output->clock.tick_seconds.denominator!=output->clock.sample_rate ||
                input->quantity!=output->quantity || input->unit!=output->unit ||
                input->shape.axes!=output->shape.axes || input->coordinates[0].mode!=CoordinateMode::None ||
                output->coordinates[0].mode!=CoordinateMode::None)return error(ProcessingError::IncompatiblePorts);
        }
        if(*k==NodeKind::SpectrumMerge){
            if(output->channel_order.size()!=1)return error(ProcessingError::InvalidContract);
            auto expected=*input;expected.semantic=output->semantic;expected.history_element=output->history_element;expected.channel_order=output->channel_order;
            if(!same_physical_contract(expected,*output))return error(ProcessingError::IncompatiblePorts);
        }
        if(unchanged){auto expected=*input;expected.semantic=output->semantic;expected.history_element=output->history_element;if(!same_physical_contract(expected,*output))return error(ProcessingError::IncompatiblePorts);}
        // Active geometry/FFT sizes/window values and implemented combinations are checked by the capability port.
        break;
    }
    }
    out={n.id,n.kind,role,op,n.inputs,n.outputs,&n.parameters,n.enabled};return {};
}

Status processing_graph_owned_bytes(const NumericProcessingGraph& g,const ProcessingAdmissionLimits& limits,std::size_t& out) noexcept {
    if(g.nodes.size()>limits.max_nodes || g.edges.size()>limits.max_edges)return Status::exhausted;
    std::size_t bytes=sizeof(NumericProcessingGraph),reserved=0;
    if(!mul(g.nodes.capacity()-g.nodes.size(),sizeof(ConfiguredProcessingNode),reserved) || !add(bytes,reserved) ||
       !mul(g.edges.capacity(),sizeof(Edge),reserved) || !add(bytes,reserved) || !add(bytes,g.graph_id.capacity()) || !add(bytes,1))return Status::overflow;
    for(const auto& node:g.nodes){std::size_t footprint=0;if(!node_footprint(node,limits,footprint) || !add(bytes,footprint))return Status::exhausted;}
    if(bytes>limits.max_owned_metadata_bytes)return Status::exhausted;
    out=bytes;return Status::ok;
}
ProcessingDiagnostic admit_processing_graph(const NumericProcessingGraph& g,const ProcessingAdmissionLimits& limits,
    const NumericDeviceCapabilities& capabilities,std::span<std::size_t> workspace,ProcessingGraphAdmission& out) noexcept {
    if(processing_versions_supported(g.versions)!=Status::ok)return {ProcessingError::UnsupportedVersion};
    if(!valid_execution_interval(g.execution_interval))return {ProcessingError::InvalidParameters};
    if(g.graph_id.empty() || g.graph_id.size()>limits.max_text_bytes || g.nodes.size()>limits.max_nodes || g.edges.size()>limits.max_edges)return {ProcessingError::Limit};
    std::size_t workspace_count=0;if(!mul(g.nodes.size(),2,workspace_count))return {ProcessingError::Limit};
    if(workspace.size()<workspace_count)return {ProcessingError::InsufficientWorkspace};
    std::size_t metadata=sizeof(NumericProcessingGraph),reserved=0;
    if(!mul(g.nodes.capacity()-g.nodes.size(),sizeof(ConfiguredProcessingNode),reserved) || !add(metadata,reserved) ||
        !mul(g.edges.capacity(),sizeof(Edge),reserved) || !add(metadata,reserved) || !add(metadata,g.graph_id.capacity()) || !add(metadata,1) ||
        metadata>limits.max_owned_metadata_bytes)return {ProcessingError::Limit};
    for(std::size_t i=0;i<g.nodes.size();++i){const auto& n=g.nodes[i];
        for(std::size_t j=0;j<i;++j)if(g.nodes[j].id==n.id)return {ProcessingError::InvalidNode,n.id};
        ResolvedProcessingNode resolved;auto diagnostic=resolve_processing_node(n,limits,resolved);if(!diagnostic)return diagnostic;
        if(resolved.operation==ProcessingOperation::Transform && n.kind=="Window"){
            const auto input=find_port(n.inputs,0)->contract.view();const auto& clock=std::get<NumericContract>(input).clock;
            const auto numerator=std::uint64_t(g.execution_interval.numerator),denominator=g.execution_interval.denominator;
            // At least one physical sample per interval; divide first so the
            // acceptance check itself cannot overflow a rate*duration product.
            const auto minimum_rate=denominator/numerator+std::uint64_t(denominator%numerator!=0);
            if(clock.sample_rate<minimum_rate)return {ProcessingError::InvalidParameters,n.id};
        }
        std::size_t footprint=0;if(!node_footprint(n,limits,footprint) || !add(metadata,footprint))return {ProcessingError::Limit,n.id};
        if(metadata>limits.max_owned_metadata_bytes)return {ProcessingError::Limit,n.id};
    }
    auto degree=workspace.first(g.nodes.size());auto order=workspace.subspan(g.nodes.size(),g.nodes.size());std::fill(degree.begin(),degree.end(),0);
    for(std::size_t i=0;i<g.edges.size();++i){const auto& e=g.edges[i];auto from=node_index(g,e.from),to=node_index(g,e.to);
        if(from==g.nodes.size() || to==g.nodes.size())return {ProcessingError::MissingEndpoint,-1,0,i};
        const auto* source=find_port(g.nodes[from].outputs,e.output_port);const auto* target=find_port(g.nodes[to].inputs,e.input_port);
        if(!source || !target)return {ProcessingError::MissingEndpoint,e.to,e.input_port,i};
        if(e.conversion>3 || (!processing_ports_compatible(*source,*target) && !processing_ports_dtype_convertible(*source,*target)))return {ProcessingError::IncompatiblePorts,e.to,e.input_port,i};
        for(std::size_t j=0;j<i;++j)if(g.edges[j].to==e.to && g.edges[j].input_port==e.input_port)return {ProcessingError::MultipleInputs,e.to,e.input_port,i};
        if(!add(degree[to],1))return {ProcessingError::Limit};
    }
    const auto visited=std::numeric_limits<std::size_t>::max();
    for(std::size_t position=0;position<g.nodes.size();++position){std::size_t next=g.nodes.size();
        for(std::size_t i=0;i<degree.size();++i)if(degree[i]==0){next=i;break;}
        if(next==g.nodes.size())return {ProcessingError::Cycle};
        order[position]=next;degree[next]=visited;
        for(const auto& e:g.edges)if(e.from==g.nodes[next].id){auto to=node_index(g,e.to);if(degree[to]!=visited)--degree[to];}
    }
    ProcessingGraphAdmission candidate;candidate.node_count=g.nodes.size();
    if(!g.nodes.empty() && !capabilities.query)return {ProcessingError::UnsupportedCombination};
    for(const auto& n:g.nodes){ResolvedProcessingNode resolved;auto diagnostic=resolve_processing_node(n,limits,resolved);if(!diagnostic)return diagnostic;
        auto support=capabilities.query(capabilities.context,resolved,capabilities.profile);
        if(!support.supported)return {ProcessingError::UnsupportedCombination,n.id};
        if(!resource_add(candidate.resources,support.resources) || !within(candidate.resources,capabilities.profile.maximum))return {ProcessingError::ResourceLimit,n.id};
    }
    std::size_t conversions=0;
    for(const auto& e:g.edges){const auto* a=find_port(g.nodes[node_index(g,e.from)].outputs,e.output_port);const auto* b=find_port(g.nodes[node_index(g,e.to)].inputs,e.input_port);
        if(processing_ports_compatible(*a,*b))continue;
        ++conversions;if(conversions>limits.max_nodes-g.nodes.size() || conversions>limits.max_edges-g.edges.size())return {ProcessingError::Limit,e.to};
        const ProcessingParameters parameters=ConverterProcessingParameters{edge_conversion_policy(e.conversion)};
        const std::array<ProcessingPort,1> inputs{{{0,PortDirection::Input,a->contract}}},outputs{{{0,PortDirection::Output,b->contract}}};
        const ResolvedProcessingNode resolved{e.to,"Converter",NodeRole::Processor,ProcessingOperation::Converter,inputs,outputs,&parameters,true};
        const auto support=capabilities.query(capabilities.context,resolved,capabilities.profile);
        if(!support.supported)return {ProcessingError::UnsupportedCombination,e.to,e.input_port};
        if(!resource_add(candidate.resources,support.resources) || !within(candidate.resources,capabilities.profile.maximum))return {ProcessingError::ResourceLimit,e.to};
        if(!add(metadata,sizeof(ConfiguredProcessingNode)+9+2*sizeof(ProcessingPort)+sizeof(Edge)) ||
           !add(metadata,a->contract.owned_bytes()) || !add(metadata,b->contract.owned_bytes()) || metadata>limits.max_owned_metadata_bytes)return {ProcessingError::Limit,e.to};
    }
    candidate.node_count+=conversions;
    candidate.peak_bytes=capabilities.profile.retained_working_bytes;
    std::size_t work_bytes=0;
    if(!mul(workspace_count,sizeof(std::size_t),work_bytes) || !add(candidate.peak_bytes,work_bytes) ||
        !add(candidate.peak_bytes,metadata) || !add(candidate.peak_bytes,candidate.resources.plan_bytes) ||
        !add(candidate.peak_bytes,candidate.resources.payload_bytes) || !add(candidate.peak_bytes,candidate.resources.scratch_bytes) ||
        !add(candidate.peak_bytes,candidate.resources.stack_bytes) || candidate.peak_bytes>capabilities.profile.max_peak_bytes)return {ProcessingError::ResourceLimit};
    out=candidate;return {};
}
}
