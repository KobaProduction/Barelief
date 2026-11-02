#include "operation_support.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace dsp_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
domain::ResolvedProcessingNode definition(const domain::ConfiguredProcessingNode& n) noexcept {
    domain::ResolvedProcessingNode r;r.id=n.id;r.kind=n.kind;r.parameters=&n.parameters;
    r.inputs=n.inputs;r.outputs=n.outputs;r.enabled=n.enabled;return r;
}
const domain::ProcessingParameterValue* parameter(const domain::ResolvedProcessingNode& n,std::string_view key) noexcept {
    const auto* p=n.parameters?std::get_if<domain::TransformProcessingParameters>(n.parameters):nullptr;
    if(p)for(const auto& entry:p->values)if(entry.key==key)return &entry.value;
    return nullptr;
}
bool number(const domain::ResolvedProcessingNode& n,std::string_view key,float fallback,float& out) noexcept {
    const auto* p=parameter(n,key);if(!p){out=fallback;return true;}
    const auto* v=std::get_if<NumericLiteral>(p);if(!v)return false;
    out=std::visit([](auto x){return float(x);},*v);return std::isfinite(out);
}
bool integer(const domain::ResolvedProcessingNode& n,std::string_view key,std::size_t fallback,std::size_t& out) noexcept {
    const auto* p=parameter(n,key);if(!p){out=fallback;return true;}
    const auto* v=std::get_if<NumericLiteral>(p);if(!v)return false;
    return std::visit([&](auto x){
        if constexpr(!std::is_integral_v<decltype(x)>)return false;
        else {if(!std::in_range<std::size_t>(x))return false;out=std::size_t(x);return true;}
    },*v);
}
bool flag(const domain::ResolvedProcessingNode& n,std::string_view key,bool fallback,bool& out) noexcept {
    const auto* p=parameter(n,key);if(!p){out=fallback;return true;}
    const auto* v=std::get_if<bool>(p);if(!v)return false;out=*v;return true;
}
bool feature_rate(const domain::ResolvedProcessingNode& node,float fallback,float& out) noexcept {
    float rate=0;std::size_t hop=0;
    if(!number(node,"sample_rate",0,rate))return false;
    if(!rate)return number(node,"frame_rate",fallback,out) && out>0;
    if(rate<0 || !integer(node,"hop_samples",512,hop) || !hop)return false;
    out=rate/float(hop);return std::isfinite(out) && out>0;
}
std::span<const float> array(const domain::ResolvedProcessingNode& n,std::string_view key) noexcept {
    const auto* p=parameter(n,key);const auto* v=p?std::get_if<domain::NumericArray>(p):nullptr;
    return v?std::span<const float>(v->values):std::span<const float>{};
}
std::string_view text(const domain::ResolvedProcessingNode& n,std::string_view key) noexcept {
    const auto* p=parameter(n,key);const auto* v=p?std::get_if<std::string>(p):nullptr;return v?std::string_view(*v):std::string_view{};
}
std::optional<NumericContract> contract(const domain::ResolvedProcessingNode& n,domain::PortDirection direction,domain::PortId id) noexcept {
    const auto ports=direction==domain::PortDirection::Input?n.inputs:n.outputs;
    for(const auto& p:ports)if(p.id==id){
        const auto value=p.contract.view();if(const auto* c=std::get_if<NumericContract>(&value))return *c;
    }
    return std::nullopt;
}
bool input_geometry(const domain::ResolvedProcessingNode& n,domain::PortId id,Rank rank,std::size_t& a,std::size_t& b) noexcept {
    const auto c=contract(n,domain::PortDirection::Input,id);
    if(!c || c->dtype!=DType::Float32 || c->shape.rank!=rank || c->channel_order.size()!=1 ||
       c->representation.encoding!=StorageEncoding::Native || c->representation.layout!=ChannelLayout::Planar || c->representation.valid_bits ||
       (c->representation.normalization!=Normalization::None && c->representation.normalization!=Normalization::FloatUnit))return false;
    if(c->shape.extents[0].maximum>SIZE_MAX || c->shape.extents[1].maximum>SIZE_MAX)return false;
    a=std::size_t(c->shape.extents[0].maximum);b=std::size_t(c->shape.extents[1].maximum);return a && b;
}
bool optional_vector(const domain::ResolvedProcessingNode& n,domain::PortId port,std::size_t maximum,bool scalar) noexcept {
    if(!contract(n,domain::PortDirection::Input,port))return true;
    std::size_t a=0,b=0;return input_geometry(n,port,Rank::Vector,a,b) && (a==maximum || (scalar && a==1));
}
namespace {
bool index_axis(const AxisCoordinates& axis) noexcept {
    if(axis.mode==CoordinateMode::None)return axis==AxisCoordinates{};
    if(axis.unit!=NumericUnit::Values && axis.unit!=NumericUnit::Frames)return false;
    if(axis.mode==CoordinateMode::ExplicitFloat64LE)return true;
    return axis.mode==CoordinateMode::Regular && axis.origin==Ratio{0,1} && axis.step==Ratio{1,1};
}
double setting(const domain::ResolvedProcessingNode& n,std::string_view key,float fallback) noexcept {
    float value=fallback;return number(n,key,fallback,value)?value:0;
}
double envelope_rate(const domain::ResolvedProcessingNode& n) noexcept {
    std::size_t hop=512;if(!integer(n,"hop_samples",512,hop) || !hop)return 0;
    return setting(n,"sample_rate",44100)/double(hop);
}
double configured_feature_rate(const domain::ResolvedProcessingNode& n) noexcept {
    float value=0;return feature_rate(n,44100.f/512,value)?double(value):0;
}
double basic_output_step(const domain::ResolvedProcessingNode& n,domain::PortId port) noexcept {
    const auto k=n.kind;
    if(k=="SpectralNovelty" && port==0)return 1/setting(n,"frame_rate",100);
    if(k=="PcmSpectralNovelty" && port==0)return setting(n,"hop_samples",441)/setting(n,"sample_rate",44100);
    if(k=="OnsetStrengthSequence")return 1/setting(n,"frame_rate",120);
    if(k=="DynamicBeatPath")return 1/configured_feature_rate(n);
    if(k=="LocalPulse" || k=="TempoRatios")return 1/setting(n,"frame_rate",44100.f/512);
    if(k=="PatternFeatures")return 1./50;
    return 0;
}
bool envelope_output(std::string_view kind,domain::PortId port) noexcept {
    if(kind=="PcmOnsetEnvelope")return true;
    if((kind=="PcmTempo" || kind=="PcmTempoRatios") && port==1)return true;
    if(kind=="PcmBeatPath" && port==2)return true;
    if(kind=="PcmLocalPulse" && port<2)return true;
    if(kind=="EnvelopeLocalPulse" && port==0)return true;
    return kind=="EnvelopeBeatPath" && port>=2;
}
double ratio_output_step(const domain::ResolvedProcessingNode& n,domain::PortId port) noexcept {
    const auto k=n.kind;
    if(k!="EnvelopeTempoRatios" && !(k=="PcmTempoRatios" && port==0) && !(k=="PcmTempogram" && port<2))return 0;
    std::size_t hop=1;if(!integer(n,"hop",1,hop))return 0;
    return double(hop)/(k.starts_with("Pcm")?envelope_rate(n):setting(n,"frame_rate",44100.f/512));
}

double output_time_step(const domain::ResolvedProcessingNode& n,domain::PortId port,std::size_t axis) noexcept {
    if(axis)return 0;
    if(const auto step=basic_output_step(n,port);step)return step;
    if(envelope_output(n.kind,port))return 1/envelope_rate(n);
    return ratio_output_step(n,port);
}
bool declared_axis(const domain::ResolvedProcessingNode& n,domain::PortId port,std::size_t axis,const AxisCoordinates& c) noexcept {
    if(index_axis(c))return true;
    const double step=output_time_step(n,port,axis);
    if(!std::isfinite(step) || step<=0 || c.unit!=NumericUnit::Seconds)return false;
    if(c.mode==CoordinateMode::ExplicitFloat64LE)return true;
    if(c.mode!=CoordinateMode::Regular || !c.origin.denominator || !c.step.denominator || c.origin.numerator)return false;
    const double actual=double(c.step.numerator)/double(c.step.denominator);
    return std::abs(actual-step)<=8*std::numeric_limits<double>::epsilon()*std::max(1.,step);
}
}
bool output_geometry(const domain::ResolvedProcessingNode& n,domain::PortId id,Rank rank,std::size_t a,std::size_t b,DType type) noexcept {
    const auto c=contract(n,domain::PortDirection::Output,id);
    return c && c->dtype==type && c->shape.rank==rank && c->channel_order.size()==1 &&
        c->shape.extents[0].maximum==a && c->shape.extents[1].maximum==b &&
        c->shape.extents[0].minimum==(rank==Rank::Scalar?1u:0u) && c->shape.extents[1].minimum==b &&
        c->clock.domain.empty() && !c->clock.sample_rate && c->clock.tick_seconds==Ratio{1,1} &&
        declared_axis(n,id,0,c->coordinates[0]) && declared_axis(n,id,1,c->coordinates[1]) &&
        c->representation.encoding==StorageEncoding::Native && c->representation.layout==ChannelLayout::Planar && !c->representation.valid_bits &&
        c->representation.normalization==Normalization::None;
}
const NumericOperationInput* input_at(std::span<const NumericOperationInput> inputs,domain::PortId id) noexcept {
    for(const auto& i:inputs)if(i.port==id)return &i;return nullptr;
}
const NumericOperationInput* available_input_at(std::span<const NumericOperationInput> inputs,domain::PortId id) noexcept {
    const auto* value=input_at(inputs,id);
    return value && value->status==Status::ok && value->stamp.available?value:nullptr;
}
NumericOperationOutput* output_at(std::span<NumericOperationOutput> outputs,domain::PortId id) noexcept {
    for(auto& o:outputs)if(o.port==id)return &o;return nullptr;
}
bool read_row(const void* context,std::size_t row,std::span<float> out) noexcept {
    const auto& i=*static_cast<const NumericOperationInput*>(context);
    if(row>=i.value.actual[0] || out.size()!=i.value.actual[1])return false;
    for(std::size_t k=0;k<out.size();++k)if(load_element(i.value,{0,row,k},out[k])!=Status::ok)return false;
    return true;
}
bool write_row(void* context,std::size_t row,std::span<const float> values) noexcept {
    auto& o=*static_cast<NumericOperationOutput*>(context);
    if(row>=o.value.actual[0] || values.size()!=o.value.actual[1])return false;
    for(std::size_t k=0;k<values.size();++k)if(store_element(o.value,{0,row,k},values[k])!=Status::ok)return false;
    return true;
}
dsp_kernels::FloatMatrixSource rows(const NumericOperationInput& i) noexcept {return {&i,read_row};}
dsp_kernels::FloatMatrixDestination rows(NumericOperationOutput& o) noexcept {return {&o,write_row};}
bool set_shape(NumericOperationOutput& out,std::size_t a,std::size_t b) noexcept {
    const auto* c=out.value.contract;
    if(!c || a<c->shape.extents[0].minimum || a>c->shape.extents[0].maximum ||
       b<c->shape.extents[1].minimum || b>c->shape.extents[1].maximum)return false;
    out.value.actual={a,b};return true;
}
template<class T> bool write_sequence(NumericOperationOutput& out,std::span<const T> values) noexcept {
    if(!set_shape(out,values.size()))return false;
    for(std::size_t k=0;k<values.size();++k)if(store_element(out.value,{0,k,0},values[k])!=Status::ok)return false;
    return true;
}
bool write_values(NumericOperationOutput& o,std::span<const float> v) noexcept {return write_sequence(o,v);}
bool write_indices(NumericOperationOutput& o,std::span<const std::size_t> v) noexcept {
    if(!set_shape(o,v.size()))return false;
    for(std::size_t k=0;k<v.size();++k)if(store_element(o.value,{0,k,0},std::uint64_t(v[k]))!=Status::ok)return false;
    return true;
}
bool write_u32(NumericOperationOutput& o,std::span<const std::uint32_t> v) noexcept {return write_sequence(o,v);}
bool write_mask(NumericOperationOutput& o,std::span<const std::uint8_t> v) noexcept {return write_sequence(o,v);}
bool write_samples(NumericOperationOutput& o,std::span<const std::uint64_t> v) noexcept {return write_sequence(o,v);}
bool write_seconds(NumericOperationOutput& o,std::span<const double> v) noexcept {return write_sequence(o,v);}
bool write_scalar(NumericOperationOutput& o,float v) noexcept {return set_shape(o,1) && store_element(o.value,{0,0,0},v)==Status::ok;}
bool within(const domain::ProcessingResources& a,const domain::ProcessingResources& b) noexcept {
    return a.plan_bytes<=b.plan_bytes && a.payload_bytes<=b.payload_bytes && a.scratch_bytes<=b.scratch_bytes && a.stack_bytes<=b.stack_bytes;
}
namespace {
bool parameter_value(const domain::ProcessingParameterValue& value,const domain::ParameterDescriptor& d) noexcept {
    using V=domain::ParameterValueKind;
    if(d.value_kind==V::Boolean)return std::holds_alternative<bool>(value);
    if(d.value_kind==V::String || d.value_kind==V::WindowFunction)return std::holds_alternative<std::string>(value);
    if(d.value_kind==V::NumericArray){const auto* a=std::get_if<domain::NumericArray>(&value);return a && a->values.size()<=domain::max_numeric_array_values;}
    const auto* n=std::get_if<NumericLiteral>(&value);if(!n)return false;
    return std::visit([&](auto x){
        if(d.value_kind==V::Integer && !std::is_integral_v<decltype(x)>)return false;
        return std::isfinite(double(x)) && double(x)>=d.minimum && double(x)<=d.maximum;
    },*n);
}
bool unique_parameter(std::span<const domain::ProcessingParameter> values,std::size_t index) noexcept {
    return std::none_of(values.begin(),values.begin()+std::ptrdiff_t(index),[&](const auto& value){return value.key==values[index].key;});
}

bool parameters_valid(const domain::ResolvedProcessingNode& n) noexcept {
    const auto* p=n.parameters?std::get_if<domain::TransformProcessingParameters>(n.parameters):nullptr;
    if(!p)return false;const auto descriptors=domain::sequence_operation_parameters(n.kind);
    for(std::size_t i=0;i<p->values.size();++i){
        const auto& v=p->values[i];const auto found=std::find_if(descriptors.begin(),descriptors.end(),[&](const auto& d){return d.key==v.key;});
        if(found==descriptors.end() || !parameter_value(v.value,*found) || !unique_parameter(p->values,i))return false;
    }
    return true;
}
double pulse_input_rate(const domain::ResolvedProcessingNode& n) noexcept {
    std::size_t hop=1;if(!integer(n,"hop",1,hop) || !hop)return 0;
    return setting(n,"frame_rate",44100.f/512)/double(hop);
}
double tracked_feature_rate(const domain::ResolvedProcessingNode& n) noexcept {
    if(n.kind=="PatternStateSequence")return setting(n,"frame_rate",50);
    const auto* spec=domain::sequence_operation_spec(n.kind);
    if((spec && spec->family==domain::SequenceOperationFamily::Histogram) || n.kind=="BeatStateSequence" || n.kind=="DownbeatStateSequence")return setting(n,"frame_rate",100);
    if(n.kind=="TempoRatios" || n.kind=="EnvelopeTempoRatios")return setting(n,"frame_rate",44100.f/512);
    return 0;
}

double primary_input_rate(const domain::ResolvedProcessingNode& n) noexcept {
    const auto k=n.kind;
    if(k=="PatternFeatures" || k=="PcmPatternSequence")return 44100;
    if(k.starts_with("Pcm"))return setting(n,"sample_rate",44100);
    if(k=="OnsetStrengthSequence")return setting(n,"frame_rate",120);
    if(k=="OnsetDetection" || k=="EnvelopeTempo" || k=="EnvelopeLocalPulse" || k=="EnvelopeBeatPath")return envelope_rate(n);
    return tracked_feature_rate(n);
}

double input_rate(const domain::ResolvedProcessingNode& n,domain::PortId port) noexcept {
    if(n.kind=="SpectralNovelty" || n.kind=="ActivationPeakMatrix")return setting(n,"frame_rate",100);
    if(n.kind=="LocalPulse" && port<2)return pulse_input_rate(n);
    if(port && !(port==1 && n.kind=="OnsetDetection"))return 0;
    if(n.kind=="DynamicBeatPath")return port?0:configured_feature_rate(n);
    if(n.kind=="TempoEstimate")return 0;
    return primary_input_rate(n);
}
bool input_axis(const domain::ResolvedProcessingNode& n,domain::PortId port,const NumericContract& c) noexcept {
    const auto& axis=c.coordinates[0];const double rate=input_rate(n,port);
    if(rate && (n.kind.starts_with("Pcm") || n.kind=="PatternFeatures") && c.clock.sample_rate && double(c.clock.sample_rate)!=rate)return false;
    if(axis.mode!=CoordinateMode::Regular || axis.unit!=NumericUnit::Seconds || !rate)return true;
    if(!axis.origin.denominator || !axis.step.denominator || axis.origin.numerator)return false;
    const double step=double(axis.step.numerator)/double(axis.step.denominator),expected=1/rate;
    return std::abs(step-expected)<=8*std::numeric_limits<double>::epsilon()*std::max(1.,expected);
}
double axis_coordinate(std::span<const std::byte> bytes,std::size_t index) noexcept {
    std::uint64_t bits=0;
    for(std::size_t b=0;b<8;++b)bits|=std::uint64_t(std::to_integer<unsigned>(bytes[8*index+b]))<<(8*b);
    return std::bit_cast<double>(bits);
}

bool explicit_input_axis(const NumericOperationInput& input,double rate) noexcept {
    const auto& axis=input.value.contract->coordinates[0];
    if(!rate || axis.mode!=CoordinateMode::ExplicitFloat64LE || axis.unit!=NumericUnit::Seconds)return true;
    const auto bytes=input.value.coordinates.axes[0];if(input.value.actual[0]>bytes.size()/8)return false;
    for(std::size_t k=0;k<input.value.actual[0];++k){
        const double value=axis_coordinate(bytes,k),expected=double(k)/rate;
        if(!std::isfinite(value) || std::abs(value-expected)>8*std::numeric_limits<double>::epsilon()*std::max(1.,expected))return false;
    }
    return true;
}
}
namespace {
bool sequence_outputs_valid(const domain::ResolvedProcessingNode& n,const FrameLimits& limits) noexcept {
    for(std::size_t k=0;k<n.outputs.size();++k)
        if(n.outputs[k].id!=k || validate_contract(n.outputs[k].contract.view(),limits)!=Status::ok)return false;
    return true;
}
bool sequence_inputs_valid(const domain::ResolvedProcessingNode& n,std::size_t maximum,const FrameLimits& limits) noexcept {
    for(std::size_t k=0;k<n.inputs.size();++k){
        const auto& input=n.inputs[k];
        if(input.id>=maximum || validate_contract(input.contract.view(),limits)!=Status::ok)return false;
        const auto c=contract(n,domain::PortDirection::Input,input.id);if(!c || !input_axis(n,input.id,*c))return false;
        for(std::size_t j=0;j<k;++j)if(n.inputs[j].id==input.id)return false;
    }
    return true;
}
}

bool descriptor_resources(const domain::ResolvedProcessingNode& n,const NumericRuntimeProfile& p,std::size_t object,
    std::size_t kernel,domain::ProcessingSupport& out) noexcept {
    domain::ProcessingSupport s;const auto* spec=domain::sequence_operation_spec(n.kind);
    if(!spec || !parameters_valid(n) || n.outputs.size()!=spec->outputs || n.inputs.empty() || n.inputs.size()>spec->inputs)return false;
    if(!sequence_outputs_valid(n,p.admission.frame) || !sequence_inputs_valid(n,spec->inputs,p.admission.frame))return false;
    if(!numeric_operation_descriptor_required_bytes(n,p.admission,s.resources.plan_bytes) || !numeric_operation_add_bytes(s.resources.plan_bytes,object))return false;
    s.resources.scratch_bytes=kernel;s.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;s.supported=true;out=s;return true;
}
void SequenceOperation::bind(std::shared_ptr<const NumericOperationDescriptor> d,domain::ProcessingAdmissionLimits limits) noexcept {
    descriptor_=std::move(d);limits_=limits;
}
std::unique_ptr<PreparedNumericOperation> SequenceOperation::clone() const {
    if(!descriptor_)return {};
    auto model=std::static_pointer_cast<const ClassicalModel>(descriptor_->immutable_resources);
    auto next=prepare_sequence(descriptor_->node,std::move(model));if(next)next->bind(descriptor_,limits_);return next;
}
Status SequenceOperation::copy_state_from(const PreparedNumericOperation&) noexcept {
    // Cold clone copies each concrete kernel independently. Graph continuity
    // transfer is deliberately unsupported, including continuing decoders;
    // runtime must reset an incompatible replacement instead of hot copying.
    return Status::incompatible_contract;
}
void SequenceOperation::reset() noexcept {reset_kernel();}
bool SequenceOperation::owned_resources(domain::ProcessingResources& out) const noexcept {
    out={};if(!descriptor_ || !numeric_operation_descriptor_owned_bytes(*descriptor_,limits_,out.plan_bytes) ||
       !numeric_operation_add_bytes(out.plan_bytes,object_bytes()))return false;
    if(descriptor_->immutable_resources){
        std::size_t bytes=0;const auto model=std::static_pointer_cast<const ClassicalModel>(descriptor_->immutable_resources);
        if(!model->owned_bytes(bytes) || !numeric_operation_add_bytes(out.plan_bytes,bytes))return false;
    }
    out.scratch_bytes=workspace_bytes();out.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;return true;
}
namespace {
bool contiguous_element_matches(const NumericOperationInput& input,std::size_t row,std::size_t column) noexcept {
    float value=0;if(load_element(input.value,{0,row,column},value)!=Status::ok)return false;
    return std::bit_cast<std::uint32_t>(value)==std::bit_cast<std::uint32_t>(input.contiguous_float32[row*input.value.actual[1]+column]);
}

bool contiguous_values_match(const NumericOperationInput& input) noexcept {
    for(std::size_t row=0;row<input.value.actual[0];++row)
        for(std::size_t column=0;column<input.value.actual[1];++column)
            if(!contiguous_element_matches(input,row,column))return false;
    return true;
}

bool finite_input(const NumericOperationInput& i,ValueDomain domain) noexcept {
    if(validate_values(i.value,domain,FrameLimits{SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX})!=Status::ok)return false;
    if(i.value.contract->dtype!=DType::Float32)return true;
    if(i.value.contract->shape.rank==Rank::Matrix && i.contiguous_float32.empty())return true;
    std::size_t count=0;if(!numeric_operation_mul_bytes(i.value.actual[0],i.value.actual[1],count) || i.contiguous_float32.size()!=count)return false;
    return contiguous_values_match(i);
}
bool disjoint(std::span<const std::byte> a,std::span<const std::byte> b) noexcept {
    if(a.empty() || b.empty())return true;const std::less<const std::byte*> less;
    return !less(a.data(),b.data()+b.size()) || !less(b.data(),a.data()+a.size());
}
bool output_coordinates(NumericOperationOutput& o,const domain::ResolvedProcessingNode& node) noexcept {
    for(std::size_t axis=0;axis<2;++axis){
        if(o.value.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE)continue;
        const double step=o.value.contract->coordinates[axis].unit==NumericUnit::Seconds?output_time_step(node,o.port,axis):1;
        for(std::size_t k=0;k<o.value.actual[axis];++k)
            if(numeric_operation_coordinate(o,axis,k,double(k)*step)!=Status::ok)return false;
        if(numeric_operation_axis(o,axis,o.value.actual[axis])!=Status::ok)return false;
    }
    return true;
}
}
namespace {
bool valid_input_ports(std::span<const NumericOperationInput> inputs,const auto& rules) noexcept {
    for(std::size_t k=0;k<inputs.size();++k){
        if(std::none_of(rules.begin(),rules.end(),[&](const auto& rule){return rule.port==inputs[k].port;}))return false;
        const auto previous=inputs.first(k);
        if(std::any_of(previous.begin(),previous.end(),[&](const auto& input){return input.port==inputs[k].port;}))return false;
    }
    return true;
}
Status sequence_input_status(const domain::ResolvedProcessingNode& node,const auto& rule,std::span<const NumericOperationInput> inputs,const FrameLimits& limits) noexcept {
    const auto* input=available_input_at(inputs,rule.port);if(!input)return rule.required?Status::unavailable:Status::ok;
    const auto c=contract(node,domain::PortDirection::Input,rule.port);
    if(!input->value.contract || !c || !same_contract(*input->value.contract,*c))return Status::incompatible_contract;
    if(validate_view(DataFrameView{input->stamp,input->value},limits)!=Status::ok || !finite_input(*input,rule.values) ||
       !explicit_input_axis(*input,input_rate(node,rule.port)))return Status::invalid_argument;
    return Status::ok;
}
bool sequence_output_disjoint(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,std::size_t at) noexcept {
    for(const auto& input:inputs)if(!disjoint(input.value.storage,outputs[at].value.storage))return false;
    for(std::size_t k=0;k<at;++k)if(!disjoint(outputs[k].value.storage,outputs[at].value.storage))return false;
    return true;
}
bool sequence_output_valid(NumericOperationOutput& output,const domain::ResolvedProcessingNode& node,const FrameLimits& limits) noexcept {
    const auto c=contract(node,domain::PortDirection::Output,output.port);
    return output.value.contract && c && same_contract(*output.value.contract,*c) && output_coordinates(output,node) &&
        validate_mutable(output.value,limits)==Status::ok;
}
FrameStamp sequence_stamp(const FrameStamp& source) noexcept {
    FrameStamp stamp;stamp.sequence=source.sequence;stamp.generation=source.generation;stamp.epoch=source.epoch;
    stamp.graph_revision=source.graph_revision;stamp.source_sequence=source.source_sequence;
    stamp.producer_epoch=source.producer_epoch;stamp.discontinuity=source.discontinuity;return stamp;
}
void mark_sequence_unavailable(std::span<NumericOperationOutput> outputs) noexcept {
    for(auto& output:outputs){output.status=Status::unavailable;output.stamp.available=false;}
}
bool fill_disabled_sequence(std::span<NumericOperationOutput> outputs,const FrameLimits& limits) noexcept {
    for(auto& output:outputs){
        output.value.actual={std::size_t(output.value.contract->shape.extents[0].maximum),std::size_t(output.value.contract->shape.extents[1].maximum)};
        if(fill_silence(output.value,limits)!=Status::ok)return false;
    }
    return true;
}
}

Status SequenceOperation::preflight_inputs(std::span<const NumericOperationInput> inputs) const noexcept {
    if(!valid_input_ports(inputs,descriptor_->policy.inputs))return Status::invalid_argument;
    const auto node=definition(descriptor_->node);
    for(const auto& rule:descriptor_->policy.inputs){
        const auto status=sequence_input_status(node,rule,inputs,limits_.frame);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status SequenceOperation::preflight_outputs(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) const noexcept {
    const auto node=definition(descriptor_->node);
    for(std::size_t k=0;k<outputs.size();++k){
        auto& output=outputs[k];const auto previous=outputs.first(k);
        if(std::any_of(previous.begin(),previous.end(),[&](const auto& value){return value.port==output.port;}))return Status::invalid_argument;
        if(!sequence_output_valid(output,node,limits_.frame))return Status::incompatible_contract;
        if(!sequence_output_disjoint(inputs,outputs,k))return Status::invalid_argument;
    }
    return Status::ok;
}
Status SequenceOperation::preflight(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs) const noexcept {
    if(!descriptor_ || outputs.size()!=descriptor_->node.outputs.size())return Status::invalid_argument;
    const auto status=preflight_inputs(inputs);return status==Status::ok?preflight_outputs(inputs,outputs):status;
}
namespace {
bool prepare_sequence_publication(NumericOperationOutput& output,const domain::ResolvedProcessingNode& node,const FrameStamp& source,ValueDomain values,const FrameLimits& limits) noexcept {
    if(!output_coordinates(output,node))return false;
    output.stamp=sequence_stamp(source);
    return validate_view(DataFrameView{output.stamp,output.value.view()},limits)==Status::ok && validate_values(output.value.view(),values,limits)==Status::ok;
}
}

bool SequenceOperation::publish(std::span<NumericOperationOutput> outputs,const FrameStamp& source) noexcept {
    const auto node=definition(descriptor_->node);
    for(auto& output:outputs){
        const auto values=descriptor_->policy.output_values.empty()?ValueDomain::FiniteOnly:descriptor_->policy.output_values[output.port];
        if(!prepare_sequence_publication(output,node,source,values,limits_.frame)){mark_sequence_unavailable(outputs);return false;}
    }
    for(auto& output:outputs)output.status=Status::ok;
    return true;
}
Status SequenceOperation::process(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,NumericOperationControl control) noexcept {
    mark_sequence_unavailable(outputs);
    const auto status=preflight(inputs,outputs);if(status!=Status::ok)return status;
    const auto* trigger=input_at(inputs,0);if(!trigger)return Status::unavailable;
    if(control.reset)reset_kernel();
    const bool ready=control.enabled?run(inputs,outputs):fill_disabled_sequence(outputs,limits_.frame);
    if(!ready)return Status::invalid_argument;
    return publish(outputs,trigger->stamp)?Status::ok:Status::invalid_argument;
}
}
