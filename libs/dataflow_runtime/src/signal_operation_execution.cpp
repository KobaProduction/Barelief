#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace dataflow_runtime::detail {
using namespace domain;using namespace signal_contracts;
namespace {
bool same_interval(const FrameStamp& a,const FrameStamp& b) noexcept {
    return a.producer_epoch==b.producer_epoch && a.generation==b.generation && a.epoch==b.epoch &&
        a.graph_revision==b.graph_revision && a.timestamp_ticks==b.timestamp_ticks &&
        a.has_sample_interval==b.has_sample_interval && (!a.has_sample_interval || (a.sample_begin==b.sample_begin && a.sample_count==b.sample_count));
}
Status retain_plane(OperationKernelState& kernel,std::size_t lane,const DataFrameView& current,const FrameLimits& limits) noexcept {
    const auto* source=std::get_if<NumericView>(&current.payload);auto& target=lane?kernel.imaginary:kernel.input;
    if(!source || source->ring.capacity || source->actual[0]!=target.size()){kernel.pending={};return Status::incompatible_contract;}
    const auto valid=validate_values(*source,ValueDomain::FiniteOnly,limits);if(valid!=Status::ok){kernel.pending={};return valid;}
    for(std::size_t i=0;i<target.size();++i){const auto status=load_element(*source,{0,i,0},target[i]);if(status!=Status::ok){kernel.pending={};return status;}}
    kernel.stamps[lane]=current.stamp;kernel.stamps[lane].stream_id={};kernel.pending[lane]=true;return Status::ok;
}
Status await_pair(OperationKernelState& kernel,const PlanNode& node,std::size_t lane) noexcept {
    if(node.node.kind==NodeKind::RealFourier)return Status::ok;
    const auto other=1-lane;
    if(node.endpoint_plan->inputs[other].parent==absent){
        if(node.operation==ProcessingOperation::ComplexPolar){kernel.pending={};return Status::unavailable;}
        auto& target=other?kernel.imaginary:kernel.input;std::fill(target.begin(),target.end(),0);
        kernel.stamps[other]=kernel.stamps[lane];kernel.pending[other]=true;
    }
    if(!kernel.pending[0] || !kernel.pending[1])return Status::would_block;
    if(!same_interval(kernel.stamps[0],kernel.stamps[1])){kernel.pending[other]=false;return Status::would_block;}
    return Status::ok;
}
FrameStamp combined_stamp(const OperationKernelState& kernel,const DataFrameView& current,bool paired) noexcept {
    auto result=current.stamp;if(!paired)return result;
    result.discontinuity=kernel.stamps[0].discontinuity || kernel.stamps[1].discontinuity;
    result.frequency_dependent_latency=kernel.stamps[0].frequency_dependent_latency || kernel.stamps[1].frequency_dependent_latency;
    result.latency_seconds=latency_greater(kernel.stamps[0].latency_seconds,kernel.stamps[1].latency_seconds)?kernel.stamps[0].latency_seconds:kernel.stamps[1].latency_seconds;
    return result;
}
Status project_polar(OperationKernelState& kernel,const PlanNode& node) noexcept {
    for(std::size_t i=0;i<kernel.input.size();++i){
        if(!kernel.spectrum_scaling.project(kernel.input[i],kernel.imaginary[i],node.half_last && i+1<kernel.input.size(),kernel.output[2*i]))return Status::overflow;
        kernel.output[2*i+1]=(kernel.input[i]!=0 || kernel.imaginary[i]!=0)?float(std::atan2(double(kernel.imaginary[i]),double(kernel.input[i]))):0;
    }
    return Status::ok;
}
Status compute_paired(OperationKernelState& kernel,const PlanNode& node) noexcept {
    if(!node.node.enabled){std::fill(kernel.output.begin(),kernel.output.end(),0);return Status::ok;}
    if(node.operation==ProcessingOperation::ArrayConcat){
        std::copy(kernel.input.begin(),kernel.input.end(),kernel.output.begin());
        std::copy(kernel.imaginary.begin(),kernel.imaginary.end(),kernel.output.begin()+kernel.input.size());return Status::ok;
    }
    if(node.operation==ProcessingOperation::ComplexPolar)return project_polar(kernel,node);
    const bool complex=node.node.kind!=NodeKind::RealFourier;
    const bool ready=complex?std::get<dsp_kernels::FFT>(kernel.operation).process_complex(kernel.input,kernel.imaginary,kernel.output):std::get<dsp_kernels::PreparedRfft>(kernel.operation).process(kernel.input,kernel.output);
    return ready?Status::ok:Status::overflow;
}
Status publish_planes(OperationKernelState& kernel,const PlanNode& node,State& state,const FrameStamp& stamp) noexcept {
    const bool concat=node.operation==ProcessingOperation::ArrayConcat,polar=node.operation==ProcessingOperation::ComplexPolar;
    for(PortId port=0;port<(concat?1:polar?2:4);++port){
        auto output=mutable_output(node,state,port);const auto count=port<2?output.actual[0]:1;
        for(std::size_t i=0;i<count;++i){
            const auto index=concat?i:polar?2*i+port:port<2?2*(i+1)+port:port-2;
            const auto status=store_element(output,{0,i,0},kernel.output[index]);if(status!=Status::ok)return status;
        }
        state.numeric_frames[port]={stamp,output.view()};
    }
    return Status::ok;
}
}
Status accept_fourier(const PlanNode& node,State& state,std::size_t lane,const DataFrameView& current,const FrameLimits& limits) noexcept {
    auto& kernel=*std::get<PreparedOperationKernel>(state.kernel).value_;
    if(node.operation==ProcessingOperation::ArrayProduct)return accept_array_product(kernel,node,state,lane,current,limits);
    if(lane>=(node.node.kind==NodeKind::RealFourier?1u:2u))return Status::invalid_argument;
    if(!current.stamp.available){kernel.pending={};return Status::unavailable;}
    const auto retained=retain_plane(kernel,lane,current,limits);if(retained!=Status::ok)return retained;
    const auto paired=await_pair(kernel,node,lane);if(paired!=Status::ok)return paired;
    const auto stamp=combined_stamp(kernel,current,node.node.kind!=NodeKind::RealFourier);kernel.pending={};
    const auto computed=compute_paired(kernel,node);if(computed!=Status::ok)return computed;
    return publish_planes(kernel,node,state,stamp);
}
}
