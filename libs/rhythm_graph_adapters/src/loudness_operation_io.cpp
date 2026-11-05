// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loudness_operations.h"
#include "frame_operation_io.h"
#include <bit>
#include <algorithm>
#include <cmath>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace dataflow_runtime;
namespace {
Status validate_input(const NumericOperationInput& input,const domain::ProcessingPort& expected,const FrameLimits& limits) noexcept {
    if(input.port!=expected.id || !input.value.contract || !same_contract(*input.value.contract,expected.contract.view()))return Status::incompatible_contract;
    if(input.status!=Status::ok || !input.stamp.available)return Status::unavailable;
    if(validate_view({input.stamp,input.value},limits)!=Status::ok || validate_values(input.value,ValueDomain::FiniteOnly,limits)!=Status::ok)return Status::invalid_argument;
    if(input.value.contract->shape.rank==Rank::Matrix)return Status::ok;
    if(input.contiguous_float32.size()!=input.value.actual[0])return Status::incompatible_contract;
    for(std::size_t k=0;k<input.contiguous_float32.size();++k){
        float value=0;if(load_element(input.value,{0,k,0},value)!=Status::ok || std::bit_cast<std::uint32_t>(value)!=std::bit_cast<std::uint32_t>(input.contiguous_float32[k]))return Status::incompatible_contract;
    }
    return Status::ok;
}
Status validate_output(NumericOperationOutput& out,const FrameLimits& limits) noexcept {
    return out.value.contract?validate_mutable(out.value,limits):Status::invalid_argument;
}
Status preflight(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,const NumericOperationDescriptor& descriptor,const FrameLimits& limits) noexcept {
    if(in.size()!=descriptor.node.inputs.size() || out.size()!=descriptor.node.outputs.size() || !proof_buffers_disjoint(in,out) || !disjoint_operation_buffers(in,out))return Status::incompatible_contract;
    for(std::size_t k=0;k<in.size();++k){const auto status=validate_input(in[k],descriptor.node.inputs[k],limits);if(status!=Status::ok)return status;}
    for(std::size_t k=0;k<out.size();++k)if(out[k].port!=k || validate_output(out[k],limits)!=Status::ok)return Status::invalid_argument;
    return Status::ok;
}
bool axis_matches(const NumericOperationInput& input,std::size_t axis,NumericUnit unit,double step) noexcept {
    const auto& coordinates=input.value.contract->coordinates[axis];if(coordinates.unit!=unit || coordinates.mode==CoordinateMode::None)return true;
    if(coordinates.mode==CoordinateMode::Regular)return coordinates.origin.numerator==0 && coordinates.step.denominator &&
        std::abs(double(coordinates.step.numerator)/double(coordinates.step.denominator)-step)<=1e-6*std::max(1.,std::abs(step));
    const auto bytes=input.value.coordinates.axes[axis];
    for(std::size_t k=0;k<input.value.actual[axis];++k){
        std::uint64_t bits=0;for(std::size_t b=0;b<8;++b)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[k*8+b]))<<(b*8);
        const double actual=std::bit_cast<double>(bits),expected=double(k)*step;
        if(!std::isfinite(actual) || std::abs(actual-expected)>1e-6*std::max(1.,std::abs(expected)))return false;
    }
    return true;
}
bool source_axes(std::span<const NumericOperationInput> in,const LoudnessPlan& p) noexcept {
    if(p.kind==LoudnessKind::Transform)return axis_matches(in[0],0,NumericUnit::Seconds,1./double(p.analysis_rate));
    if(p.kind==LoudnessKind::Ratios || p.kind==LoudnessKind::Frequency)return axis_matches(in[0],0,NumericUnit::Hertz,double(p.rate)/(2.*double(std::max(std::size_t{1},p.rows-1))));
    if(p.kind==LoudnessKind::LoopFit || p.kind==LoudnessKind::LoopTempo || p.kind==LoudnessKind::Segment || p.kind==LoudnessKind::Sequence || p.kind==LoudnessKind::Activity)
        return axis_matches(in[0],0,NumericUnit::Seconds,1./double(p.rate));
    return true;
}
Status publish(std::span<NumericOperationOutput> out,const FrameStamp& source,const LoudnessPlan& plan,const FrameLimits& limits) noexcept {
    auto stamp=batch_stamp(source);stamp.available=true;
    for(auto& o:out){
        o.stamp=stamp;
        if(validate_view({stamp,o.value.view()},limits)!=Status::ok || validate_values(o.value.view(),plan.shape[o.port].values,limits)!=Status::ok){invalidate_outputs(out);return Status::invalid_argument;}
    }
    for(auto& o:out)o.status=Status::ok;return Status::ok;
}
}
bool loudness_shape(NumericOperationOutput& o,std::size_t rows,std::size_t columns) noexcept {
    if(!o.value.contract)return false;const auto& shape=o.value.contract->shape;
    if(rows<shape.extents[0].minimum || rows>shape.extents[0].maximum || columns<shape.extents[1].minimum || columns>shape.extents[1].maximum)return false;
    o.value.actual={rows,columns};return true;
}
bool loudness_matrix_row(void* context,std::size_t row,std::span<const float> values) noexcept {
    auto& out=*static_cast<NumericOperationOutput*>(context);if(row>=out.value.actual[0] || values.size()!=out.value.actual[1])return false;
    for(std::size_t k=0;k<values.size();++k)if(store_element(out.value,{0,row,k},values[k])!=Status::ok)return false;return true;
}
bool loudness_indices(NumericOperationOutput& out,std::span<const std::size_t> values) noexcept {
    if(!loudness_shape(out,values.size()))return false;
    for(std::size_t k=0;k<values.size();++k)if(store_element(out.value,{0,k,0},std::uint64_t(values[k]))!=Status::ok)return false;return true;
}
bool loudness_complex(NumericOperationOutput& out,std::span<const float> values) noexcept {
    if(values.size()%2 || !loudness_shape(out,values.size()/2,2))return false;
    for(std::size_t k=0;k<values.size();++k)if(store_element(out.value,{0,k/2,k%2},values[k])!=Status::ok)return false;return true;
}
Status LoudnessOperation::process(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,NumericOperationControl control) noexcept {
    invalidate_outputs(out);if(!descriptor_ || !output_contracts_match(out))return Status::incompatible_contract;
    const auto status=preflight(in,out,*descriptor_,limits_);if(status!=Status::ok)return status;
    if(!source_axes(in,plan_))return Status::incompatible_contract;
    if(control.reset)reset_kernel();
    if(control.enabled){if(!run(in,out))return Status::invalid_argument;}
    else for(auto& o:out){o.value.actual={std::size_t(o.value.contract->shape.extents[0].maximum),std::size_t(o.value.contract->shape.extents[1].maximum)};if(fill_silence(o.value,limits_)!=Status::ok)return Status::invalid_argument;}
    return publish(out,in[0].stamp,plan_,limits_);
}
}
