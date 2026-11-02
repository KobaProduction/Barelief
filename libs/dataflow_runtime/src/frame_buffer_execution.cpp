#include "transform_execution.h"
#include "frame_access.h"
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
    bool append(float value) noexcept {
        state.pcm[state.oldest]=value;state.oldest=(state.oldest+1)%state.pcm.size();
        if(state.frames<state.pcm.size())return ++state.frames==state.pcm.size();
        if(++state.progress<node.frame_hop)return false;
        state.progress=0;return true;
    }
    Status copy_frame() noexcept {
        for(std::size_t i=0;i<state.pcm.size();++i){
            const auto status=store_element(output,{0,i,0},state.pcm[(state.oldest+i)%state.pcm.size()]);
            if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status frame_stamp(std::size_t offset,FrameStamp& stamp) noexcept {
        stamp=input.stamp;stamp.sequence=++state.row_ordinal;stamp.discontinuity=state.pending_discontinuity;state.pending_discontinuity=false;
        if(!stamp.has_sample_interval)return Status::ok;
        const auto advanced=std::int64_t(offset)+1,count=std::int64_t(state.pcm.size());
        if(stamp.sample_begin>INT64_MAX-advanced)return Status::overflow;
        const auto end=stamp.sample_begin+advanced;if(end<INT64_MIN+count)return Status::overflow;
        stamp.sample_begin=end-count;stamp.sample_count=std::uint64_t(count);return Status::ok;
    }
    Status emit(std::size_t offset) noexcept {
        FrameStamp stamp;const auto status=frame_stamp(offset,stamp);if(status!=Status::ok)return status;
        const auto copied=copy_frame();if(copied!=Status::ok)return copied;
        state.numeric_frames[0]={stamp,output.view()};delivery.emit(delivery.user);return Status::ok;
    }
    Status consume(std::size_t index) noexcept {
        float value=0;const auto loaded=load_element(source,{0,index,0},value);
        if(loaded!=Status::ok)return loaded;
        return append(value)?emit(index):Status::ok;
    }
public:
    FrameBufferExecution(const PlanNode& prepared,State& context,const DataFrameView& frame,TransformDelivery sink) noexcept
        :node(prepared),state(context),input(frame),source(std::get<NumericView>(frame.payload)),delivery(sink),output(mutable_output(node,state,0)){}
    Status run(const FrameLimits& limits) noexcept {
        state.pending_discontinuity|=input.stamp.discontinuity;
        if(!node.frame_hop || state.pcm.empty() || source.contract->dtype!=DType::Float32 || source.contract->shape.rank!=Rank::Vector)return Status::incompatible_contract;
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
