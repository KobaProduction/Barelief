// SPDX-License-Identifier: AGPL-3.0-or-later
#include "frame_operation_io.h"
#include <algorithm>
#include <functional>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
bool numeric_port(const domain::ResolvedProcessingNode& node,bool input,domain::PortId id,NumericContract& contract) noexcept {
    for(const auto& port:input?node.inputs:node.outputs){
        if(port.id!=id)continue;
        const auto data=port.contract.view();const auto* numeric=std::get_if<NumericContract>(&data);if(!numeric)return false;
        contract=*numeric;return true;
    }
    return false;
}
bool equal_frame_clock(const ClockContract& a,const ClockContract& b) noexcept {
    return a.domain==b.domain && a.tick_seconds==b.tick_seconds && a.sample_rate==b.sample_rate;
}
bool equal_frame_identity(const NumericOperationInput& a,const NumericOperationInput& b) noexcept {
    const auto& x=a.stamp;const auto& y=b.stamp;
    if(x.sequence!=y.sequence || x.source_sequence!=y.source_sequence || x.generation!=y.generation || x.epoch!=y.epoch || x.graph_revision!=y.graph_revision ||
       x.producer_epoch!=y.producer_epoch || x.timestamp_ticks!=y.timestamp_ticks || x.has_sample_interval!=y.has_sample_interval)return false;
    if(x.has_sample_interval && (x.sample_begin!=y.sample_begin || x.sample_count!=y.sample_count))return false;
    for(std::size_t axis=0;axis<2;++axis)
        if(a.value.coordinates.axes[axis].size()!=b.value.coordinates.axes[axis].size() ||
           !std::equal(a.value.coordinates.axes[axis].begin(),a.value.coordinates.axes[axis].end(),b.value.coordinates.axes[axis].begin()))return false;
    return true;
}
bool proof_buffers_disjoint(std::span<const NumericOperationInput> inputs,std::span<const NumericOperationOutput> outputs) noexcept {
    const std::less<const std::byte*> before;
    for(const auto& input:inputs){
        const auto bytes=std::as_bytes(input.contiguous_float32);if(bytes.empty())continue;
        for(const auto& output:outputs){
            const auto storage=output.value.storage;if(storage.empty())continue;
            if(before(bytes.data(),storage.data()+storage.size()) && before(storage.data(),bytes.data()+bytes.size()))return false;
        }
    }
    return true;
}
Status validate_frame_vector(const NumericOperationInput& input,const domain::ResolvedProcessingNode& node,
    std::size_t bins,const FrameLimits& limits,bool nonnegative) noexcept {
    NumericContract expected;if(!numeric_port(node,true,input.port,expected))return Status::incompatible_contract;
    const auto status=validate_vector(input,bins,expected,limits);if(status!=Status::ok)return status;
    if(input.contiguous_float32.size()!=bins)return Status::invalid_argument;
    if(nonnegative && std::any_of(input.contiguous_float32.begin(),input.contiguous_float32.end(),[](float x){return x<0;}))return Status::invalid_argument;
    return Status::ok;
}
Status validate_operation_control(const NumericOperationInput& input,const NumericContract& expected,const FrameLimits& limits) noexcept {
    if(input.status!=Status::ok || !input.stamp.available)return Status::ok;
    if(!input.value.contract || !same_physical_contract(*input.value.contract,expected))return Status::incompatible_contract;
    const auto status=validate_view(DataFrameView{input.stamp,input.value},limits);if(status!=Status::ok)return status;
    std::uint8_t value=0;const auto loaded=load_element(input.value,{0,0,0},value);
    return loaded==Status::ok && value>3?Status::invalid_argument:loaded;
}
FrameStamp operation_point_stamp(const NumericOperationInput* input) noexcept {
    auto stamp=operation_frame_stamp(input,true);stamp.has_sample_interval=false;stamp.sample_begin=0;stamp.sample_count=0;return stamp;
}
FrameStamp operation_frame_stamp(const NumericOperationInput* input,bool enabled) noexcept {
    if(!input)return {};
    if(enabled){auto stamp=input->stamp;stamp.stream_id={};return stamp;}
    auto stamp=batch_stamp(input->stamp);stamp.timestamp_ticks=input->stamp.timestamp_ticks;return stamp;
}
}
