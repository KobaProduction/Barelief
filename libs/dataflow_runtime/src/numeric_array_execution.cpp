#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cmath>
#include <limits>
namespace dataflow_runtime::detail {
using namespace domain;using namespace signal_contracts;
namespace {
double multiply_factors(const OperationKernelState& kernel,std::size_t sample) noexcept {
    const auto count=kernel.input.size();double product=1;
    for(std::size_t factor=0;factor<kernel.factor_ready.size();++factor)product*=kernel.factors[factor*count+sample];
    if(std::isfinite(product) && product!=0)return product;
    double mantissa=1;int exponent=0;
    for(std::size_t factor=0;factor<kernel.factor_ready.size();++factor){
        int shift=0;mantissa*=std::frexp(double(kernel.factors[factor*count+sample]),&shift);exponent+=shift;
    }
    return std::ldexp(mantissa,exponent);
}
double add_factors(const OperationKernelState& kernel,std::size_t sample) noexcept {
    const auto count=kernel.input.size();double value=0;
    for(std::size_t factor=0;factor<kernel.factor_ready.size();++factor)value+=kernel.factors[factor*count+sample];
    if(!kernel.clamp_floor)return value;
    return kernel.factors[kernel.frame_input*count+sample]<=kernel.minimum?kernel.minimum:std::max(value,kernel.minimum);
}
Status render_product(OperationKernelState& kernel,const PlanNode& node,State& state) noexcept {
    auto output=mutable_output(node,state,0);
    for(std::size_t sample=0;sample<kernel.input.size();++sample){
        const double value=kernel.add_arrays?add_factors(kernel,sample):multiply_factors(kernel,sample);
        if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max()){kernel.product_pending=false;return Status::overflow;}
        const auto status=store_element(output,{0,sample,0},float(value));if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status retain_product_coordinates(State& state,const NumericView& source) noexcept {
    for(std::size_t axis=0;axis<2;++axis)if(source.contract->coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
        auto& coordinates=state.numeric_outputs[0].coordinates[axis];const auto bytes=source.coordinates.axes[axis];
        if(bytes.size()!=coordinates.size())return Status::incompatible_contract;
        std::copy(bytes.begin(),bytes.end(),coordinates.begin());
    }
    return Status::ok;
}
Status retain_product_frame(OperationKernelState& kernel,State& state,const DataFrameView& frame,const NumericView& source) noexcept {
    const auto status=retain_product_coordinates(state,source);if(status!=Status::ok)return status;
    if(frame.stamp.stream_id.size()>kernel.data_stream.size())return Status::exhausted;
    std::copy(frame.stamp.stream_id.begin(),frame.stamp.stream_id.end(),kernel.data_stream.begin());
    kernel.stream_size=frame.stamp.stream_id.size();kernel.product_stamp=frame.stamp;kernel.product_stamp.stream_id={};kernel.product_pending=true;
    return Status::ok;
}
Status load_product_lane(OperationKernelState& kernel,std::size_t lane,const NumericView& source) noexcept {
    const auto count=kernel.input.size();
    for(std::size_t sample=0;sample<count;++sample){
        const auto status=load_element(source,{0,sample,0},kernel.factors[lane*count+sample]);if(status!=Status::ok)return status;
    }
    kernel.factor_ready[lane]=true;return Status::ok;
}
Status validate_product_lane(OperationKernelState& kernel,std::size_t lane,const DataFrameView& frame,const FrameLimits& limits) noexcept {
    if(lane>=kernel.factor_ready.size())return Status::invalid_argument;
    Status status=Status::unavailable;
    if(frame.stamp.available){
        const auto* source=std::get_if<NumericView>(&frame.payload);
        status=!source || source->ring.capacity || source->actual[0]!=kernel.input.size()?Status::incompatible_contract:validate_values(*source,ValueDomain::FiniteOnly,limits);
    }
    if(status!=Status::ok){kernel.factor_ready[lane]=false;kernel.product_pending=false;}
    return status;
}
Status publish_product(OperationKernelState& kernel,const PlanNode& node,State& state) noexcept {
    if(!kernel.product_pending || std::find(kernel.factor_ready.begin(),kernel.factor_ready.end(),0)!=kernel.factor_ready.end())return Status::would_block;
    const auto status=render_product(kernel,node,state);if(status!=Status::ok)return status;
    const auto output=mutable_output(node,state,0);auto stamp=kernel.product_stamp;
    stamp.stream_id={kernel.data_stream.data(),kernel.stream_size};kernel.product_pending=false;
    state.numeric_frames[0]={stamp,output.view()};return Status::ok;
}
Status accept_enabled_product(OperationKernelState& kernel,const PlanNode& node,State& state,std::size_t lane,const DataFrameView& frame) noexcept {
    const auto& source=std::get<NumericView>(frame.payload);
    const auto loaded=load_product_lane(kernel,lane,source);if(loaded!=Status::ok)return loaded;
    if(lane==kernel.frame_input){const auto retained=retain_product_frame(kernel,state,frame,source);if(retained!=Status::ok)return retained;}
    return publish_product(kernel,node,state);
}
}
Status accept_array_product(OperationKernelState& kernel,const PlanNode& node,State& state,std::size_t lane,const DataFrameView& frame,const FrameLimits& limits) noexcept {
    const auto valid=validate_product_lane(kernel,lane,frame,limits);if(valid!=Status::ok)return valid;
    if(!node.node.enabled){if(lane!=kernel.frame_input)return Status::would_block;state.numeric_frames[0]=frame;return Status::ok;}
    return accept_enabled_product(kernel,node,state,lane,frame);
}
}
