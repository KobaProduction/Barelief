#include "numeric_operation_bridge.h"
#include <bit>
#include <cmath>

namespace dataflow_runtime::detail {
using namespace signal_contracts;
void NumericOperationBindingAccess::read_identity(NumericOperationOrigin& out,const FrameStamp& in) noexcept {
    auto& s=out.identity_;s.sequence=in.sequence;s.source_sequence=in.source_sequence;s.generation=in.generation;
    s.epoch=in.epoch;s.graph_revision=in.graph_revision;s.producer_epoch=in.producer_epoch;s.discontinuity=in.discontinuity;
}
void NumericOperationBindingAccess::read_timing(NumericOperationOrigin& out,const FrameStamp& in) noexcept {
    auto& s=out.identity_;s.timestamp=in.timestamp_ticks;s.sample_begin=in.sample_begin;s.sample_count=in.sample_count;
    s.interval=in.has_sample_interval;s.latency_samples=in.latency_samples;s.latency_numerator=in.latency_seconds.numerator;
    s.latency_denominator=in.latency_seconds.denominator;s.frequency_dependent_latency=in.frequency_dependent_latency;
}
void NumericOperationBindingAccess::write_identity(const NumericOperationOrigin& in,FrameStamp& out) noexcept {
    const auto& s=in.identity_;out.sequence=s.sequence;out.source_sequence=s.source_sequence;out.generation=s.generation;
    out.epoch=s.epoch;out.graph_revision=s.graph_revision;out.producer_epoch=s.producer_epoch;out.discontinuity=s.discontinuity;
}
void NumericOperationBindingAccess::write_timing(const NumericOperationOrigin& in,FrameStamp& out) noexcept {
    const auto& s=in.identity_;out.timestamp_ticks=s.timestamp;out.sample_begin=s.sample_begin;out.sample_count=s.sample_count;
    out.has_sample_interval=s.interval;out.latency_samples=s.latency_samples;out.latency_seconds={s.latency_numerator,s.latency_denominator};
    out.frequency_dependent_latency=s.frequency_dependent_latency;
}
NumericOperationOrigin NumericOperationBindingAccess::origin(const NumericOperationInvocation& call,std::size_t index) noexcept {
    NumericOperationOrigin result;read_identity(result,call.inputs[index].stamp);read_timing(result,call.inputs[index].stamp);
    result.clock_=call.clocks[index];result.authority_=call.authority;result.invocation_=call.generation;
    result.coordinate_group_=call.coordinate_groups[index];return result;
}
NumericOperationOrigin NumericOperationBindingAccess::publication(NumericOperationOrigin origin,NumericOperationPublication mode) noexcept {
    origin.publication_=mode;return origin;
}
bool NumericOperationBindingAccess::same_session(const NumericOperationOrigin& a,const NumericOperationOrigin& b) noexcept {
    const auto& x=a.identity_;const auto& y=b.identity_;
    return a.authority_==b.authority_ && x.producer_epoch==y.producer_epoch && x.generation==y.generation &&
        x.epoch==y.epoch && x.graph_revision==y.graph_revision;
}
bool NumericOperationBindingAccess::same_step(const NumericOperationOrigin& a,const NumericOperationOrigin& b) noexcept {
    const auto& x=a.identity_;const auto& y=b.identity_;
    return same_session(a,b) && x.sequence==y.sequence && x.source_sequence==y.source_sequence && x.timestamp==y.timestamp &&
        x.interval==y.interval && (!x.interval || (x.sample_begin==y.sample_begin && x.sample_count==y.sample_count));
}
bool NumericOperationBindingAccess::same_coordinates(const NumericOperationOrigin& a,const NumericOperationOrigin& b) noexcept {
    return a.authority_==b.authority_ && a.invocation_==b.invocation_ && a.coordinate_group_==b.coordinate_group_;
}
NumericOperationCoordinateWriter NumericOperationBindingAccess::writer(const NumericOperationInvocation& call,std::size_t index) noexcept {
    NumericOperationCoordinateWriter result;result.lease_=call.lease;result.generation_=call.generation;result.output_=index;
    for(std::size_t axis=0;axis<2;++axis)if(call.outputs[index].contract->coordinates[axis].mode==CoordinateMode::ExplicitFloat64LE)result.axes_|=1u<<axis;
    return result;
}
bool NumericOperationBindingAccess::valid_writer(const NumericOperationCoordinateWriter& writer,const NumericOperationInvocation& call,std::size_t output) noexcept {
    return writer.lease_ && writer.lease_==call.lease && writer.lease_->active==&call && writer.generation_==call.generation && writer.output_==output;
}
Status NumericOperationBindingAccess::write(NumericOperationOutput& output,std::size_t axis,std::size_t index,double value) noexcept {
    const auto& writer=output.coordinates;const auto* call=writer.lease_?writer.lease_->active:nullptr;
    if(!call || !valid_writer(writer,*call,writer.output_) || writer.output_>=call->output_buffers.size() ||
       &call->output_buffers[writer.output_]!=&output || !writer.available(axis) || !std::isfinite(value))return Status::invalid_argument;
    const auto bytes=call->coordinate_storage[writer.output_][axis];if(index>=bytes.size()/8)return Status::buffer_too_small;
    const auto bits=std::bit_cast<std::uint64_t>(value);
    for(std::size_t byte=0;byte<8;++byte)bytes[index*8+byte]=std::byte((bits>>(byte*8))&255);
    return Status::ok;
}
Status NumericOperationBindingAccess::axis(NumericOperationOutput& output,std::size_t axis,std::size_t count) noexcept {
    const auto& writer=output.coordinates;auto* call=writer.lease_?writer.lease_->active:nullptr;
    if(!call || !valid_writer(writer,*call,writer.output_) || writer.output_>=call->output_buffers.size() ||
       &call->output_buffers[writer.output_]!=&output || !writer.available(axis))return Status::invalid_argument;
    if(count!=output.value.actual[axis] || count>call->coordinate_storage[writer.output_][axis].size()/8)return Status::buffer_too_small;
    call->coordinate_counts[writer.output_][axis]=count;return Status::ok;
}
namespace {
bool clock_matches(const NumericOperationClock& a,const ClockContract& b) noexcept {
    return a.domain==b.domain && a.tick==b.tick_seconds && a.sample_rate==b.sample_rate;
}
}
Status NumericOperationBindingAccess::stamp(const NumericOperationOrigin& origin,const ClockContract& output,
    const std::shared_ptr<const NumericOperationAuthority>& owner,FrameStamp& stamp) noexcept {
    if(!origin.authority_ || origin.authority_!=owner || !origin.clock_ ||
       unsigned(origin.publication_)>unsigned(NumericOperationPublication::DisabledFrame))return Status::incompatible_contract;
    stamp={};write_identity(origin,stamp);
    if(clock_matches(*origin.clock_,output)){
        if(origin.publication_==NumericOperationPublication::Frame || origin.publication_==NumericOperationPublication::Point)write_timing(origin,stamp);
        if(origin.publication_==NumericOperationPublication::DisabledFrame)stamp.timestamp_ticks=origin.identity_.timestamp;
    }
    if(origin.publication_==NumericOperationPublication::Point){stamp.has_sample_interval=false;stamp.sample_begin=0;stamp.sample_count=0;}
    return Status::ok;
}
} // namespace dataflow_runtime::detail
namespace dataflow_runtime {
NumericOperationOrigin numeric_operation_origin(NumericOperationOrigin origin,NumericOperationPublication mode) noexcept {return detail::NumericOperationBindingAccess::publication(std::move(origin),mode);}
bool numeric_operation_same_session(const NumericOperationOrigin& a,const NumericOperationOrigin& b) noexcept {return detail::NumericOperationBindingAccess::same_session(a,b);}
bool numeric_operation_same_step(const NumericOperationOrigin& a,const NumericOperationOrigin& b) noexcept {return detail::NumericOperationBindingAccess::same_step(a,b);}
bool numeric_operation_same_coordinates(const NumericOperationOrigin& a,const NumericOperationOrigin& b) noexcept {return detail::NumericOperationBindingAccess::same_coordinates(a,b);}
signal_contracts::Status numeric_operation_coordinate(NumericOperationOutput& output,std::size_t axis,std::size_t index,double value) noexcept {return detail::NumericOperationBindingAccess::write(output,axis,index,value);}
signal_contracts::Status numeric_operation_axis(NumericOperationOutput& output,std::size_t axis,std::size_t count) noexcept {return detail::NumericOperationBindingAccess::axis(output,axis,count);}
}
