#include "resampler_execution.h"
#include "frame_access.h"
#include "preparation_support.h"
#include "signal_time.h"
#include <numeric>
namespace dataflow_runtime::detail {
namespace {
class ResamplerExecution {
    const PlanNode& node;
    State& state;
    const DataFrameView& current;
    const TransformDelivery delivery;
    const NumericView& source;
    MutableNumericView output;
    dsp_kernels::PreparedResampler& resampler;
    const std::size_t columns,count,input_stride;
    std::uint64_t ordinal=0;
    std::size_t written=0;
    FrameStamp stamp;
    Status load_input() noexcept {
        for(std::size_t plane=0;plane<source.planes.size();++plane)for(std::size_t column=0;column<columns;++column){
            float value=0;
            if(node.node.enabled){const auto status=load_element(source,{plane,column,0},value);if(status!=Status::ok)return status;}
            state.left[plane*input_stride+column]=value;
        }
        return Status::ok;
    }
    Status anchor() noexcept {
        if(resampler.anchored())return Status::ok;
        std::int64_t converted=0;
        if(!convert_rate_ticks(current.stamp.timestamp_ticks,source.contract->clock.sample_rate,output.contract->clock.sample_rate,converted))return Status::overflow;
        const auto in_rate=source.contract->clock.sample_rate,out_rate=output.contract->clock.sample_rate;
        auto remainder=current.stamp.timestamp_ticks%std::int64_t(in_rate);if(remainder<0)remainder+=in_rate;
        const auto residue=std::uint32_t(std::uint64_t(remainder)*out_rate%in_rate);
        if(residue && converted==INT64_MAX)return Status::overflow;
        if(residue)++converted;
        return resampler.start_at(converted,residue?in_rate-residue:0)?Status::ok:Status::invalid_argument;
    }
    Status process_input() noexcept {
        ordinal=resampler.produced();
        if(!resampler.process(state.left,columns,input_stride,state.values,count,written)){state.pending_discontinuity=true;return Status::overflow;}
        if(!resampler.advance_input(current.stamp.timestamp_ticks,columns)){resampler.reset();state.pending_discontinuity=true;return Status::overflow;}
        return Status::ok;
    }
    Status copy_output() noexcept {
        if(ordinal>std::uint64_t(INT64_MAX) || resampler.anchor_ticks()>INT64_MAX-std::int64_t(ordinal) || state.output_sequence==UINT64_MAX)return Status::overflow;
        output.actual[0]=written;
        for(std::size_t plane=0;plane<output.planes.size();++plane)for(std::size_t column=0;column<written;++column){
            const auto status=store_element(output,{plane,column,0},node.node.enabled?state.values[plane*count+column]:0.f);if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status add_fixed_latency(std::uint64_t delay) noexcept {
        if(!delay)return Status::ok;
        const auto denominator=current.stamp.latency_seconds.denominator,rate=std::uint64_t(source.contract->clock.sample_rate),common=std::gcd(denominator,rate);
        const auto left_scale=rate/common,right_scale=denominator/common;
        if(denominator>UINT64_MAX/left_scale || std::uint64_t(current.stamp.latency_seconds.numerator)>std::uint64_t(INT64_MAX)/left_scale || right_scale>std::uint64_t(INT64_MAX)/delay)return Status::overflow;
        const auto left=std::uint64_t(current.stamp.latency_seconds.numerator)*left_scale,right=delay*right_scale;if(left>std::uint64_t(INT64_MAX)-right)return Status::overflow;
        const auto numerator=left+right,denominator_out=denominator*left_scale,reduce=std::gcd(numerator,denominator_out);
        stamp.latency_seconds={std::int64_t(numerator/reduce),denominator_out/reduce};return Status::ok;
    }
    Status output_stamp() noexcept {
        const auto first=resampler.anchor_ticks()+std::int64_t(ordinal);if(first>INT64_MAX-std::int64_t(written-1))return Status::overflow;
        stamp=current.stamp;stamp.timestamp_ticks=first;stamp.sequence=++state.output_sequence;stamp.sample_begin=first;stamp.sample_count=written;stamp.has_sample_interval=true;
        const auto resources=resampler.resources();stamp.frequency_dependent_latency=stamp.frequency_dependent_latency || resources.frequency_dependent_delay;
        return add_fixed_latency(std::uint64_t(resources.fixed_delay_input_samples));
    }
    Status latency_diagnostic() noexcept {
        std::uint64_t diagnostic=0;
        if(!divide_products(std::uint64_t(stamp.latency_seconds.numerator),output.contract->clock.sample_rate,stamp.latency_seconds.denominator,1,diagnostic))return Status::overflow;
        stamp.latency_samples=diagnostic;return Status::ok;
    }
    Status prepare_input() noexcept {
        {const auto status=load_input();if(status!=Status::ok)return status;}
        {const auto status=anchor();if(status!=Status::ok)return status;}
        return process_input();
    }
    Status prepare_output() noexcept {
        {const auto status=copy_output();if(status!=Status::ok)return status;}
        {const auto status=output_stamp();if(status!=Status::ok)return status;}
        return latency_diagnostic();
    }
    void publish() noexcept {
        stamp.discontinuity=stamp.discontinuity || state.pending_discontinuity;state.pending_discontinuity=false;
        if(!stamp.source_sequence)stamp.source_sequence=current.stamp.sequence;
        state.numeric_frames[0]={stamp,output.view()};delivery.emit(delivery.user);
    }
public:
    ResamplerExecution(const PlanNode& prepared,State& context,const DataFrameView& input,TransformDelivery sink) noexcept
        :node(prepared),state(context),current(input),delivery(sink),source(std::get<NumericView>(current.payload)),output(mutable_output(node,state,0)),resampler(std::get<ResamplerKernel>(state.kernel).operation()),columns(source.actual[0]),count(output.actual[0]),input_stride(std::size_t(source.contract->shape.extents[0].maximum)){}
    TransformResult process() noexcept {
        {const auto status=prepare_input();if(status!=Status::ok)return {delivery.reject(delivery.user,status),true};}
        if(!written)return {Status::ok,true};
        {const auto status=prepare_output();if(status!=Status::ok)return {delivery.reject(delivery.user,status),true};}
        publish();return {Status::ok,true};
    }
};
}
TransformResult execute_resampler(const PlanNode& node,State& state,const DataFrameView& current,TransformDelivery delivery) noexcept {
    return ResamplerExecution(node,state,current,delivery).process();
}
}
