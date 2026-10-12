#include "mixer_execution.h"
#include "signal_time.h"
#include "frame_access.h"
#include <algorithm>
#include <cmath>

namespace dataflow_runtime::detail {
bool aligned_operation(const PlanNode& n,const NumericProcessingGraph& g) noexcept {
    return n.operation==ProcessingOperation::ChannelComparison || (n.operation==ProcessingOperation::Mixer &&
        std::get<MixerProcessingParameters>(g.nodes[n.configured].parameters).consumption==MixerConsumption::SourceAligned);
}
double mixed_sample(const MixerProcessingParameters& p,const PlanNode& n,const std::vector<MixerQueue>& queues,std::size_t offset) noexcept {
    double sum=0,total=0,scale=0,weight_scale=0,largest=0;
    weight_scale=n.endpoint_plan->weight_scale;
    for(std::size_t q=0;q<queues.size();++q){double weight=n.endpoint_plan->weights[q];
        const auto x=double(queues[q].samples[(queues[q].head+offset)%queues[q].samples.size()]);
        if(p.mode==MixerMode::WeightedMean||p.mode==MixerMode::WeightedRMS)weight=weight_scale?weight/weight_scale:0;
        sum+=weight*x;total+=weight;if(std::fabs(weight*x)>std::fabs(largest))largest=weight*x;if(weight>0)scale=std::max(scale,std::fabs(x));}
    if(p.mode==MixerMode::WeightedMean)return total?sum/total:0;
    if(p.mode==MixerMode::MaxAbsolute)return largest;
    if(p.mode==MixerMode::WeightedRMS){double squares=0;for(std::size_t q=0;q<queues.size();++q){double weight=weight_scale?n.endpoint_plan->weights[q]/weight_scale:0;
            const auto x=scale?double(queues[q].samples[(queues[q].head+offset)%queues[q].samples.size()])/scale:0;squares+=weight*x*x;}
        return total&&scale?scale*std::sqrt(squares/total):0;}
    return sum;
}
Status accept_mixer_contribution(const PlanNode& consumer,State& consumer_state,std::size_t input_lane,const DataFrameView& frame,const NumericProcessingGraph& graph,const NumericRuntimeProfile& profile,MixerDelivery delivery) noexcept {
    Status result=Status::ok;
    const bool aligned=aligned_operation(consumer,graph);
    auto now=[&]() noexcept {return profile.monotonic_nanoseconds?profile.monotonic_nanoseconds(profile.measurement_clock):std::uint64_t{0};};
    auto& queue=consumer_state.mixer_queues[input_lane];
    auto computation_started=now();
    auto charge_consumer=[&](){if(!aligned)return;const auto end=now();if(end>=computation_started){const auto elapsed=end-computation_started;consumer_state.pending_self_nanoseconds+=elapsed;delivery.child_nanoseconds+=elapsed;}computation_started=end;};
    auto send_consumer=[&](const DataFrameView& produced,Status status){charge_consumer();const auto begin=now();const auto sent=delivery.send(delivery.user,produced,status);const auto end=now();if(end>=begin)delivery.child_nanoseconds+=end-begin;computation_started=end;return sent;};
    const auto old_stamp=queue.last_input;
    const bool gap=aligned&&queue.input_marker&&(!frame.stamp.available||frame.stamp.discontinuity||
        frame.stamp.producer_epoch!=old_stamp.producer_epoch||frame.stamp.generation!=old_stamp.generation||
        !old_stamp.has_sample_interval||!frame.stamp.has_sample_interval||old_stamp.sample_count>std::uint64_t(INT64_MAX)||
        old_stamp.sample_begin>INT64_MAX-std::int64_t(old_stamp.sample_count)||
        frame.stamp.sample_begin!=old_stamp.sample_begin+std::int64_t(old_stamp.sample_count));
    auto& cohort=consumer_state.mixer_queues.front();
    const bool same_boundary=cohort.boundary_marker&&cohort.boundary.producer_epoch==frame.stamp.producer_epoch&&cohort.boundary.generation==frame.stamp.generation&&
        cohort.boundary.sample_begin==frame.stamp.sample_begin&&cohort.boundary.sample_count==frame.stamp.sample_count;
    if(gap){if(!same_boundary){for(auto& lane:consumer_state.mixer_queues){lane.head=lane.count=lane.stamp_head=lane.stamp_count=0;lane.discontinuity=true;}
            cohort.aligned_start_valid=false;
            cohort.boundary=frame.stamp;cohort.boundary.stream_id={};cohort.boundary_marker=true;}
        consumer_state.pending_discontinuity=true;
        // Scalar views must not retain the last metric during a gap.
        if(consumer.operation==ProcessingOperation::ChannelComparison){auto invalid=frame;auto dst=mutable_output(consumer,consumer_state,0);auto value=dst.view();value.storage={};invalid.payload=value;invalid.stamp.available=false;
            const auto sent=send_consumer(invalid,Status::unavailable);if(sent!=Status::ok)result=sent;}}
    queue.last_input=frame.stamp;queue.last_input.stream_id={};queue.input_marker=true;
    if(frame.stamp.discontinuity){queue.head=queue.count=queue.stamp_head=queue.stamp_count=0;queue.discontinuity=true;}
    if(!frame.stamp.available)return result;
    const auto& value=std::get<NumericView>(frame.payload);
    // Validate a whole contribution before mutating either FIFO. A
    // malformed tail must not leave a partial spectral frame queued.
    bool valid=!aligned||(frame.stamp.has_sample_interval&&frame.stamp.sample_count==value.actual[0]&&
        frame.stamp.sample_count<=std::uint64_t(INT64_MAX)&&frame.stamp.sample_begin<=INT64_MAX-std::int64_t(frame.stamp.sample_count));
    if(spectral_vector(*value.contract) && value.actual[0]!=value.contract->shape.extents[0].maximum)valid=false;
    for(std::size_t r=0;valid && r<value.actual[0];++r){float sample=0;const auto loaded=load_element(value,{0,r,0},sample);if(loaded!=Status::ok || !std::isfinite(sample))valid=false;}
    if(!valid){++consumer_state.stats.errors;result=Status::incompatible_contract;return result;}
    if(spectral_vector(*value.contract)){if(queue.stamp_count==queue.stamps.size()){queue.stamp_head=(queue.stamp_head+1)%queue.stamps.size();--queue.stamp_count;}
        auto saved=frame.stamp;saved.stream_id={};const auto slot=(queue.stamp_head+queue.stamp_count)%queue.stamps.size();queue.stamps[slot]=saved;
        if(!queue.coordinates[slot].empty()){if(value.coordinates.axes[0].size()!=queue.coordinates[slot].size()){result=Status::incompatible_contract;return result;}std::copy(value.coordinates.axes[0].begin(),value.coordinates.axes[0].end(),queue.coordinates[slot].begin());}++queue.stamp_count;}
    std::uint64_t delay=0,remainder=0;
    if(aligned&&(!divide_products(std::uint64_t(frame.stamp.latency_seconds.numerator),value.contract->clock.sample_rate,frame.stamp.latency_seconds.denominator,1,delay,&remainder)||
        delay>std::uint64_t(INT64_MAX)||remainder>std::uint64_t(INT64_MAX)||frame.stamp.sample_begin<INT64_MIN+std::int64_t(delay))){result=Status::overflow;return result;}
    for(std::size_t r=0;r<value.actual[0];++r){float sample=0;const auto loaded=load_element(value,{0,r,0},sample);if(loaded!=Status::ok){result=loaded;break;}
        if(aligned&&consumer.endpoint_plan->inputs[input_lane].parent==absent)sample=0;
        if(queue.count==queue.samples.size()){queue.head=(queue.head+1)%queue.samples.size();--queue.count;++queue.dropped;queue.discontinuity=true;}
        const auto slot=(queue.head+queue.count)%queue.samples.size();queue.samples[slot]=sample;
        if(!queue.provenance.empty())queue.provenance[slot].latency_seconds=frame.stamp.latency_seconds;
        if(aligned)queue.aligned[slot]={frame.stamp.sample_begin+std::int64_t(r)-std::int64_t(delay),{std::int64_t(remainder),frame.stamp.latency_seconds.denominator}};
        ++queue.count;}
    if(aligned){std::size_t windows=0;
        const auto interval_start=frame.stamp.sample_begin-std::int64_t(delay);
        if(!cohort.aligned_start_valid||interval_start>cohort.aligned_start_watermark)cohort.aligned_start_watermark=interval_start;
        cohort.aligned_start_valid=true;
        // A fixed 100ms source-timeline deadline is distinct from
        // prepared FIFO capacity. Keep max-envelope storage so an
        // 8192-sample pair can arrive lane-by-lane without loss.
        // Only a subsequent interval start advances this deadline.
        const auto deadline_samples=(std::uint64_t(value.contract->clock.sample_rate)*100+999)/1000;
        const auto cutoff=cohort.aligned_start_watermark<INT64_MIN+std::int64_t(deadline_samples)?INT64_MIN:cohort.aligned_start_watermark-std::int64_t(deadline_samples);
        bool expired=false;
        for(auto& lane:consumer_state.mixer_queues){std::uint64_t discarded=0;
            while(lane.count&&lane.aligned[lane.head].position<cutoff){lane.head=(lane.head+1)%lane.samples.size();--lane.count;++discarded;}
            if(discarded){lane.dropped+=discarded;lane.underrun+=discarded;lane.discontinuity=true;expired=true;}}
        if(expired){consumer_state.pending_discontinuity=true;
            if(consumer.operation==ProcessingOperation::ChannelComparison){auto invalid=frame;auto dst=mutable_output(consumer,consumer_state,0);auto expired_value=dst.view();expired_value.storage={};invalid.payload=expired_value;invalid.stamp.available=false;
                const auto sent=send_consumer(invalid,Status::unavailable);if(sent!=Status::ok)result=sent;}}
        auto discard_unmatched=[&](){
            for(auto& lane:consumer_state.mixer_queues){lane.dropped+=lane.count;lane.head=lane.count=lane.stamp_head=lane.stamp_count=0;lane.discontinuity=true;}
            consumer_state.pending_discontinuity=true;cohort.boundary_marker=false;
            if(consumer.operation==ProcessingOperation::ChannelComparison){auto invalid=frame;auto dst=mutable_output(consumer,consumer_state,0);auto value=dst.view();value.storage={};invalid.payload=value;invalid.stamp.available=false;
                const auto sent=send_consumer(invalid,Status::unavailable);if(sent!=Status::ok)result=sent;}
        };
        while(true){bool ready=true,fraction_mismatch=false;std::int64_t begin=INT64_MIN;Ratio fraction{0,1};bool first_lane=true;
            for(const auto& lane:consumer_state.mixer_queues){if(!lane.count){ready=false;break;}const auto point=lane.aligned[lane.head];begin=std::max(begin,point.position);
                if(first_lane){fraction=point.fractional_delay;first_lane=false;}else if(!same_fraction(fraction,point.fractional_delay)){fraction_mismatch=true;}}
            if(!ready)break;
            if(fraction_mismatch){discard_unmatched();break;}
            for(auto& lane:consumer_state.mixer_queues)while(lane.count&&lane.aligned[lane.head].position<begin){lane.head=(lane.head+1)%lane.samples.size();--lane.count;++lane.dropped;lane.discontinuity=true;consumer_state.pending_discontinuity=true;}
            std::size_t count=SIZE_MAX;for(const auto& lane:consumer_state.mixer_queues){if(!lane.count){ready=false;break;}count=std::min(count,lane.count);}if(!ready)break;
            const auto& contract=std::get<NumericContract>(consumer.numeric_outputs[0]);
            const bool comparison=consumer.operation==ProcessingOperation::ChannelComparison;
            if(comparison){if(count<consumer.hop_base)break;count=consumer.hop_base;}
            else {count=std::min(count,std::size_t(contract.shape.extents[0].maximum));if(count<contract.shape.extents[0].minimum)break;}
            // A contiguous ingress interval can still change latency.
            // Check the full matched prefix, not merely FIFO heads:
            // every lane must describe the same consecutive source
            // samples with one fractional delay throughout this frame.
            std::size_t matched=0;
            for(;matched<count;++matched){bool agrees=begin<=INT64_MAX-std::int64_t(matched);
                for(const auto& lane:consumer_state.mixer_queues){const auto point=lane.aligned[(lane.head+matched)%lane.samples.size()];
                    agrees=agrees && point.position==begin+std::int64_t(matched) && same_fraction(fraction,point.fractional_delay);}
                if(!agrees)break;}
            if(matched<count){
                const auto minimum=comparison?consumer.hop_base:std::size_t(contract.shape.extents[0].minimum);
                if(!matched || matched<minimum){discard_unmatched();break;}
                count=matched;
            }
            if(++windows>profile.max_windows_per_ingress){result=Status::exhausted;break;}
            auto dst=mutable_output(consumer,consumer_state,0);dst.actual={comparison?1:count,1};
            auto stamp=consumer_state.mixer_queues.front().last_input;stamp.stream_id={};stamp.discontinuity=consumer_state.pending_discontinuity;stamp.latency_seconds={0,1};
            for(const auto& lane:consumer_state.mixer_queues){stamp.discontinuity=stamp.discontinuity||lane.discontinuity;const auto latency=lane.provenance[lane.head].latency_seconds;if(latency_greater(latency,stamp.latency_seconds))stamp.latency_seconds=latency;}
            double energies[2]{};bool finite=true;
            if(comparison){for(std::size_t lane=0;lane<2;++lane)for(std::size_t r=0;r<count;++r){const auto x=double(consumer_state.mixer_queues[lane].samples[(consumer_state.mixer_queues[lane].head+r)%consumer_state.mixer_queues[lane].samples.size()]);energies[lane]+=x*x;}
                const auto sum=energies[0]+energies[1];const float balance=consumer.node.enabled&&sum>consumer.comparison_threshold?float((energies[1]-energies[0])/sum):0.f;finite=store_element(dst,{0,0,0},balance)==Status::ok;
            }else for(std::size_t r=0;r<count;++r){const auto mixed=consumer.node.enabled?mixed_sample(std::get<MixerProcessingParameters>(graph.nodes[consumer.configured].parameters),consumer,consumer_state.mixer_queues,r):0;
                if(!std::isfinite(mixed)||std::fabs(mixed)>std::numeric_limits<float>::max()){finite=false;break;}const auto stored=store_element(dst,{0,r,0},float(mixed));if(stored!=Status::ok){finite=false;break;}}
            std::uint64_t nominal_delay=0;if(!divide_products(std::uint64_t(stamp.latency_seconds.numerator),std::get<NumericContract>(consumer.numeric_input).clock.sample_rate,stamp.latency_seconds.denominator,1,nominal_delay)||nominal_delay>std::uint64_t(INT64_MAX)||begin>INT64_MAX-std::int64_t(nominal_delay)||begin+std::int64_t(nominal_delay)>INT64_MAX-std::int64_t(count)){result=Status::overflow;break;}
            stamp.sample_begin=begin+std::int64_t(nominal_delay);stamp.sample_count=count;stamp.timestamp_ticks=stamp.sample_begin+std::int64_t(count)-1;stamp.has_sample_interval=true;stamp.latency_samples=nominal_delay;
            if(consumer_state.output_sequence==UINT64_MAX){result=Status::overflow;break;}stamp.sequence=++consumer_state.output_sequence;stamp.available=finite;
            for(auto& lane:consumer_state.mixer_queues){lane.head=(lane.head+count)%lane.samples.size();lane.count-=count;lane.discontinuity=false;}
            consumer_state.pending_discontinuity=false;cohort.boundary_marker=false;auto produced=dst.view();if(!finite){produced.storage={};result=Status::overflow;++consumer_state.stats.errors;}
            const auto sent=send_consumer({stamp,produced},finite?Status::ok:Status::overflow);if(sent!=Status::ok)result=sent;
        }
        charge_consumer();
    }
    return result;
}
}
