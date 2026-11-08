#include "numeric_extension_state.h"
#include <algorithm>

namespace dataflow_runtime::detail {
namespace {
bool matches(const NumericInputLatch& first,const NumericInputLatch& second,NumericInputMatch policy) noexcept {
    if(policy==NumericInputMatch::IndependentBatch)return true;
    const auto& left=first.stamp;const auto& right=second.stamp;
    if(left.producer_epoch!=right.producer_epoch || left.generation!=right.generation || left.epoch!=right.epoch || left.graph_revision!=right.graph_revision)return false;
    if(policy==NumericInputMatch::SessionSequence)return left.source_sequence==right.source_sequence;
    if(left.has_sample_interval && !first.contract.clock.sample_rate)return false;
    return left.timestamp_ticks==right.timestamp_ticks && left.has_sample_interval==right.has_sample_interval &&
        (!left.has_sample_interval || (left.sample_begin==right.sample_begin && left.sample_count==right.sample_count));
}
bool flush(const NumericOperationDescriptor& descriptor,NumericOperationControl control) noexcept {
    return descriptor.policy.mode==NumericOperationMode::ContinuingStream && control.event==NumericOperationEvent::EndOfStream;
}
Status read_control(const NumericInputLatch& latch,NumericOperationControl& control) noexcept {
    std::uint8_t value=0;const auto input=latch.input();
    const auto status=load_element(input.value,{},value);if(status!=Status::ok)return status;
    if(value>3)return Status::invalid_argument;
    control.reset=value==3;
    control.event=value==1?NumericOperationEvent::BatchComplete:value==2?NumericOperationEvent::EndOfStream:NumericOperationEvent::Continue;
    return Status::ok;
}
}
bool PreparedNumericExtension::Impl::ready() const noexcept {
    if(!descriptor || !processor || !triggered)return false;
    const NumericInputLatch* anchor=nullptr;
    for(const auto& latch:latches){
        if(latch.rule.required && !latch.ready && !(flush(*descriptor,control) && latch.rule.use==NumericInputUse::Frame))return false;
        if(!latch.ready || latch.rule.use!=NumericInputUse::Frame)continue;
        if(anchor && !matches(*anchor,latch,descriptor->policy.match))return false;
        anchor=&latch;
    }
    return true;
}
namespace {
template<class Extension> Status apply_control(Extension& state,NumericInputLatch& latch) noexcept {
    if(!state.descriptor->policy.control || latch.rule.port!=*state.descriptor->policy.control)return Status::ok;
    const auto status=read_control(latch,state.control);if(status!=Status::ok){latch.ready=false;return status;}
    if(state.control.reset){state.reset();return Status::unavailable;}
    state.triggered=true;return Status::ok;
}
template<class Extension> void apply_trigger(Extension& state,const NumericInputLatch& latch) noexcept {
    if(latch.rule.port!=state.descriptor->policy.trigger)return;
    state.triggered=true;
    if(state.descriptor->policy.mode==NumericOperationMode::IndependentBatch)state.control.event=NumericOperationEvent::BatchComplete;
}
}
Status PreparedNumericExtension::Impl::retain(std::size_t lane,const DataFrameView& frame,const FrameLimits& limits) noexcept {
    if(lane>=latches.size() || !descriptor || !processor)return Status::invalid_argument;
    auto& latch=latches[lane];latch.ready=false;
    auto status=latch.retain(frame,limits);if(status!=Status::ok)return status;
    status=apply_control(*this,latch);if(status!=Status::ok)return status;
    apply_trigger(*this,latch);
    return ready()?Status::ok:Status::would_block;
}
void PreparedNumericExtension::Impl::clear_borrowed() noexcept {
    if(writer_lease)writer_lease->active=nullptr;
    invocation={};
    for(auto& input:inputs)input={};
    for(auto& output:outputs)output={};
}
void PreparedNumericExtension::Impl::consume() noexcept {
    for(auto& latch:latches)if(latch.rule.use!=NumericInputUse::HeldValue)latch.ready=false;
    triggered=false;control={};clear_borrowed();
}
void PreparedNumericExtension::Impl::reset() noexcept {
    if(processor)processor->reset();
    for(auto& latch:latches){latch.ready=false;latch.stream_size=0;latch.stamp={};}
    std::fill(output_statuses.begin(),output_statuses.end(),Status::unavailable);
    for(auto& stamp:output_stamps){stamp={};stamp.available=false;}
    triggered=false;control={};clear_borrowed();
}
Status PreparedNumericExtension::accept(const PlanNode& node,State& state,std::size_t lane,const DataFrameView& frame,const FrameLimits& limits) noexcept {
    const auto status=value_->retain(lane,frame,limits);
    if(status==Status::would_block)return status;
    if(status!=Status::ok){value_->consume();value_->invalidate(state,status);return status;}
    return value_->process(node,state,limits);
}
Status PreparedNumericExtension::output_status(PortId port) const noexcept {
    if(port>=value_->output_statuses.size())return Status::unavailable;
    const auto status=value_->output_statuses[port];
    return status==Status::ok && !value_->output_stamps[port].available?Status::unavailable:status;
}
} // namespace dataflow_runtime::detail
