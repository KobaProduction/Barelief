#include "runtime_internal.h"
#include "signal_time.h"
#include "mixer_execution.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
Status PreparedGraph::process_clock(domain::NodeId clock_node,const signal_contracts::FrameStamp& event,RuntimeContext& context,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks) const noexcept {
    using namespace domain;using namespace signal_contracts;
    if(!context.impl_ || context.impl_->plan!=this || !event.has_sample_interval || !event.sample_count || event.sample_begin<0 || event.sample_count>std::uint64_t(INT64_MAX) || event.sample_begin>INT64_MAX-std::int64_t(event.sample_count))return Status::invalid_argument;
    const auto at=index(impl_->nodes,clock_node);if(at==absent || impl_->nodes[at].operation!=ProcessingOperation::Clock)return Status::invalid_argument;
    auto& clock_state=context.impl_->states[at];
    if(clock_state.numeric_marker && clock_state.numeric_stamp.producer_epoch==event.producer_epoch && clock_state.numeric_stamp.generation==event.generation && event.sequence<=clock_state.numeric_stamp.sequence)return Status::would_block;
    const auto& previous=clock_state.numeric_stamp;
    const bool boundary=event.discontinuity || (clock_state.numeric_marker &&
        (previous.producer_epoch!=event.producer_epoch || previous.generation!=event.generation || !previous.has_sample_interval ||
         previous.sample_count>std::uint64_t(INT64_MAX) || previous.sample_begin>INT64_MAX-std::int64_t(previous.sample_count) ||
         event.sample_begin!=previous.sample_begin+std::int64_t(previous.sample_count)));
    clock_state.numeric_stamp=event;clock_state.numeric_stamp.stream_id={};clock_state.numeric_marker=true;
    Status result=Status::ok;
    for(std::size_t phase=0;phase<2;++phase)for(std::size_t i=0;i<impl_->nodes.size();++i){const auto& n=impl_->nodes[i];const auto& configured=impl_->numeric_graph->nodes[n.configured];
        if((n.operation==ProcessingOperation::Mixer && aligned_operation(n,*impl_->numeric_graph)) || configured.clock_node!=clock_node || (phase==0?n.node.kind!=NodeKind::SignalGenerator:n.operation!=ProcessingOperation::Mixer))continue;
        auto& state=context.impl_->states[i];auto& buffer=state.numeric_outputs[0];const auto& contract=std::get<NumericContract>(n.numeric_outputs[0]);
        const bool spectral=n.operation==ProcessingOperation::Mixer && spectral_vector(contract);
        if(boundary && spectral){state.progress=state.hop=0;state.remainder=0;state.pending_discontinuity=true;}
        std::uint64_t offset=0,emissions=0;
        do {
        auto tick=event;
        if(spectral){
            bool due=false;
            while(offset<event.sample_count){
                if(!state.hop){state.hop=n.hop_base;
                    if(state.remainder>=n.hop_denominator-n.hop_fraction){++state.hop;state.remainder-=n.hop_denominator-n.hop_fraction;}
                    else state.remainder+=n.hop_fraction;}
                const auto taken=std::min<std::uint64_t>(event.sample_count-offset,state.hop-state.progress);
                offset+=taken;state.progress+=std::size_t(taken);
                if(state.progress==state.hop){
                    if(offset>std::uint64_t(INT64_MAX) || event.sample_begin>INT64_MAX-std::int64_t(offset))return Status::overflow;
                    const auto end=event.sample_begin+std::int64_t(offset);
                    if(end<INT64_MIN+std::int64_t(state.hop))return Status::overflow;
                    tick.sample_begin=end-std::int64_t(state.hop);tick.sample_count=state.hop;tick.timestamp_ticks=end-1;
                    state.progress=state.hop=0;due=true;break;}
            }
            if(!due)break;
            if(++emissions>impl_->numeric_profile.max_windows_per_ingress)return Status::exhausted;
        }else offset=event.sample_count;
        const auto measure_begin=impl_->numeric_profile.monotonic_nanoseconds?impl_->numeric_profile.monotonic_nanoseconds(impl_->numeric_profile.measurement_clock):0;
        const auto count=spectral?contract.shape.extents[0].maximum:tick.sample_count;
        if(count<contract.shape.extents[0].minimum || count>contract.shape.extents[0].maximum || count>buffer.floats.size()){++state.stats.errors;result=Status::incompatible_contract;continue;}
        auto stamp=tick;stamp.latency_seconds={0,1};stamp.latency_samples=0;stamp.discontinuity=boundary || state.pending_discontinuity;state.pending_discontinuity=false;
        if(n.node.kind==NodeKind::SignalGenerator){if(!std::get<OscillatorKernel>(state.kernel).value->seek(tick.sample_begin) || !std::get<OscillatorKernel>(state.kernel).value->render(std::span(buffer.floats).first(count))){++state.stats.errors;result=Status::overflow;continue;}}
        else {const auto& parameters=std::get<MixerProcessingParameters>(configured.parameters);
            // RMS uses scaled squares, preserving a representable answer even
            // when direct x*x would overflow. Port iteration defines tie order.
            std::array<double,128> samples{},weights{};
            if(state.mixer_queues.size()>samples.size())return Status::exhausted;
            std::array<std::size_t,128> order{};for(std::size_t q=0;q<state.mixer_queues.size();++q)order[q]=q;
            std::sort(order.begin(),order.begin()+state.mixer_queues.size(),[&](auto a,auto b){return n.endpoint_plan->inputs[a].port<n.endpoint_plan->inputs[b].port;});
            std::uint64_t aperture=0;bool coordinates_selected=false;
            for(std::size_t q=0;q<state.mixer_queues.size();++q){auto& queue=state.mixer_queues[q];
                if(spectral && queue.stamp_count && queue.count>=count){
                    if(!queue.coordinates[queue.stamp_head].empty()){const auto& coordinates=queue.coordinates[queue.stamp_head];if(!coordinates_selected){std::copy(coordinates.begin(),coordinates.end(),buffer.coordinates[0].begin());coordinates_selected=true;}else if(!std::equal(coordinates.begin(),coordinates.end(),buffer.coordinates[0].begin())){stamp.available=false;++state.stats.errors;result=Status::incompatible_contract;}}
                    const auto& provenance=queue.stamps[queue.stamp_head];if(provenance.has_sample_interval)aperture=std::max(aperture,provenance.sample_count);
                    if(latency_greater(provenance.latency_seconds,stamp.latency_seconds))stamp.latency_seconds=provenance.latency_seconds;
                    queue.stamp_head=(queue.stamp_head+1)%queue.stamps.size();--queue.stamp_count;}
                stamp.discontinuity=stamp.discontinuity || queue.discontinuity;queue.discontinuity=false;
                for(const auto& w:parameters.weights)if(w.port==n.endpoint_plan->inputs[q].port)weights[q]=w.weight;}
            if(spectral && aperture){if(tick.sample_count>std::uint64_t(INT64_MAX) || tick.sample_begin>INT64_MAX-std::int64_t(tick.sample_count) || aperture>std::uint64_t(INT64_MAX))return Status::overflow;
                const auto end=tick.sample_begin+std::int64_t(tick.sample_count);if(end<INT64_MIN+std::int64_t(aperture))return Status::overflow;stamp.sample_begin=end-std::int64_t(aperture);stamp.sample_count=aperture;stamp.timestamp_ticks=end-1;}
            for(std::size_t r=0;r<count;++r){double sum=0,weight=0,scale=0,largest=0;
                for(std::size_t k=0;k<state.mixer_queues.size();++k){const auto q=order[k];auto& queue=state.mixer_queues[q];double x=0;
                    if(queue.count){x=queue.samples[queue.head];
                        if(!spectral && latency_greater(queue.provenance[queue.head].latency_seconds,stamp.latency_seconds))stamp.latency_seconds=queue.provenance[queue.head].latency_seconds;
                        queue.head=(queue.head+1)%queue.samples.size();--queue.count;}else ++queue.underrun;
                    samples[q]=x;const auto w=weights[q];sum+=w*x;weight+=w;
                    if(std::fabs(w*x)>std::fabs(largest))largest=w*x;
                    if(w>0)scale=std::max(scale,std::fabs(x));
                }
                double value=sum;
                double weight_scale=0;for(std::size_t q=0;q<state.mixer_queues.size();++q)weight_scale=std::max(weight_scale,weights[q]);
                if(parameters.mode==MixerMode::WeightedMean || parameters.mode==MixerMode::WeightedRMS){weight=0;sum=0;for(std::size_t q=0;q<state.mixer_queues.size();++q){const auto w=weight_scale?weights[q]/weight_scale:0;weight+=w;sum+=w*samples[q];}}

                if(parameters.mode==MixerMode::WeightedMean)value=weight?sum/weight:0;
                if(parameters.mode==MixerMode::MaxAbsolute)value=largest;
                if(parameters.mode==MixerMode::WeightedRMS){double squares=0;if(scale)for(std::size_t q=0;q<state.mixer_queues.size();++q){const auto x=samples[q]/scale;squares+=(weight_scale?weights[q]/weight_scale:0)*x*x;}value=weight && scale?scale*std::sqrt(squares/weight):0;}
                if(!configured.enabled)value=0;
                if(!std::isfinite(value) || std::fabs(value)>std::numeric_limits<float>::max()){++state.stats.errors;result=Status::overflow;value=0;stamp.available=false;}
                buffer.floats[r]=float(value);
            }
        }
        // Duration is the maximum declared algorithmic latency of consumed
        // contributions. FIFO residence is not added: FIFO controls delivery,
        // and does not change the samples' declared signal-time delay.
        if(!divide_products(std::uint64_t(stamp.latency_seconds.numerator),contract.clock.sample_rate,stamp.latency_seconds.denominator,1,stamp.latency_samples)){++state.stats.errors;return Status::overflow;}
        if(spectral && state.output_sequence==UINT64_MAX){++state.stats.errors;return Status::overflow;}
        stamp.sequence=spectral?++state.output_sequence:event.sequence;buffer.actual={std::size_t(count),1};
        NumericView view{&contract,buffer.storage(),buffer.planes,buffer.actual,{},{}};
        for(std::size_t axis=0;axis<2;++axis)if(!buffer.coordinates[axis].empty())view.coordinates.axes[axis]={buffer.coordinates[axis].data(),buffer.actual[axis]*8};

        if(!stamp.available)view.storage={};
        if(impl_->numeric_profile.monotonic_nanoseconds){const auto end=impl_->numeric_profile.monotonic_nanoseconds(impl_->numeric_profile.measurement_clock);if(end>=measure_begin){const auto elapsed=end-measure_begin;state.pending_self_nanoseconds=elapsed;}}
        const auto status=process_numeric_impl(configured.id,{stamp,view},context,delivery,callbacks,nullptr,{},{},{},0);
        state.pending_self_nanoseconds=0;
        if(status!=Status::ok)result=status;
        }while(spectral && offset<event.sample_count);
    }
    return result;
}
Status PreparedGraph::advance_unavailable_source(domain::NodeId node,const signal_contracts::FrameStamp& event,RuntimeContext& context,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks,domain::PortId port) const noexcept {
    using namespace signal_contracts;
    if(!context.impl_ || context.impl_->plan!=this)return Status::invalid_argument;
    const auto at=index(impl_->nodes,node);if(at==absent || impl_->nodes[at].operation!=domain::ProcessingOperation::Source || port>=impl_->nodes[at].numeric_outputs.size())return Status::invalid_argument;
    const auto* contract=std::get_if<NumericContract>(&impl_->nodes[at].numeric_outputs[port]);if(!contract)return Status::incompatible_contract;
    auto& buffer=context.impl_->states[at].numeric_outputs[port];auto actual=buffer.actual;
    if(contract->shape.rank==Rank::Vector && contract->coordinates[0].mode==CoordinateMode::None && event.has_sample_interval){
        if(event.sample_count<contract->shape.extents[0].minimum || event.sample_count>contract->shape.extents[0].maximum)return Status::invalid_argument;
        actual[0]=std::size_t(event.sample_count);}
    auto& source=context.impl_->states[at];const auto accepted=source.source_stamps[port];const bool marked=source.source_markers[port];
    const auto retired=source.retired;const auto retired_count=source.retired_count,retired_next=source.retired_next;
    auto stamp=event;
    if(marked){if(accepted.sequence>UINT64_MAX-std::max(std::uint64_t{1},event.sequence))return Status::overflow;
        stamp.sequence=accepted.sequence+std::max(std::uint64_t{1},event.sequence);stamp.producer_epoch=accepted.producer_epoch;stamp.generation=accepted.generation;}
    stamp.available=!contract->clock.domain.empty() && contract->clock.sample_rate;
    for(const auto& axis:contract->coordinates)if(axis.mode==CoordinateMode::ExplicitFloat64LE)stamp.available=false;
    MutableNumericView zero{contract,buffer.storage(),buffer.planes,actual,{},{}};
    if(stamp.available && !buffer.silence){const auto status=fill_silence(zero,impl_->numeric_profile.admission.frame);if(status!=Status::ok)return status;}
    auto value=zero.view();if(!stamp.available)value.storage={};value.semantic_silence=stamp.available;
    const auto status=process_numeric_impl(node,{stamp,value},context,delivery,callbacks,nullptr,{},{},{},port);
    // Local continuity is not authoritative delivery from the provider. Restore
    // its acceptance watermark, so a resumed real stream need not out-run zeros.
    source.source_stamps[port]=accepted;source.source_markers[port]=marked;
    source.retired=retired;source.retired_count=retired_count;source.retired_next=retired_next;
    return status;
}

} // namespace dataflow_runtime
