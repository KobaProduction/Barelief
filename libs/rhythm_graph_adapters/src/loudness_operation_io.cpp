// SPDX-License-Identifier: AGPL-3.0-or-later
#include "loudness_operations.h"
#include "frame_operation_io.h"
#include <bit>
#include <algorithm>
#include <cmath>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace dataflow_runtime;
namespace {
Status validate_loudness_proof(const NumericOperationInput& input) noexcept {
    if(input.contiguous_float32.size()!=input.value.actual[0])return Status::incompatible_contract;
    for(std::size_t k=0;k<input.contiguous_float32.size();++k){
        float value=0;if(load_element(input.value,{0,k,0},value)!=Status::ok || std::bit_cast<std::uint32_t>(value)!=std::bit_cast<std::uint32_t>(input.contiguous_float32[k]))return Status::incompatible_contract;
    }
    return Status::ok;
}
Status validate_input(const NumericOperationInput& input,const dataflow_runtime::NumericOperationPort& expected,const FrameLimits& limits) noexcept {
    if(input.port!=expected.id || !input.value.contract || !same_contract(*input.value.contract,expected.contract.view()))return Status::incompatible_contract;
    if(input.status!=Status::ok)return Status::unavailable;
    if(validate_view(input.value,limits)!=Status::ok || validate_values(input.value,ValueDomain::FiniteOnly,limits)!=Status::ok)return Status::invalid_argument;
    if(input.value.contract->shape.rank==Rank::Matrix)return Status::ok;
    return validate_loudness_proof(input);
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

Status loudness_silence(std::span<NumericOperationOutput> out,const FrameLimits& limits) noexcept {
    for(auto& output:out){
        output.value.actual={std::size_t(output.value.contract->shape.extents[0].maximum),std::size_t(output.value.contract->shape.extents[1].maximum)};
        if(fill_silence(output.value,limits)!=Status::ok)return Status::invalid_argument;
    }
    return Status::ok;
}
Status publish(std::span<NumericOperationOutput> out,const dataflow_runtime::NumericOperationOrigin& source,const LoudnessPlan& plan,const FrameLimits& limits) noexcept {
    auto stamp=batch_stamp(source);
    for(auto& o:out){
        o.origin=stamp;
        if(validate_view(o.value.view(),limits)!=Status::ok || validate_values(o.value.view(),plan.shape[o.port].values,limits)!=Status::ok){invalidate_outputs(out);return Status::invalid_argument;}
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
    if(control.reset)reset_kernel();
    if(control.enabled){if(!run(in,out))return Status::invalid_argument;}
    else if(loudness_silence(out,limits_)!=Status::ok)return Status::invalid_argument;
    return publish(out,in[0].origin,plan_,limits_);
}
}
