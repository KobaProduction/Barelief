#include "window_execution.h"
#include "frame_access.h"
#include "preparation_support.h"
#include "signal_time.h"
#include <dsp_kernels/sample_cadence.h>
#include <algorithm>
#include <utility>
namespace dataflow_runtime::detail {
namespace {
class WindowExecution {
    const PlanNode& node;
    State& state;
    const DataFrameView& current;
    const TransformDelivery delivery;
    const NumericView& source;
    MutableNumericView output;
    const std::size_t columns,count;
    const bool direct;
    std::size_t offset=0;
    Status append_coordinates(std::size_t taken,std::size_t kept) noexcept {
        if(output.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
        auto& coordinates=state.numeric_outputs[0].coordinates[0];std::move(coordinates.begin()+kept*8,coordinates.end(),coordinates.begin());
        const auto incoming=source.coordinates.axes[0].subspan((offset+taken-kept)*8,kept*8);
        std::copy(incoming.begin(),incoming.end(),coordinates.begin()+(count-kept)*8);return Status::ok;
    }
    Status append_plane(std::size_t plane,std::size_t taken,std::size_t kept) noexcept {
        auto window=std::span(state.pcm).subspan(plane*count,count);std::move(window.begin()+kept,window.end(),window.begin());
        for(std::size_t column=0;column<kept;++column){const auto status=load_element(source,{plane,offset+taken-kept+column,0},window[count-kept+column]);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    Status append_samples(std::size_t taken,std::size_t kept) noexcept {
        if(!node.node.enabled)return Status::ok;
        for(std::size_t plane=0;plane<source.planes.size();++plane){const auto status=append_plane(plane,taken,kept);if(status!=Status::ok)return status;}
        return Status::ok;
    }
    Status append_input() noexcept {
        if(!state.hop)state.hop=dsp_kernels::next_sample_hop(node.hop_base,node.hop_fraction,node.hop_denominator,state.remainder);
        const auto taken=std::min(columns-offset,state.hop-state.progress),kept=std::min(count,taken);
        (void)append_coordinates(taken,kept);const auto status=append_samples(taken,kept);if(status!=Status::ok)return status;
        offset+=taken;state.progress+=taken;return Status::ok;
    }
    Status ticks_at_end(std::uint64_t& ticks) const noexcept {
        const auto& clock=source.contract->clock;
        if(clock.tick_seconds.numerator<=0 || !clock.sample_rate || !divide_products(std::uint64_t(offset),clock.tick_seconds.denominator,clock.sample_rate,std::uint64_t(clock.tick_seconds.numerator),ticks))return Status::overflow;
        return ticks>std::uint64_t(INT64_MAX) || current.stamp.timestamp_ticks>INT64_MAX-std::int64_t(ticks)?Status::overflow:Status::ok;
    }
    Status copy_window() noexcept {
        if(direct)return Status::ok;
        for(std::size_t plane=0;plane<output.planes.size();++plane)for(std::size_t column=0;column<count;++column){
            const auto status=store_element(output,{plane,column,0},node.node.enabled?state.pcm[plane*count+column]:0.f);if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status sample_interval(FrameStamp& stamp) const noexcept {
        if(!current.stamp.has_sample_interval)return Status::ok;
        if(current.stamp.sample_begin>INT64_MAX-std::int64_t(offset))return Status::overflow;
        const auto end=current.stamp.sample_begin+std::int64_t(offset);if(end<INT64_MIN+std::int64_t(count))return Status::overflow;
        stamp.sample_begin=end-std::int64_t(count);stamp.sample_count=count;stamp.has_sample_interval=true;stamp.timestamp_ticks=end-1;return Status::ok;
    }
    void publish(const FrameStamp& stamp) noexcept {
        auto value=output.view();
        if(direct && !node.node.enabled){value.storage=std::as_const(state.numeric_outputs[0]).storage();value.semantic_silence=true;}
        state.pending_discontinuity=false;state.numeric_frames[0]={stamp,value};delivery.emit(delivery.user);state.progress=state.hop=0;
    }
    Status emit_window() noexcept {
        if(state.output_sequence==UINT64_MAX)return Status::overflow;
        std::uint64_t ticks=0;{const auto status=ticks_at_end(ticks);if(status!=Status::ok)return status;}
        {const auto status=copy_window();if(status!=Status::ok)return status;}
        auto stamp=current.stamp;stamp.sequence=++state.output_sequence;if(!stamp.source_sequence)stamp.source_sequence=current.stamp.sequence;stamp.timestamp_ticks+=std::int64_t(ticks);
        {const auto status=sample_interval(stamp);if(status!=Status::ok)return status;}
        stamp.discontinuity=state.pending_discontinuity;publish(stamp);return Status::ok;
    }
public:
    WindowExecution(const PlanNode& prepared,State& context,const DataFrameView& input,TransformDelivery sink) noexcept
        :node(prepared),state(context),current(input),delivery(sink),source(std::get<NumericView>(current.payload)),output(mutable_output(node,state,0)),columns(source.actual[0]),count(output.actual[0]),direct(direct_window(*output.contract)){}
    TransformResult process() noexcept {
        state.pending_discontinuity=state.pending_discontinuity || current.stamp.discontinuity;if(direct)output.storage=std::as_writable_bytes(std::span(state.pcm));
        while(offset<columns){
            {const auto status=append_input();if(status!=Status::ok)return {delivery.reject(delivery.user,status),true};}
            if(state.progress==state.hop){const auto status=emit_window();if(status!=Status::ok)return {delivery.reject(delivery.user,status),true};}
        }
        return {Status::ok,true};
    }
};
}
TransformResult execute_window(const PlanNode& node,State& state,const DataFrameView& current,TransformDelivery delivery) noexcept {
    return WindowExecution(node,state,current,delivery).process();
}
}
