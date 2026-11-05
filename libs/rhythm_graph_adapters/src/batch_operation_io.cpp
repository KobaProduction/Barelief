// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include "operation_io.h"
#include "frame_operation_io.h"
#include <cmath>
#include <bit>
#include <cstring>
#include <algorithm>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace dataflow_runtime;
namespace {
bool read_row(const void* context,std::size_t row,std::span<float> values) noexcept {
    const auto& in=*static_cast<const NumericOperationInput*>(context);
    if(row>=in.value.actual[0] || values.size()!=in.value.actual[1])return false;
    for(std::size_t col=0;col<values.size();++col)if(load_element(in.value,{0,row,col},values[col])!=Status::ok)return false;
    return true;
}
bool shape(NumericOperationOutput& o,std::size_t rows,std::size_t columns=1) noexcept {
    const auto* c=o.value.contract;if(!c || rows<c->shape.extents[0].minimum || rows>c->shape.extents[0].maximum || columns<c->shape.extents[1].minimum || columns>c->shape.extents[1].maximum)return false;
    o.value.actual={rows,columns};return true;
}
bool coordinate_pair(const NumericOperationInput& a,const NumericOperationInput& b,bool spectrum) noexcept {
    for(std::size_t axis=0;axis<(spectrum?2u:1u);++axis){
        const auto x=a.value.coordinates.axes[axis],y=b.value.coordinates.axes[axis];
        if(x.size()!=y.size() || !std::equal(x.begin(),x.end(),y.begin()))return false;
    }
    return true;
}
double coordinate_value(std::span<const std::byte> bytes,std::size_t index) noexcept {
    std::uint64_t bits=0;for(std::size_t i=0;i<8;++i)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[index*8+i]))<<(8*i);
    return std::bit_cast<double>(bits);
}
bool frequency_coordinates(const NumericOperationInput& input,float step) noexcept {
    const auto& axis=input.value.contract->coordinates[1];
    if(axis.mode!=CoordinateMode::ExplicitFloat64LE || axis.unit!=NumericUnit::Hertz)return true;
    const auto data=input.value.coordinates.axes[1];
    for(std::size_t bin=0;bin<input.value.actual[1];++bin){
        const double actual=coordinate_value(data,bin),expected=double(bin)*step;
        if(!std::isfinite(actual) || std::abs(actual-expected)>1e-6*std::max(1.,std::abs(expected)))return false;
    }
    return true;
}
Status validate_batch_proof(const NumericOperationInput& input) noexcept {
    if(input.contiguous_float32.size()!=input.value.actual[0])return Status::incompatible_contract;
    for(std::size_t k=0;k<input.contiguous_float32.size();++k){
        float v=0;if(load_element(input.value,{0,k,0},v)!=Status::ok || std::bit_cast<std::uint32_t>(v)!=std::bit_cast<std::uint32_t>(input.contiguous_float32[k]))return Status::incompatible_contract;
    }
    return Status::ok;
}
Status validate_input(const NumericOperationInput& input,const domain::ProcessingPort& expected,const FrameLimits& limits) noexcept {
    if(input.port!=expected.id || !input.value.contract || !same_physical_contract(*input.value.contract,expected.contract.view()))return Status::incompatible_contract;
    if(input.status!=Status::ok || !input.stamp.available)return Status::unavailable;
    if(validate_view({input.stamp,input.value},limits)!=Status::ok || validate_values(input.value,ValueDomain::FiniteOnly,limits)!=Status::ok)return Status::invalid_argument;
    if(input.value.contract->dtype!=DType::Float32 || input.value.contract->shape.rank!=Rank::Vector)return Status::ok;
    return validate_batch_proof(input);
}
Status validate_batch_outputs(std::span<NumericOperationOutput> outputs,const FrameLimits& limits) noexcept {
    for(const auto& output:outputs) {
        if(!output.value.contract || output.value.planes.size()!=1)return Status::invalid_argument;
        const auto status=validate_mutable(output.value,limits);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status fill_disabled_batch(std::span<NumericOperationOutput> outputs,const FrameLimits& limits) noexcept {
    for(auto& output:outputs){
        output.value.actual={std::size_t(output.value.contract->shape.extents[0].maximum),std::size_t(output.value.contract->shape.extents[1].maximum)};
        const auto status=fill_silence(output.value,limits);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status publish(std::span<NumericOperationOutput> out,const NumericOperationInput& input,const BatchPlan& plan,const FrameLimits& limits) noexcept {
    for(auto& o:out){
        auto stamp=plan.kind==BatchKind::PulseOnset?input.stamp:batch_stamp(input.stamp);stamp.available=true;o.stamp=stamp;
        if(validate_view({stamp,o.value.view()},limits)!=Status::ok || validate_values(o.value.view(),plan.shape[o.port].values,limits)!=Status::ok){invalidate_outputs(out);return Status::invalid_argument;}
    }
    for(auto& o:out)o.status=Status::ok;
    return Status::ok;
}
}
dsp_kernels::FloatMatrixSource batch_rows(const NumericOperationInput& in) noexcept {return {&in,read_row};}
bool batch_scalar(NumericOperationOutput& o,float value) noexcept {return shape(o,1) && store_element(o.value,{0,0,0},value)==Status::ok;}
bool batch_integer(NumericOperationOutput& o,std::uint64_t value) noexcept {return shape(o,1) && store_element(o.value,{0,0,0},value)==Status::ok;}
bool batch_status(NumericOperationOutput& o,std::uint8_t value) noexcept {return shape(o,1) && store_element(o.value,{0,0,0},value)==Status::ok;}
bool batch_vector(NumericOperationOutput& o,std::span<const float> values) noexcept {
    if(!shape(o,values.size()))return false;
    for(std::size_t i=0;i<values.size();++i)if(store_element(o.value,{0,i,0},values[i])!=Status::ok)return false;
    return true;
}
bool batch_peaks(NumericOperationOutput& o,std::span<const rhythm_reference_ports::InterpolatedPeak> peaks) noexcept {
    if(!shape(o,peaks.size(),2))return false;
    for(std::size_t i=0;i<peaks.size();++i)if(store_element(o.value,{0,i,0},peaks[i].position)!=Status::ok || store_element(o.value,{0,i,1},peaks[i].value)!=Status::ok)return false;
    return true;
}
Status BatchOperation::preflight(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) const noexcept {
    if(!descriptor_ || in.size()!=descriptor_->node.inputs.size() || out.size()!=plan_.outputs || !output_contracts_match(out) || !proof_buffers_disjoint(in,out) || !disjoint_operation_buffers(in,out))return Status::incompatible_contract;
    for(std::size_t i=0;i<in.size();++i){const auto status=validate_input(in[i],descriptor_->node.inputs[i],limits_);if(status!=Status::ok)return status;}
    if((plan_.kind==BatchKind::FourierHistogram || plan_.kind==BatchKind::PhasePulse) &&
       (!coordinate_pair(in[0],in[1],plan_.kind==BatchKind::FourierHistogram) || !frequency_coordinates(in[0],plan_.histogram.path.frequency_step)))return Status::incompatible_contract;
    return validate_batch_outputs(out,limits_);
}
Status BatchOperation::process(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out,NumericOperationControl control) noexcept {
    invalidate_outputs(out);const auto valid=preflight(in,out);if(valid!=Status::ok)return valid;
    if(control.reset)reset_kernel();
    if(control.enabled){if(!run(in,out))return Status::invalid_argument;}
    else if(fill_disabled_batch(out,limits_)!=Status::ok)return Status::invalid_argument;
    return publish(out,in[0],plan_,limits_);
}
}
