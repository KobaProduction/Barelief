#include <dsp_kernels/sample_cadence.h>
#include "runtime_internal.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
#include "mixer_execution.h"
#include "clock_mix_execution.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
namespace {
struct ClockDelivery {
    void* user;
    Status (*send)(void*,NodeId,const DataFrameView&);
};
class ClockExecution {
    const std::span<const PlanNode> nodes;
    const std::span<State> states;
    const NumericProcessingGraph& graph;
    const NumericRuntimeProfile& profile;
    const NodeId clock_node;
    const FrameStamp& event;
    const ClockDelivery delivery;
    Status result=Status::ok;
    bool boundary=false,internal=false;
    bool clock_boundary(const State& state) const noexcept {
        const auto& prior=state.numeric_stamp;
        return event.discontinuity || (state.numeric_marker && (prior.producer_epoch!=event.producer_epoch || prior.generation!=event.generation || !prior.has_sample_interval || prior.sample_count>std::uint64_t(INT64_MAX) || prior.sample_begin>INT64_MAX-std::int64_t(prior.sample_count) || event.sample_begin!=prior.sample_begin+std::int64_t(prior.sample_count)));
    }
    bool selected(const PlanNode& node,std::size_t phase) const noexcept {
        const auto& configured=graph.nodes[node.configured];
        if(configured.clock_node!=clock_node && !(internal && configured.id==clock_node))return false;
        if(node.operation==ProcessingOperation::Mixer && aligned_operation(node,graph))return false;
        return phase==0?node.node.kind==NodeKind::SignalGenerator:node.operation==ProcessingOperation::Mixer;
    }
    Status publish_constant(std::size_t at) noexcept {
        const auto& node=nodes[at];auto output=mutable_output(node,states[at],0);
        const auto status=store_bits(output,{},output.contract->dtype,graph.nodes[node.configured].enabled?node.constant_bits:0);if(status!=Status::ok)return status;
        auto stamp=event;stamp.has_sample_interval=false;stamp.sample_begin=0;stamp.sample_count=0;
        stamp.latency_seconds={0,1};stamp.latency_samples=0;stamp.frequency_dependent_latency=false;
        return delivery.send(delivery.user,node.node.id,{stamp,output.view()});
    }
    Status finish_tick(State& state,std::uint64_t offset,FrameStamp& tick) const noexcept {
        if(offset>std::uint64_t(INT64_MAX) || event.sample_begin>INT64_MAX-std::int64_t(offset))return Status::overflow;
        const auto end=event.sample_begin+std::int64_t(offset);if(end<INT64_MIN+std::int64_t(state.hop))return Status::overflow;
        tick.sample_begin=end-std::int64_t(state.hop);tick.sample_count=state.hop;tick.timestamp_ticks=end-1;
        state.progress=state.hop=0;return Status::ok;
    }
    Status next_tick(const PlanNode& node,State& state,std::uint64_t& offset,FrameStamp& tick,bool& due) const noexcept {
        while(offset<event.sample_count){
            if(!state.hop)state.hop=dsp_kernels::next_sample_hop(node.hop_base,node.hop_fraction,node.hop_denominator,state.remainder);
            const auto taken=std::min<std::uint64_t>(event.sample_count-offset,state.hop-state.progress);offset+=taken;state.progress+=std::size_t(taken);
            if(state.progress==state.hop){due=true;return finish_tick(state,offset,tick);}
        }
        return Status::ok;
    }
    bool valid_count(const NumericContract& contract,const NumericBuffer& buffer,std::uint64_t count,State& state) noexcept {
        if(count>=contract.shape.extents[0].minimum && count<=contract.shape.extents[0].maximum && count<=buffer.floats.size())return true;
        ++state.stats.errors;result=Status::incompatible_contract;return false;
    }
    FrameStamp publication_stamp(State& state,const FrameStamp& tick) const noexcept {
        auto stamp=tick;stamp.latency_seconds={0,1};stamp.latency_samples=0;stamp.frequency_dependent_latency=false;
        stamp.discontinuity=boundary || state.pending_discontinuity;state.pending_discontinuity=false;return stamp;
    }
    bool render_generator(State& state,const FrameStamp& tick,std::size_t count) noexcept {
        auto& oscillator=*std::get<OscillatorKernel>(state.kernel).value;
        if(oscillator.seek(tick.sample_begin) && oscillator.render(std::span(state.numeric_outputs[0].floats).first(count)))return true;
        ++state.stats.errors;result=Status::overflow;return false;
    }
    Status render_tick(const PlanNode& node,State& state,const FrameStamp& tick,FrameStamp& stamp,std::size_t count,bool spectral,bool& ready) noexcept {
        if(node.node.kind==NodeKind::SignalGenerator){ready=render_generator(state,tick,count);return Status::ok;}
        const auto& configured=graph.nodes[node.configured];
        const auto mixed=mix_clock_frame(node,state,std::get<MixerProcessingParameters>(configured.parameters),stamp,tick,count,spectral,configured.enabled);
        if(mixed.fatal)return mixed.status;
        if(mixed.status!=Status::ok)result=mixed.status;
        return Status::ok;
    }
    Status publication_interval(State& state,const NumericContract& contract,FrameStamp& stamp,bool spectral) const noexcept {
        if(!divide_products(std::uint64_t(stamp.latency_seconds.numerator),contract.clock.sample_rate,stamp.latency_seconds.denominator,1,stamp.latency_samples)){++state.stats.errors;return Status::overflow;}
        if(spectral && state.output_sequence==UINT64_MAX){++state.stats.errors;return Status::overflow;}
        stamp.sequence=spectral?++state.output_sequence:event.sequence;return Status::ok;
    }
    NumericView publication_view(const NumericContract& contract,NumericBuffer& buffer,std::size_t count,bool available) const noexcept {
        buffer.actual={count,1};NumericView view{&contract,buffer.storage(),buffer.planes,buffer.actual,{},{}};
        for(std::size_t axis=0;axis<2;++axis)if(!buffer.coordinates[axis].empty())view.coordinates.axes[axis]={buffer.coordinates[axis].data(),buffer.actual[axis]*8};
        if(!available)view.storage={};
        return view;
    }
    void deliver_tick(const PlanNode& node,State& state,const FrameStamp& stamp,const NumericView& view,std::uint64_t measured) noexcept {
        if(profile.monotonic_nanoseconds){const auto end=profile.monotonic_nanoseconds(profile.measurement_clock);if(end>=measured)state.pending_self_nanoseconds=end-measured;}
        const auto status=delivery.send(delivery.user,node.node.id,{stamp,view});state.pending_self_nanoseconds=0;
        if(status!=Status::ok)result=status;
    }
    Status process_tick(const PlanNode& node,State& state,const FrameStamp& tick,bool spectral) noexcept {
        auto& buffer=state.numeric_outputs[0];const auto& contract=std::get<NumericContract>(node.numeric_outputs[0]);
        const auto measured=profile.monotonic_nanoseconds?profile.monotonic_nanoseconds(profile.measurement_clock):0;
        const auto count=spectral?contract.shape.extents[0].maximum:tick.sample_count;
        if(!valid_count(contract,buffer,count,state))return Status::ok;
        auto stamp=publication_stamp(state,tick);bool ready=true;
        {const auto status=render_tick(node,state,tick,stamp,count,spectral,ready);if(status!=Status::ok || !ready)return status;}
        {const auto status=publication_interval(state,contract,stamp,spectral);if(status!=Status::ok)return status;}
        deliver_tick(node,state,stamp,publication_view(contract,buffer,std::size_t(count),stamp.available),measured);return Status::ok;
    }
    Status prepare_tick(const PlanNode& node,State& state,bool spectral,std::uint64_t& offset,std::uint64_t& emissions,FrameStamp& tick,bool& due) const noexcept {
        if(!spectral){offset=event.sample_count;due=true;return Status::ok;}
        const auto status=next_tick(node,state,offset,tick,due);if(status!=Status::ok)return status;
        return due && ++emissions>profile.max_windows_per_ingress?Status::exhausted:Status::ok;
    }
    Status process_ticks(const PlanNode& node,State& state,bool spectral) noexcept {
        std::uint64_t offset=0,emissions=0;
        do {
            auto tick=event;bool due=false;
            {const auto status=prepare_tick(node,state,spectral,offset,emissions,tick,due);if(status!=Status::ok)return status;}
            if(!due)break;
            const auto status=process_tick(node,state,tick,spectral);if(status!=Status::ok)return status;
        }while(spectral && offset<event.sample_count);
        return Status::ok;
    }
    Status process_node(std::size_t at) noexcept {
        const auto& node=nodes[at];auto& state=states[at];const auto& contract=std::get<NumericContract>(node.numeric_outputs[0]);
        const bool spectral=node.operation==ProcessingOperation::Mixer && spectral_vector(contract);
        if(boundary && spectral){state.progress=state.hop=0;state.remainder=0;state.pending_discontinuity=true;}
        return process_ticks(node,state,spectral);
    }
    Status accept_clock(std::size_t at) noexcept {
        auto& state=states[at];
        if(state.numeric_marker && state.numeric_stamp.producer_epoch==event.producer_epoch && state.numeric_stamp.generation==event.generation && event.sequence<=state.numeric_stamp.sequence)return Status::would_block;
        boundary=clock_boundary(state);state.numeric_stamp=event;state.numeric_stamp.stream_id={};state.numeric_marker=true;return Status::ok;
    }
    Status dispatch() noexcept {
        for(std::size_t phase=0;phase<2;++phase)for(std::size_t at=0;at<nodes.size();++at)if(selected(nodes[at],phase)){
            const auto status=process_node(at);if(status!=Status::ok)return status;
        }
        return result;
    }
public:
    ClockExecution(std::span<const PlanNode> prepared,std::span<State> context,const NumericProcessingGraph& definition,const NumericRuntimeProfile& runtime,NodeId clock,const FrameStamp& tick,ClockDelivery output) noexcept
        :nodes(prepared),states(context),graph(definition),profile(runtime),clock_node(clock),event(tick),delivery(output){}
    Status run() noexcept {
        const auto at=index(nodes,clock_node);if(at==absent)return Status::invalid_argument;
        const auto& node=nodes[at];const auto& configured=graph.nodes[node.configured];
        internal=node.node.kind==NodeKind::SignalGenerator && configured.clock_node<0;const bool constant=numeric_constant_kind(configured.kind);
        if(node.operation!=ProcessingOperation::Clock && !internal && !constant)return Status::invalid_argument;
        const auto status=accept_clock(at);if(status!=Status::ok)return status;
        return constant?publish_constant(at):dispatch();
    }
};
}
Status PreparedGraph::process_clock(domain::NodeId clock_node,const signal_contracts::FrameStamp& event,RuntimeContext& context,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks) const noexcept {
    if(!context.impl_ || context.impl_->plan!=this || !event.has_sample_interval || !event.sample_count || event.sample_begin<0 || event.sample_count>std::uint64_t(INT64_MAX) || event.sample_begin>INT64_MAX-std::int64_t(event.sample_count))return Status::invalid_argument;
    struct Output {const PreparedGraph& graph;RuntimeContext& context;DeliveryStamp delivery;NumericRuntimeCallbacks callbacks;};
    Output output{*this,context,delivery,callbacks};
    ClockDelivery sink{&output,[](void* user,NodeId node,const DataFrameView& frame){auto& out=*static_cast<Output*>(user);return out.graph.process_numeric_impl(node,frame,out.context,out.delivery,out.callbacks,nullptr,{},{},{},0);}};
    return ClockExecution(impl_->nodes,context.impl_->states,*impl_->numeric_graph,impl_->numeric_profile,clock_node,event,sink).run();
}
namespace {
class ProviderWatermark {
    State& state;
    const PortId port;
    const FrameStamp stamp;
    const bool marker;
    const decltype(State::retired) retired;
    const std::size_t count,next;
public:
    ProviderWatermark(State& source,PortId lane) noexcept:state(source),port(lane),stamp(state.source_stamps[port]),marker(state.source_markers[port]),retired(state.retired),count(state.retired_count),next(state.retired_next){}
    ~ProviderWatermark(){state.source_stamps[port]=stamp;state.source_markers[port]=marker;state.retired=retired;state.retired_count=count;state.retired_next=next;}
};
Status unavailable_geometry(const NumericContract& contract,const FrameStamp& event,std::array<std::size_t,2>& actual) noexcept {
    if(contract.shape.rank!=Rank::Vector || contract.coordinates[0].mode!=CoordinateMode::None || !event.has_sample_interval)return Status::ok;
    if(event.sample_count<contract.shape.extents[0].minimum || event.sample_count>contract.shape.extents[0].maximum)return Status::invalid_argument;
    actual[0]=std::size_t(event.sample_count);return Status::ok;
}
Status unavailable_stamp(const NumericContract& contract,const State& source,PortId port,FrameStamp& stamp) noexcept {
    const auto& accepted=source.source_stamps[port];
    if(source.source_markers[port]){
        const auto advance=std::max(std::uint64_t{1},stamp.sequence);if(accepted.sequence>UINT64_MAX-advance)return Status::overflow;
        stamp.sequence=accepted.sequence+advance;stamp.producer_epoch=accepted.producer_epoch;stamp.generation=accepted.generation;
    }
    stamp.available=!contract.clock.domain.empty() && contract.clock.sample_rate;
    for(const auto& axis:contract.coordinates)if(axis.mode==CoordinateMode::ExplicitFloat64LE)stamp.available=false;
    return Status::ok;
}
Status prepare_zero_view(const NumericContract& contract,NumericBuffer& buffer,
                         std::array<std::size_t,2> actual,const FrameStamp& stamp,
                         const FrameLimits& limits,DataFrameView& frame) noexcept {
    MutableNumericView zero{&contract,buffer.storage(),buffer.planes,actual,{},{}};
    if(stamp.available && !buffer.silence){
        const auto status=fill_silence(zero,limits);if(status!=Status::ok)return status;
    }
    auto value=zero.view();
    if(!stamp.available)value.storage={};
    value.semantic_silence=stamp.available;
    frame={stamp,value};
    return Status::ok;
}
Status prepare_unavailable(const NumericContract& contract,State& source,PortId port,const FrameStamp& event,const FrameLimits& limits,DataFrameView& frame) noexcept {
    auto& buffer=source.numeric_outputs[port];auto actual=buffer.actual;auto stamp=event;
    {const auto status=unavailable_geometry(contract,event,actual);if(status!=Status::ok)return status;}
    {const auto status=unavailable_stamp(contract,source,port,stamp);if(status!=Status::ok)return status;}
    return prepare_zero_view(contract,buffer,actual,stamp,limits,frame);
}
}
Status PreparedGraph::advance_unavailable_source(domain::NodeId node,const signal_contracts::FrameStamp& event,RuntimeContext& context,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks,domain::PortId port) const noexcept {
    if(!context.impl_ || context.impl_->plan!=this)return Status::invalid_argument;
    const auto at=index(impl_->nodes,node);if(at==absent || impl_->nodes[at].operation!=ProcessingOperation::Source || port>=impl_->nodes[at].numeric_outputs.size())return Status::invalid_argument;
    const auto* contract=std::get_if<NumericContract>(&impl_->nodes[at].numeric_outputs[port]);if(!contract)return Status::incompatible_contract;
    auto& source=context.impl_->states[at];ProviderWatermark watermark{source,port};DataFrameView frame;
    const auto status=prepare_unavailable(*contract,source,port,event,impl_->numeric_profile.admission.frame,frame);if(status!=Status::ok)return status;
    // Local zeros update continuity without advancing the provider's watermark.
    return process_numeric_impl(node,frame,context,delivery,callbacks,nullptr,{},{},{},port);
}

} // namespace dataflow_runtime
