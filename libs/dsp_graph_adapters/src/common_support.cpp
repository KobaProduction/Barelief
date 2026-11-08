#include "common_array.h"
#include <dsp_kernels/kernel_support.h>
#include <domain/numeric/common_operations.h>
#include <algorithm>
#include <functional>
#include <utility>

namespace dsp_graph_adapters::detail {
using namespace signal_contracts;using namespace dataflow_runtime;
namespace {
bool disjoint(std::span<const std::byte> a,std::span<const std::byte> b) noexcept {
    if(a.empty() || b.empty())return true;
    const std::less<const std::byte*> less;
    return !less(a.data(),b.data()+b.size()) || !less(b.data(),a.data()+a.size());
}
bool common_ports_valid(NumericOperationPorts ports,const FrameLimits& limits) noexcept {
    for(std::size_t k=0;k<ports.size();++k)
        if(ports[k].id!=k || validate_contract(ports[k].contract.view(),limits)!=Status::ok)return false;
    return true;
}
bool output_storage_disjoint(std::span<const NumericOperationInput> input,std::span<NumericOperationOutput> output,std::size_t at) noexcept {
    for(const auto& value:input)if(!disjoint(value.value.storage,output[at].value.storage))return false;
    for(std::size_t k=0;k<at;++k)if(!disjoint(output[k].value.storage,output[at].value.storage))return false;
    return true;
}
bool configured_axes(const dataflow_runtime::NumericOperationNodeView& node,NumericOperationOutput& out) noexcept {
    if(node.kind!="FilterMatrixProjection" || out.port)return true;
    const auto centers=array(node,"centers_hz");if(centers.empty())return true;
    const auto axis=out.value.contract->shape.rank==Rank::Matrix?1u:0u;
    if(!out.coordinates.available(axis))return true;
    for(std::size_t bin=0;bin<centers.size();++bin)if(numeric_operation_coordinate(out,axis,bin,centers[bin])!=Status::ok)return false;
    return numeric_operation_axis(out,axis,out.value.actual[axis])==Status::ok;
}
bool disabled_axis_shape(const dataflow_runtime::NumericOperationNodeView& node,const NumericOperationInput& input,NumericOperationOutput& out) noexcept {
    CommonArrayGeometry geometry;bool cumulative=false;
    if(!common_array_geometry(node,0,geometry,true,true))return false;
    if(node.kind=="AxisSum" && !flag(node,"cumulative",false,cumulative))return false;
    const bool reduced=node.kind=="AxisMaximum"?out.port!=2:!cumulative;
    out.value.actual=reduced?std::array<std::size_t,2>{common_array_rows(input.value,geometry),1}:input.value.actual;
    return true;
}
bool disabled_preserves_shape(std::string_view kind) noexcept {
    return kind=="ArrayGather" || kind=="NumericMap" || kind=="AbsoluteValue" || kind=="Clamp" ||
        kind=="QuantileMask" || kind=="LogCompression" || kind=="VectorNormalize" || kind=="RecursiveFilter";
}
bool disabled_row_shape(std::string_view kind,Rank rank) noexcept {
    return kind=="CombFilterBank" || ((kind=="FilterMatrixProjection" || kind=="DirectCorrelation") && rank==Rank::Matrix);
}
bool disabled_value_shape(const dataflow_runtime::NumericOperationNode& node,const NumericOperationInput& input,NumericOperationOutput& out) noexcept {
    if(node.kind=="LinearExpansion")return linear_expansion_actual(definition(node),input.value.actual[0],out.value.actual[0]);
    if(disabled_preserves_shape(node.kind))out.value.actual=input.value.actual;
    if(disabled_row_shape(node.kind,input.value.contract->shape.rank))out.value.actual[0]=input.value.actual[0];
    if(node.kind=="TemporalFrameSequence")return temporal_frame_sequence_rows(definition(node),input.value.actual[0],out.value.actual[0]);
    return true;
}

bool disabled_shape(const dataflow_runtime::NumericOperationNode& node,const NumericOperationInput& input,NumericOperationOutput& out) noexcept {
    if(node.kind=="AxisMaximum" || node.kind=="AxisSum")return disabled_axis_shape(definition(node),input,out);
    if(node.kind=="InverseFourier" && input.value.contract->shape.rank==Rank::Matrix){out.value.actual[0]=input.value.actual[0];return true;}
    if(out.port)return true;
    return disabled_value_shape(node,input,out);
}

bool fill_disabled_common(const dataflow_runtime::NumericOperationNode& node,const NumericOperationInput& input,std::span<NumericOperationOutput> output,const FrameLimits& limits) noexcept {
    for(auto& value:output){
        value.value.actual={std::size_t(value.value.contract->shape.extents[0].maximum),std::size_t(value.value.contract->shape.extents[1].maximum)};
        if(!disabled_shape(node,input,value))return false;
        if(fill_silence(value.value,limits)!=Status::ok)return false;
    }
    return true;
}
void mark_common_unavailable(std::span<NumericOperationOutput> outputs) noexcept {
    for(auto& output:outputs){output.status=Status::unavailable;}
}
}
bool common_vector(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId port,std::size_t& size) noexcept {
    const auto c=contract(n,domain::PortDirection::Input,port);
    if(!c || c->shape.rank!=Rank::Vector || c->dtype!=DType::Float32 || c->plane_count!=1 ||
       c->representation.encoding!=StorageEncoding::Native || c->representation.layout!=ChannelLayout::Planar || c->representation.valid_bits ||
       (c->representation.normalization!=Normalization::None && c->representation.normalization!=Normalization::FloatUnit) || c->shape.extents[0].maximum>SIZE_MAX)return false;
    size=std::size_t(c->shape.extents[0].maximum);return size!=0;
}
bool common_output(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId port,Rank rank,std::size_t a,std::size_t b,DType type) noexcept {
    const auto c=contract(n,domain::PortDirection::Output,port);
    if(!c || c->dtype!=type || c->shape.rank!=rank || c->plane_count!=1 || c->shape.extents[0].maximum!=a || c->shape.extents[1].maximum!=b ||
       c->representation.encoding!=StorageEncoding::Native || c->representation.layout!=ChannelLayout::Planar || c->representation.valid_bits ||
       c->representation.normalization!=Normalization::None)return false;
    return true;
}
bool common_values(const NumericOperationInput& input,std::span<float> scratch,std::span<const float>& values) noexcept {
    if(input.value.actual[0]>scratch.size() || input.value.actual[1]!=1)return false;
    auto row=scratch.first(input.value.actual[0]);
    for(std::size_t k=0;k<row.size();++k)if(load_element(input.value,{0,k,0},row[k])!=Status::ok)return false;
    values=row;return true;
}
bool common_resources(const dataflow_runtime::NumericOperationNodeView& n,const NumericOperationProfile& p,std::size_t object,
    std::size_t workspace,domain::ProcessingSupport& out) noexcept {
    const auto* spec=domain::common_operation_spec(n.kind);
    if(!spec || n.inputs.size()<spec->inputs || n.inputs.size()>spec->inputs+spec->optional_inputs || n.outputs.size()!=spec->outputs)return false;
    if(!common_ports_valid(n.inputs,p.admission.frame) || !common_ports_valid(n.outputs,p.admission.frame))return false;
    domain::ProcessingSupport next;
    if(!numeric_operation_descriptor_required_bytes(n,p.admission,next.resources.plan_bytes) || !numeric_operation_add_bytes(next.resources.plan_bytes,object))return false;
    next.resources.scratch_bytes=workspace;next.resources.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;next.supported=true;out=next;return true;
}
bool CommonOperation::owned_resources(domain::ProcessingResources& out) const noexcept {
    out={};if(!descriptor_ || !numeric_operation_descriptor_owned_bytes(*descriptor_,limits_,out.plan_bytes) ||
        !numeric_operation_add_bytes(out.plan_bytes,object_bytes()))return false;
    out.scratch_bytes=workspace_bytes();out.stack_bytes=dsp_kernels::numeric_reference_stack_bytes;return true;
}
Status CommonOperation::preflight_inputs(std::span<const NumericOperationInput> in) const noexcept {
    const auto node=definition(descriptor_->node);
    for(std::size_t k=0;k<in.size();++k){
        const auto& input=in[k];const auto c=contract(node,domain::PortDirection::Input,domain::PortId(k));
        if(input.port!=k || !c || !input.value.contract || !same_physical_contract(*input.value.contract,*c))return Status::incompatible_contract;
        if(input.status!=Status::ok)return Status::unavailable;
        if(validate_view(input.value,limits_.frame)!=Status::ok || validate_values(input.value,descriptor_->policy.inputs[k].values,limits_.frame)!=Status::ok)return Status::invalid_argument;
    }
    return Status::ok;
}
Status CommonOperation::preflight_outputs(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) const noexcept {
    const auto node=definition(descriptor_->node);
    for(std::size_t k=0;k<out.size();++k){
        const auto c=contract(node,domain::PortDirection::Output,domain::PortId(k));
        if(out[k].port!=k || !c || !out[k].value.contract || !same_physical_contract(*out[k].value.contract,*c) || validate_mutable(out[k].value,limits_.frame)!=Status::ok)return Status::incompatible_contract;
        if(!output_storage_disjoint(in,out,k))return Status::invalid_argument;
    }
    return Status::ok;
}
Status CommonOperation::preflight(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) const noexcept {
    mark_common_unavailable(out);
    if(!descriptor_ || in.size()!=descriptor_->node.inputs.size() || out.size()!=descriptor_->node.outputs.size())return Status::invalid_argument;
    const auto status=preflight_inputs(in);
    if(status!=Status::ok)return status;
    const auto outputs=preflight_outputs(in,out);if(outputs!=Status::ok)return outputs;
    return input_at(in,descriptor_->policy.stream_input.value_or(0))?Status::ok:Status::invalid_argument;
}
bool CommonOperation::publish(std::span<NumericOperationOutput> out,const NumericOperationInput& input) const noexcept {
    for(auto& o:out){
        if(!configured_axes(definition(descriptor_->node),o))return false;
        const auto mode=descriptor_->node.kind=="FeatureAggregate" && o.value.contract->shape.rank==Rank::Scalar?NumericOperationPublication::Point:NumericOperationPublication::Frame;
        o.origin=numeric_operation_origin(input.origin,mode);
        const auto domain=descriptor_->policy.output_values[o.port];
        if(validate_view(o.value.view(),limits_.frame)!=Status::ok || validate_values(o.value.view(),domain,limits_.frame)!=Status::ok)return false;
    }
    for(auto& o:out)o.status=Status::ok;
    return true;
}
Status CommonOperation::process(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,NumericOperationControl control) noexcept {
    const auto status=preflight(in,out);if(status!=Status::ok)return status;
    const auto* driver=input_at(in,descriptor_->policy.stream_input.value_or(0));
    if(control.reset)reset_kernel();
    if(control.enabled){if(!run(in,out))return Status::invalid_argument;}
    else if(!fill_disabled_common(descriptor_->node,*driver,out,limits_.frame))return Status::invalid_argument;
    return publish(out,*driver)?Status::ok:Status::invalid_argument;
}
}
