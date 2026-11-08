#include "operation_support.h"
#include <dsp_kernels/kernel_support.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace dsp_graph_adapters::detail {
using namespace signal_contracts;
using namespace dataflow_runtime;
dataflow_runtime::NumericOperationNodeView definition(const dataflow_runtime::NumericOperationNode& n) noexcept {
    dataflow_runtime::NumericOperationNodeView r;r.id=n.id;r.kind=n.kind;r.parameters=&n.parameters;
    r.inputs=n.inputs;r.outputs=n.outputs;r.enabled=n.enabled;return r;
}
const domain::ProcessingParameterValue* parameter(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key) noexcept {
    const auto* p=n.parameters?std::get_if<domain::TransformProcessingParameters>(n.parameters):nullptr;
    if(p)for(const auto& entry:p->values)if(entry.key==key)return &entry.value;
    return nullptr;
}
bool number(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key,float fallback,float& out) noexcept {
    const auto* p=parameter(n,key);if(!p){out=fallback;return true;}
    const auto* v=std::get_if<NumericLiteral>(p);if(!v)return false;
    out=std::visit([](auto x){return float(x);},*v);return std::isfinite(out);
}
bool integer(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key,std::size_t fallback,std::size_t& out) noexcept {
    const auto* p=parameter(n,key);if(!p){out=fallback;return true;}
    const auto* v=std::get_if<NumericLiteral>(p);if(!v)return false;
    return std::visit([&](auto x){
        if constexpr(!std::is_integral_v<decltype(x)>)return false;
        else {if(!std::in_range<std::size_t>(x))return false;out=std::size_t(x);return true;}
    },*v);
}
bool flag(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key,bool fallback,bool& out) noexcept {
    const auto* p=parameter(n,key);if(!p){out=fallback;return true;}
    const auto* v=std::get_if<bool>(p);if(!v)return false;out=*v;return true;
}
bool feature_rate(const dataflow_runtime::NumericOperationNodeView& node,float fallback,float& out) noexcept {
    float rate=0;std::size_t hop=0;
    if(!number(node,"sample_rate",0,rate))return false;
    if(!rate)return number(node,"frame_rate",fallback,out) && out>0;
    if(rate<0 || !integer(node,"hop_samples",512,hop) || !hop)return false;
    out=rate/float(hop);return std::isfinite(out) && out>0;
}
std::span<const float> array(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key) noexcept {
    const auto* p=parameter(n,key);const auto* v=p?std::get_if<domain::NumericArray>(p):nullptr;
    return v?std::span<const float>(v->values):std::span<const float>{};
}
std::string_view text(const dataflow_runtime::NumericOperationNodeView& n,std::string_view key) noexcept {
    const auto* p=parameter(n,key);const auto* v=p?std::get_if<std::string>(p):nullptr;return v?std::string_view(*v):std::string_view{};
}
std::optional<NumericBufferContract> contract(const dataflow_runtime::NumericOperationNodeView& n,domain::PortDirection direction,domain::PortId id) noexcept {
    const auto ports=direction==domain::PortDirection::Input?n.inputs:n.outputs;
    for(const auto& p:ports)if(p.id==id){
        return p.contract;
    }
    return std::nullopt;
}
bool input_geometry(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId id,Rank rank,std::size_t& a,std::size_t& b) noexcept {
    const auto c=contract(n,domain::PortDirection::Input,id);
    if(!c || c->dtype!=DType::Float32 || c->shape.rank!=rank || c->plane_count!=1 ||
       c->representation.encoding!=StorageEncoding::Native || c->representation.layout!=ChannelLayout::Planar || c->representation.valid_bits ||
       (c->representation.normalization!=Normalization::None && c->representation.normalization!=Normalization::FloatUnit))return false;
    if(c->shape.extents[0].maximum>SIZE_MAX || c->shape.extents[1].maximum>SIZE_MAX)return false;
    a=std::size_t(c->shape.extents[0].maximum);b=std::size_t(c->shape.extents[1].maximum);return a && b;
}
bool optional_vector(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId port,std::size_t maximum,bool scalar) noexcept {
    if(!contract(n,domain::PortDirection::Input,port))return true;
    std::size_t a=0,b=0;return input_geometry(n,port,Rank::Vector,a,b) && (a==maximum || (scalar && a==1));
}
bool output_geometry(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId id,Rank rank,std::size_t a,std::size_t b,DType type) noexcept {
    const auto c=contract(n,domain::PortDirection::Output,id);
    return c && c->dtype==type && c->shape.rank==rank && c->plane_count==1 &&
        c->shape.extents[0].maximum==a && c->shape.extents[1].maximum==b &&
        c->shape.extents[0].minimum==(rank==Rank::Scalar?1u:0u) && c->shape.extents[1].minimum==b &&
        c->representation.encoding==StorageEncoding::Native && c->representation.layout==ChannelLayout::Planar && !c->representation.valid_bits &&
        c->representation.normalization==Normalization::None;
}
const NumericOperationInput* input_at(std::span<const NumericOperationInput> inputs,domain::PortId id) noexcept {
    for(const auto& i:inputs)if(i.port==id)return &i;return nullptr;
}
const NumericOperationInput* available_input_at(std::span<const NumericOperationInput> inputs,domain::PortId id) noexcept {
    const auto* value=input_at(inputs,id);
    return value && value->status==Status::ok?value:nullptr;
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

bool parameters_valid(const dataflow_runtime::NumericOperationNodeView& n) noexcept {
    const auto* p=n.parameters?std::get_if<domain::TransformProcessingParameters>(n.parameters):nullptr;
    if(!p)return false;const auto descriptors=domain::sequence_operation_parameters(n.kind);
    for(std::size_t i=0;i<p->values.size();++i){
        const auto& v=p->values[i];const auto found=std::find_if(descriptors.begin(),descriptors.end(),[&](const auto& d){return d.key==v.key;});
        if(found==descriptors.end() || !parameter_value(v.value,*found) || !unique_parameter(p->values,i))return false;
    }
    return true;
}



}
namespace {
bool sequence_outputs_valid(const dataflow_runtime::NumericOperationNodeView& n,const FrameLimits& limits) noexcept {
    for(std::size_t k=0;k<n.outputs.size();++k)
        if(n.outputs[k].id!=k || validate_contract(n.outputs[k].contract.view(),limits)!=Status::ok)return false;
    return true;
}
bool sequence_inputs_valid(const dataflow_runtime::NumericOperationNodeView& n,std::size_t maximum,const FrameLimits& limits) noexcept {
    for(std::size_t k=0;k<n.inputs.size();++k){
        const auto& input=n.inputs[k];
        if(input.id>=maximum || validate_contract(input.contract.view(),limits)!=Status::ok)return false;
        for(std::size_t j=0;j<k;++j)if(n.inputs[j].id==input.id)return false;
    }
    return true;
}
}

bool descriptor_resources(const dataflow_runtime::NumericOperationNodeView& n,const NumericOperationProfile& p,std::size_t object,
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
Status sequence_input_status(const dataflow_runtime::NumericOperationNodeView& node,const auto& rule,std::span<const NumericOperationInput> inputs,const FrameLimits& limits) noexcept {
    const auto* input=available_input_at(inputs,rule.port);if(!input)return rule.required?Status::unavailable:Status::ok;
    const auto c=contract(node,domain::PortDirection::Input,rule.port);
    if(!input->value.contract || !c || !same_contract(*input->value.contract,*c))return Status::incompatible_contract;
    if(validate_view(input->value,limits)!=Status::ok || !finite_input(*input,rule.values))return Status::invalid_argument;
    return Status::ok;
}
bool sequence_output_disjoint(std::span<const NumericOperationInput> inputs,std::span<NumericOperationOutput> outputs,std::size_t at) noexcept {
    for(const auto& input:inputs)if(!disjoint(input.value.storage,outputs[at].value.storage))return false;
    for(std::size_t k=0;k<at;++k)if(!disjoint(outputs[k].value.storage,outputs[at].value.storage))return false;
    return true;
}
bool sequence_output_valid(NumericOperationOutput& output,const dataflow_runtime::NumericOperationNodeView& node,const FrameLimits& limits) noexcept {
    const auto c=contract(node,domain::PortDirection::Output,output.port);
    return output.value.contract && c && same_contract(*output.value.contract,*c) &&
        validate_mutable(output.value,limits)==Status::ok;
}
void mark_sequence_unavailable(std::span<NumericOperationOutput> outputs) noexcept {
    for(auto& output:outputs){output.status=Status::unavailable;}
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
bool prepare_sequence_publication(NumericOperationOutput& output,NumericOperationOrigin source,ValueDomain values,const FrameLimits& limits) noexcept {
    output.origin=numeric_operation_origin(source,NumericOperationPublication::Batch);
    return validate_view(output.value.view(),limits)==Status::ok && validate_values(output.value.view(),values,limits)==Status::ok;
}
}

bool SequenceOperation::publish(std::span<NumericOperationOutput> outputs,NumericOperationOrigin source) noexcept {
    for(auto& output:outputs){
        const auto values=descriptor_->policy.output_values.empty()?ValueDomain::FiniteOnly:descriptor_->policy.output_values[output.port];
        if(!prepare_sequence_publication(output,source,values,limits_.frame)){mark_sequence_unavailable(outputs);return false;}
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
    return publish(outputs,trigger->origin)?Status::ok:Status::invalid_argument;
}
}
