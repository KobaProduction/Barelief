#include "transform_execution.h"
#include "frame_access.h"
#include "signal_time.h"
#include <algorithm>
namespace dataflow_runtime::detail {
namespace {
class FrameBufferExecution {
    const PlanNode& node;
    State& state;
    const DataFrameView& input;
    const NumericView& source;
    const TransformDelivery delivery;
    MutableNumericView output;
    template<typename Scalar> bool append(std::span<Scalar> samples,Scalar value) noexcept {
        samples[state.oldest]=value;state.oldest=(state.oldest+1)%samples.size();
        if(state.frames<samples.size())return ++state.frames==samples.size();
        if(++state.progress<node.frame_hop)return false;
        state.progress=0;return true;
    }
    template<typename Scalar> Status copy_samples(std::span<const Scalar> samples) noexcept {
        for(std::size_t i=0;i<samples.size();++i){
            const auto status=store_element(output,{0,i,0},samples[(state.oldest+i)%samples.size()]);
            if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status copy_frame() noexcept {
        return source.contract->dtype==DType::Int16?copy_samples(std::span<const std::int16_t>(state.pcm16)):copy_samples(std::span<const float>(state.pcm));
    }
    Status end_timestamp(std::size_t offset,FrameStamp& stamp) const noexcept {
        const auto& clock=source.contract->clock;if(!clock.sample_rate || clock.tick_seconds.numerator<=0)return Status::ok;
        std::uint64_t delta=0;
        if(!divide_products(offset,clock.tick_seconds.denominator,clock.sample_rate,std::uint64_t(clock.tick_seconds.numerator),delta) ||
           delta>std::uint64_t(INT64_MAX) || stamp.timestamp_ticks>INT64_MAX-std::int64_t(delta))return Status::overflow;
        stamp.timestamp_ticks+=std::int64_t(delta);return Status::ok;
    }
    Status frame_stamp(std::size_t offset,FrameStamp& stamp) noexcept {
        stamp=input.stamp;stamp.sequence=++state.row_ordinal;stamp.discontinuity=state.pending_discontinuity;state.pending_discontinuity=false;
        if(!stamp.has_sample_interval)return Status::ok;
        const auto advanced=std::int64_t(offset)+1,count=std::int64_t(output.actual[0]);
        if(stamp.sample_begin>INT64_MAX-advanced)return Status::overflow;
        const auto end=stamp.sample_begin+advanced;if(end<INT64_MIN+count)return Status::overflow;
        stamp.sample_begin=end-count;stamp.sample_count=std::uint64_t(count);return end_timestamp(offset,stamp);
    }
    Status emit(std::size_t offset) noexcept {
        FrameStamp stamp;const auto status=frame_stamp(offset,stamp);if(status!=Status::ok)return status;
        const auto copied=copy_frame();if(copied!=Status::ok)return copied;
        state.numeric_frames[0]={stamp,output.view()};delivery.emit(delivery.user);return Status::ok;
    }
    template<typename Scalar> Status consume_value(std::size_t index,std::span<Scalar> samples) noexcept {
        Scalar value=0;const auto loaded=load_element(source,{0,index,0},value);
        if(loaded!=Status::ok)return loaded;
        return append(samples,value)?emit(index):Status::ok;
    }
    Status consume(std::size_t index) noexcept {
        return source.contract->dtype==DType::Int16?consume_value(index,std::span(state.pcm16)):consume_value(index,std::span(state.pcm));
    }
public:
    FrameBufferExecution(const PlanNode& prepared,State& context,const DataFrameView& frame,TransformDelivery sink) noexcept
        :node(prepared),state(context),input(frame),source(std::get<NumericView>(frame.payload)),delivery(sink),output(mutable_output(node,state,0)){}
    Status run(const FrameLimits& limits) noexcept {
        state.pending_discontinuity|=input.stamp.discontinuity;
        if(!node.frame_hop || (state.pcm.empty() && state.pcm16.empty()) || (source.contract->dtype!=DType::Float32 && source.contract->dtype!=DType::Int16) || source.contract->shape.rank!=Rank::Vector)return Status::incompatible_contract;
        const auto valid=validate_values(source,ValueDomain::FiniteOnly,limits);if(valid!=Status::ok)return valid;
        for(std::size_t i=0;i<source.actual[0];++i){
            const auto status=consume(i);if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
};
}
TransformResult execute_frame_buffer(const PlanNode& node,State& state,const DataFrameView& frame,const FrameLimits& limits,TransformDelivery delivery) noexcept {
    const auto status=FrameBufferExecution(node,state,frame,delivery).run(limits);
    return {status==Status::ok?status:delivery.reject(delivery.user,status),true};
}
}
