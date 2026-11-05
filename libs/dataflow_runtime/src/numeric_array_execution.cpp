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
template<class Values> double product_value(const Values& values,std::size_t count,std::size_t factors,std::size_t sample) noexcept {
    double product=1;
    for(std::size_t factor=0;factor<factors;++factor)product*=values[factor*count+sample];
    if(std::isfinite(product) && product!=0)return product;
    double mantissa=1;int exponent=0;
    for(std::size_t factor=0;factor<factors;++factor){
        int shift=0;mantissa*=std::frexp(double(values[factor*count+sample]),&shift);exponent+=shift;
    }
    return std::ldexp(mantissa,exponent);
}
double multiply_factors(const OperationKernelState& kernel,std::size_t sample) noexcept {
    return kernel.factors64.empty()?product_value(kernel.factors,kernel.input.size(),kernel.factor_ready.size(),sample):
        product_value(kernel.factors64,kernel.input.size(),kernel.factor_ready.size(),sample);
}
template<class Values> double sum_value(const Values& values,const OperationKernelState& kernel,std::size_t sample) noexcept {
    const auto count=kernel.input.size();double value=0;
    for(std::size_t factor=0;factor<kernel.factor_ready.size();++factor)value+=values[factor*count+sample];
    if(!kernel.clamp_floor)return value;
    return values[kernel.frame_input*count+sample]<=kernel.minimum?kernel.minimum:std::max(value,kernel.minimum);
}
double add_factors(const OperationKernelState& kernel,std::size_t sample) noexcept {
    return kernel.factors64.empty()?sum_value(kernel.factors,kernel,sample):sum_value(kernel.factors64,kernel,sample);
}
Status store_product_value(MutableNumericView& output,std::size_t sample,double value) noexcept {
    const ElementIndex index{0,sample/output.actual[1],sample%output.actual[1]};
    if(!std::isfinite(value))return Status::overflow;
    if(output.contract->dtype==DType::Float64)return store_element(output,index,value);
    return std::abs(value)>std::numeric_limits<float>::max()?Status::overflow:store_element(output,index,float(value));
}
Status render_product(OperationKernelState& kernel,const PlanNode& node,State& state) noexcept {
    auto output=mutable_output(node,state,0);output.actual=kernel.product_actual;
    for(std::size_t sample=0;sample<output.actual[0]*output.actual[1];++sample){
        const double value=kernel.add_arrays?add_factors(kernel,sample):multiply_factors(kernel,sample);
        const auto status=store_product_value(output,sample,value);if(status!=Status::ok){kernel.product_pending=false;return status;}
    }
    return Status::ok;
}
Status retain_product_coordinates(State& state,const NumericView& source) noexcept {
    for(std::size_t axis=0;axis<2;++axis)if(source.contract->coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE){
        auto& coordinates=state.numeric_outputs[0].coordinates[axis];const auto bytes=source.coordinates.axes[axis];
        if(bytes.size()!=source.actual[axis]*sizeof(double) || bytes.size()>coordinates.size())return Status::incompatible_contract;
        std::copy(bytes.begin(),bytes.end(),coordinates.begin());
    }
    return Status::ok;
}
Status retain_product_frame(OperationKernelState& kernel,State& state,const DataFrameView& frame,const NumericView& source) noexcept {
    const auto status=retain_product_coordinates(state,source);if(status!=Status::ok)return status;
    if(frame.stamp.stream_id.size()>kernel.data_stream.size())return Status::exhausted;
    std::copy(frame.stamp.stream_id.begin(),frame.stamp.stream_id.end(),kernel.data_stream.begin());
    kernel.stream_size=frame.stamp.stream_id.size();kernel.product_stamp=frame.stamp;kernel.product_stamp.stream_id={};kernel.product_pending=true;kernel.product_actual=source.actual;
    return Status::ok;
}
Status load_product_value(OperationKernelState& kernel,std::size_t destination,const NumericView& source,std::size_t index) noexcept {
    if(kernel.factors64.empty())return load_element(source,{0,index/source.actual[1],index%source.actual[1]},kernel.factors[destination]);
    if(source.contract->dtype==DType::Float64)return load_element(source,{0,index/source.actual[1],index%source.actual[1]},kernel.factors64[destination]);
    float value=0;const auto status=load_element(source,{0,index/source.actual[1],index%source.actual[1]},value);
    kernel.factors64[destination]=value;return status;
}
Status load_product_lane(OperationKernelState& kernel,std::size_t lane,const NumericView& source) noexcept {
    const auto count=kernel.input.size();
    const bool scalar=source.contract->shape.rank==Rank::Scalar;
    const auto actual=source.actual[0]*source.actual[1];
    const auto loaded=source.contract->shape.rank==Rank::Matrix?actual:count;
    for(std::size_t sample=0;sample<loaded;++sample){
        const auto index=scalar?0:sample%actual;
        const auto status=load_product_value(kernel,lane*count+sample,source,index);if(status!=Status::ok)return status;
    }
    kernel.factor_actual[lane]=source.contract->shape.rank==Rank::Matrix?source.actual:std::array<std::size_t,2>{};
    kernel.factor_ready[lane]=true;return Status::ok;
}
Status validate_product_lane(OperationKernelState& kernel,std::size_t lane,const DataFrameView& frame,const FrameLimits& limits) noexcept {
    if(lane>=kernel.factor_ready.size())return Status::invalid_argument;
    Status status=Status::unavailable;
    if(frame.stamp.available){
        const auto* source=std::get_if<NumericView>(&frame.payload);
        status=!source || !source->actual[1] || source->actual[0]>kernel.input.size()/source->actual[1]?Status::incompatible_contract:validate_values(*source,ValueDomain::FiniteOnly,limits);
    }
    if(status!=Status::ok){kernel.factor_ready[lane]=false;kernel.product_pending=false;}
    return status;
}
MutableNumericView product_output(const OperationKernelState& kernel,const PlanNode& node,State& state) noexcept {
    auto output=mutable_output(node,state,0);output.actual=kernel.product_actual;
    for(std::size_t axis=0;axis<2;++axis)if(output.contract->coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE)
        output.coordinates.axes[axis]={state.numeric_outputs[0].coordinates[axis].data(),output.actual[axis]*sizeof(double)};
    return output;
}
Status publish_product(OperationKernelState& kernel,const PlanNode& node,State& state) noexcept {
    if(!kernel.product_pending || std::find(kernel.factor_ready.begin(),kernel.factor_ready.end(),0)!=kernel.factor_ready.end())return Status::would_block;
    for(const auto actual:kernel.factor_actual)if(actual[0] && actual!=kernel.product_actual)return Status::incompatible_contract;
    const auto status=render_product(kernel,node,state);if(status!=Status::ok)return status;
    auto stamp=kernel.product_stamp;
    stamp.stream_id={kernel.data_stream.data(),kernel.stream_size};kernel.product_pending=false;
    state.numeric_frames[0]={stamp,product_output(kernel,node,state).view()};return Status::ok;
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
