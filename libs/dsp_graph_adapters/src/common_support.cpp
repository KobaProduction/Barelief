#include "common_support.h"
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
bool index_axis(const AxisCoordinates& a) noexcept {
    return a.mode==CoordinateMode::None || (a.mode==CoordinateMode::Regular &&
        (a.unit==NumericUnit::Values || a.unit==NumericUnit::Frames) && !a.origin.numerator &&
        a.origin.denominator && a.step.numerator>0 && std::uint64_t(a.step.numerator)==a.step.denominator);
}
bool clock_matches(const ClockContract& a,const ClockContract& b) noexcept {
    return a.domain==b.domain && a.tick_seconds==b.tick_seconds && a.sample_rate==b.sample_rate;
}
FrameStamp output_stamp(const NumericOperationInput& input,const ClockContract& clock,bool point) noexcept {
    auto stamp=input.stamp;
    if(point){stamp.has_sample_interval=false;stamp.sample_begin=0;stamp.sample_count=0;}
    if(clock_matches(clock,input.value.contract->clock))return stamp;
    stamp.timestamp_ticks=0;stamp.stream_id={};stamp.has_sample_interval=false;stamp.sample_begin=0;
    stamp.sample_count=0;stamp.latency_samples=0;stamp.latency_seconds={0,1};stamp.frequency_dependent_latency=false;
    return stamp;
}
bool read_first_lag(const domain::ResolvedProcessingNode& node,std::int64_t& first) noexcept {
    const auto* p=parameter(node,"first_lag");if(!p)return true;
    const auto* literal=std::get_if<NumericLiteral>(p);if(!literal)return false;
    return std::visit([&](auto value){
        if constexpr(std::is_integral_v<decltype(value)>){
            if(!std::in_range<std::int64_t>(value))return false;
            first=std::int64_t(value);return true;
        }else return false;
    },*literal);
}
bool projection_axis(const domain::ResolvedProcessingNode& node,const NumericContract& input,std::size_t axis,const AxisCoordinates& declared) noexcept {
    if(input.shape.rank==Rank::Matrix && !axis)return declared==input.coordinates[0];
    if(axis==(input.shape.rank==Rank::Matrix?1u:0u) && !array(node,"centers_hz").empty())
        return declared==AxisCoordinates{CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};
    return index_axis(declared);
}
bool common_ports_valid(std::span<const domain::ProcessingPort> ports,const FrameLimits& limits) noexcept {
    for(std::size_t k=0;k<ports.size();++k)
        if(ports[k].id!=k || validate_contract(ports[k].contract.view(),limits)!=Status::ok)return false;
    return true;
}
bool output_storage_disjoint(std::span<const NumericOperationInput> input,std::span<NumericOperationOutput> output,std::size_t at) noexcept {
    for(const auto& value:input)if(!disjoint(value.value.storage,output[at].value.storage))return false;
    for(std::size_t k=0;k<at;++k)if(!disjoint(output[k].value.storage,output[at].value.storage))return false;
    return true;
}
bool output_axis(const domain::ResolvedProcessingNode& n,const AxisCoordinates& axis) noexcept {
    if(n.kind!="DirectCorrelation")return index_axis(axis);
    std::int64_t first=0;if(!read_first_lag(n,first))return false;
    return axis.mode==CoordinateMode::Regular && axis.unit==NumericUnit::Frames && axis.origin==Ratio{first,1} && axis.step==Ratio{1,1};
}
bool retained_axis(const domain::ResolvedProcessingNode& n,domain::PortId port,std::size_t axis,const AxisCoordinates& declared) noexcept {
    const auto input=contract(n,domain::PortDirection::Input,0);
    if(!port && input){
        if(n.kind=="LogCompression" || n.kind=="VectorNormalize" || n.kind=="RecursiveFilter")return declared==input->coordinates[axis];
        if(n.kind=="FilterMatrixProjection")return projection_axis(n,*input,axis,declared);
    }
    return !axis?output_axis(n,declared):index_axis(declared);
}
bool bind_axes(NumericOperationOutput& out) noexcept {
    for(std::size_t axis=0;axis<2;++axis)if(out.value.contract->coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE)
        if(numeric_operation_axis(out,axis,out.value.actual[axis])!=Status::ok)return false;
    return true;
}
bool copy_axes(const NumericOperationInput& input,NumericOperationOutput& out,bool projection) noexcept {
    for(std::size_t axis=0;axis<2;++axis){
        if(projection && axis==(input.value.contract->shape.rank==Rank::Matrix?1u:0u))continue;
        if(out.value.contract->coordinates[axis].mode!=CoordinateMode::ExplicitFloat64LE ||
            out.value.contract->coordinates[axis]!=input.value.contract->coordinates[axis])continue;
        const auto source=input.value.coordinates.axes[axis];
        if(input.value.actual[axis]!=out.value.actual[axis] || source.size()>out.writable_coordinates[axis].size())return false;
        std::copy(source.begin(),source.end(),out.writable_coordinates[axis].begin());
    }
    return bind_axes(out);
}
bool configured_axes(const domain::ResolvedProcessingNode& node,NumericOperationOutput& out) noexcept {
    if(node.kind!="FilterMatrixProjection" || out.port)return true;
    const auto centers=array(node,"centers_hz");if(centers.empty())return true;
    const auto axis=out.value.contract->shape.rank==Rank::Matrix?1u:0u;
    for(std::size_t bin=0;bin<centers.size();++bin)if(numeric_operation_coordinate(out,axis,bin,centers[bin])!=Status::ok)return false;
    return true;
}
void disabled_shape(const domain::ConfiguredProcessingNode& node,const NumericOperationInput& input,NumericOperationOutput& out) noexcept {
    if(out.port)return;
    if(node.kind=="LogCompression" || node.kind=="VectorNormalize" || node.kind=="RecursiveFilter")out.value.actual=input.value.actual;
    if(node.kind=="FilterMatrixProjection" && input.value.contract->shape.rank==Rank::Matrix)out.value.actual[0]=input.value.actual[0];
}

bool fill_disabled_common(const domain::ConfiguredProcessingNode& node,const NumericOperationInput& input,std::span<NumericOperationOutput> output,const FrameLimits& limits) noexcept {
    for(auto& value:output){
        value.value.actual={std::size_t(value.value.contract->shape.extents[0].maximum),std::size_t(value.value.contract->shape.extents[1].maximum)};
        disabled_shape(node,input,value);
        if(!bind_axes(value) || fill_silence(value.value,limits)!=Status::ok)return false;
    }
    return true;
}
}
bool common_vector(const domain::ResolvedProcessingNode& n,domain::PortId port,std::size_t& size) noexcept {
    const auto c=contract(n,domain::PortDirection::Input,port);
    if(!c || c->shape.rank!=Rank::Vector || c->dtype!=DType::Float32 || c->channel_order.size()!=1 ||
       c->representation.encoding!=StorageEncoding::Native || c->representation.layout!=ChannelLayout::Planar || c->representation.valid_bits ||
       (c->representation.normalization!=Normalization::None && c->representation.normalization!=Normalization::FloatUnit) || c->shape.extents[0].maximum>SIZE_MAX)return false;
    size=std::size_t(c->shape.extents[0].maximum);return size!=0;
}
bool common_output(const domain::ResolvedProcessingNode& n,domain::PortId port,Rank rank,std::size_t a,std::size_t b,DType type) noexcept {
    const auto c=contract(n,domain::PortDirection::Output,port);
    if(!c || c->dtype!=type || c->shape.rank!=rank || c->channel_order.size()!=1 || c->shape.extents[0].maximum!=a || c->shape.extents[1].maximum!=b ||
       c->representation.encoding!=StorageEncoding::Native || c->representation.layout!=ChannelLayout::Planar || c->representation.valid_bits ||
       c->representation.normalization!=Normalization::None || !retained_axis(n,port,0,c->coordinates[0]) || !retained_axis(n,port,1,c->coordinates[1]))return false;
    const auto input=contract(n,domain::PortDirection::Input,0);
    return (c->clock.domain.empty() && !c->clock.sample_rate && c->clock.tick_seconds==Ratio{1,1}) || (input && clock_matches(c->clock,input->clock));
}
bool common_values(const NumericOperationInput& input,std::span<float> scratch,std::span<const float>& values) noexcept {
    if(input.value.actual[0]>scratch.size() || input.value.actual[1]!=1)return false;
    auto row=scratch.first(input.value.actual[0]);
    for(std::size_t k=0;k<row.size();++k)if(load_element(input.value,{0,k,0},row[k])!=Status::ok)return false;
    values=row;return true;
}
bool common_resources(const domain::ResolvedProcessingNode& n,const NumericRuntimeProfile& p,std::size_t object,
    std::size_t workspace,domain::ProcessingSupport& out) noexcept {
    const auto* spec=domain::common_operation_spec(n.kind);
    if(!spec || n.inputs.size()!=spec->inputs || n.outputs.size()!=spec->outputs)return false;
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
        if(input.status!=Status::ok || !input.stamp.available)return Status::unavailable;
        if(validate_view({input.stamp,input.value},limits_.frame)!=Status::ok || validate_values(input.value,descriptor_->policy.inputs[k].values,limits_.frame)!=Status::ok)return Status::invalid_argument;
    }
    return Status::ok;
}
Status CommonOperation::preflight_outputs(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) const noexcept {
    const auto node=definition(descriptor_->node);
    for(std::size_t k=0;k<out.size();++k){
        const auto c=contract(node,domain::PortDirection::Output,domain::PortId(k));
        if(out[k].port!=k || !c || !out[k].value.contract || !same_physical_contract(*out[k].value.contract,*c) || !bind_axes(out[k]) || validate_mutable(out[k].value,limits_.frame)!=Status::ok)return Status::incompatible_contract;
        if(!output_storage_disjoint(in,out,k))return Status::invalid_argument;
    }
    return Status::ok;
}
Status CommonOperation::preflight(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) const noexcept {
    if(!descriptor_ || in.size()!=descriptor_->node.inputs.size() || out.size()!=descriptor_->node.outputs.size())return Status::invalid_argument;
    const auto status=preflight_inputs(in);
    return status==Status::ok?preflight_outputs(in,out):status;
}
bool CommonOperation::publish(std::span<NumericOperationOutput> out,const NumericOperationInput& input) const noexcept {
    for(auto& o:out){
        if(!configured_axes(definition(descriptor_->node),o) || !copy_axes(input,o,!o.port && descriptor_->node.kind=="FilterMatrixProjection"))return false;
        auto stamp=output_stamp(input,o.value.contract->clock,descriptor_->node.kind=="FeatureAggregate" && o.value.contract->shape.rank==Rank::Scalar);
        stamp.available=true;o.stamp=stamp;
        const auto domain=descriptor_->policy.output_values[o.port];
        if(validate_view({stamp,o.value.view()},limits_.frame)!=Status::ok || validate_values(o.value.view(),domain,limits_.frame)!=Status::ok)return false;
    }
    for(auto& o:out)o.status=Status::ok;
    return true;
}
namespace {
void mark_common_unavailable(std::span<NumericOperationOutput> outputs) noexcept {
    for(auto& output:outputs){output.status=Status::unavailable;output.stamp.available=false;}
}
}

Status CommonOperation::process(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,NumericOperationControl control) noexcept {
    mark_common_unavailable(out);
    const auto status=preflight(in,out);if(status!=Status::ok)return status;
    if(control.reset)reset_kernel();
    if(control.enabled){if(!run(in,out))return Status::invalid_argument;}
    else if(!fill_disabled_common(descriptor_->node,in[0],out,limits_.frame))return Status::invalid_argument;
    return publish(out,in.front())?Status::ok:Status::invalid_argument;
}
}
