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
dsp_kernels::WeightedMixOperation weighted_mix_operation(MixerMode mode) noexcept {
    switch(mode){
    case MixerMode::WeightedMean:return dsp_kernels::WeightedMixOperation::Mean;
    case MixerMode::WeightedRMS:return dsp_kernels::WeightedMixOperation::Rms;
    case MixerMode::MaxAbsolute:return dsp_kernels::WeightedMixOperation::MaximumAbsolute;
    default:return dsp_kernels::WeightedMixOperation::Sum;
    }
}
namespace {
double queue_sample(const MixerQueue& queue,std::size_t offset) noexcept {
    return double(queue.samples[(queue.head+offset)%queue.samples.size()]);
}
double weighted_squares(const PlanNode& node,const std::vector<MixerQueue>& queues,std::size_t offset,double scale) noexcept {
    const auto weight_scale=node.endpoint_plan->weight_scale;double squares=0;
    for(std::size_t lane=0;lane<queues.size();++lane){
        const auto weight=dsp_kernels::scaled_mix_weight(node.endpoint_plan->weights[lane],weight_scale);
        const auto sample=scale?queue_sample(queues[lane],offset)/scale:0;squares+=weight*sample*sample;
    }
    return squares;
}
}
double mixed_sample(const MixerProcessingParameters& parameters,const PlanNode& node,const std::vector<MixerQueue>& queues,std::size_t offset) noexcept {
    dsp_kernels::WeightedMixAccumulator accumulation;
    const auto weight_scale=node.endpoint_plan->weight_scale;
    const bool normalize=parameters.mode==MixerMode::WeightedMean || parameters.mode==MixerMode::WeightedRMS;
    for(std::size_t lane=0;lane<queues.size();++lane){
        double weight=node.endpoint_plan->weights[lane];if(normalize)weight=dsp_kernels::scaled_mix_weight(weight,weight_scale);
        accumulation.add_weighted(queue_sample(queues[lane],offset),weight);
    }
    const auto squares=parameters.mode==MixerMode::WeightedRMS?weighted_squares(node,queues,offset,accumulation.amplitude_scale()):0;
    return accumulation.value(weighted_mix_operation(parameters.mode),squares);
}
namespace {
class MixerAcceptance {
    const PlanNode& consumer;
    State& state;
    const std::size_t input_lane;
    const DataFrameView& frame;
    const NumericProcessingGraph& graph;
    const NumericRuntimeProfile& profile;
    MixerDelivery delivery;
    const bool aligned;
    MixerQueue& queue;
    MixerQueue& cohort;
    Status result=Status::ok;
    std::uint64_t computation_started,delay=0,remainder=0;
    struct Match {std::int64_t begin=INT64_MIN;Ratio fraction{0,1};std::size_t count=0;};
    std::uint64_t now() const noexcept {return profile.monotonic_nanoseconds?profile.monotonic_nanoseconds(profile.measurement_clock):std::uint64_t{0};}
    const NumericView& value() const noexcept {return std::get<NumericView>(frame.payload);}
    bool comparison() const noexcept {return consumer.operation==ProcessingOperation::ChannelComparison;}
    void charge_consumer() noexcept {
        if(!aligned)return;
        const auto end=now();
        if(end>=computation_started){const auto elapsed=end-computation_started;state.pending_self_nanoseconds+=elapsed;delivery.child_nanoseconds+=elapsed;}
        computation_started=end;
    }
    Status send_consumer(const DataFrameView& produced,Status status) noexcept {
        charge_consumer();const auto begin=now();const auto sent=delivery.send(delivery.user,produced,status);const auto end=now();
        if(end>=begin)delivery.child_nanoseconds+=end-begin;
        computation_started=end;return sent;
    }
    void invalidate_comparison() noexcept {
        if(!comparison())return;
        auto invalid=frame;const auto destination=mutable_output(consumer,state,0);auto view=destination.view();view.storage={};
        invalid.payload=view;invalid.stamp.available=false;
        const auto sent=send_consumer(invalid,Status::unavailable);if(sent!=Status::ok)result=sent;
    }
    bool input_gap() const noexcept {
        const auto& old=queue.last_input;
        return aligned && queue.input_marker && (!frame.stamp.available || frame.stamp.discontinuity || frame.stamp.producer_epoch!=old.producer_epoch || frame.stamp.generation!=old.generation || !old.has_sample_interval || !frame.stamp.has_sample_interval || old.sample_count>std::uint64_t(INT64_MAX) || old.sample_begin>INT64_MAX-std::int64_t(old.sample_count) || frame.stamp.sample_begin!=old.sample_begin+std::int64_t(old.sample_count));
    }
    bool same_boundary() const noexcept {
        return cohort.boundary_marker && cohort.boundary.producer_epoch==frame.stamp.producer_epoch && cohort.boundary.generation==frame.stamp.generation && cohort.boundary.sample_begin==frame.stamp.sample_begin && cohort.boundary.sample_count==frame.stamp.sample_count;
    }
    void reset_cohort() noexcept {
        for(auto& lane:state.mixer_queues){lane.head=lane.count=lane.stamp_head=lane.stamp_count=0;lane.discontinuity=true;}
        cohort.aligned_start_valid=false;cohort.boundary=frame.stamp;cohort.boundary.stream_id={};cohort.boundary_marker=true;
    }
    void accept_boundary() noexcept {
        if(input_gap()){
            if(!same_boundary())reset_cohort();
            state.pending_discontinuity=true;invalidate_comparison();
        }
        queue.last_input=frame.stamp;queue.last_input.stream_id={};queue.input_marker=true;
        if(frame.stamp.discontinuity){queue.head=queue.count=queue.stamp_head=queue.stamp_count=0;queue.discontinuity=true;}
    }
    bool valid_contribution() const noexcept {
        const auto& source=value();
        if(aligned && (frame.stamp.frequency_dependent_latency || !frame.stamp.has_sample_interval || frame.stamp.sample_count!=source.actual[0] || frame.stamp.sample_count>std::uint64_t(INT64_MAX) || frame.stamp.sample_begin>INT64_MAX-std::int64_t(frame.stamp.sample_count)))return false;
        if(spectral_vector(*source.contract) && source.actual[0]!=source.contract->shape.extents[0].maximum)return false;
        for(std::size_t row=0;row<source.actual[0];++row){
            float sample=0;const auto status=load_element(source,{0,row,0},sample);
            if(status!=Status::ok || !std::isfinite(sample))return false;
        }
        return true;
    }
    bool retain_spectral_coordinates(const NumericView& source,std::size_t slot) noexcept {
        if(queue.coordinates[slot].empty())return true;
        if(source.coordinates.axes[0].size()!=queue.coordinates[slot].size()) {
            result=Status::incompatible_contract;return false;
        }
        std::copy(source.coordinates.axes[0].begin(),source.coordinates.axes[0].end(),queue.coordinates[slot].begin());
        return true;
    }
    bool retain_spectral_stamp() noexcept {
        const auto& source=value();if(!spectral_vector(*source.contract))return true;
        if(queue.stamp_count==queue.stamps.size()){queue.stamp_head=(queue.stamp_head+1)%queue.stamps.size();--queue.stamp_count;}
        auto saved=frame.stamp;saved.stream_id={};const auto slot=(queue.stamp_head+queue.stamp_count)%queue.stamps.size();queue.stamps[slot]=saved;
        if(!retain_spectral_coordinates(source,slot))return false;
        ++queue.stamp_count;return true;
    }
    bool resolve_delay() noexcept {
        if(!aligned)return true;
        if(!divide_products(std::uint64_t(frame.stamp.latency_seconds.numerator),value().contract->clock.sample_rate,frame.stamp.latency_seconds.denominator,1,delay,&remainder) || delay>std::uint64_t(INT64_MAX) || remainder>std::uint64_t(INT64_MAX) || frame.stamp.sample_begin<INT64_MIN+std::int64_t(delay)){result=Status::overflow;return false;}
        return true;
    }
    void append_sample(float sample,std::size_t row) noexcept {
        if(aligned && consumer.endpoint_plan->inputs[input_lane].parent==absent)sample=0;
        if(queue.count==queue.samples.size()){queue.head=(queue.head+1)%queue.samples.size();--queue.count;++queue.dropped;queue.discontinuity=true;}
        const auto slot=(queue.head+queue.count)%queue.samples.size();queue.samples[slot]=sample;
        if(!queue.provenance.empty())queue.provenance[slot]={frame.stamp.latency_seconds,frame.stamp.frequency_dependent_latency};
        if(aligned)queue.aligned[slot]={frame.stamp.sample_begin+std::int64_t(row)-std::int64_t(delay),{std::int64_t(remainder),frame.stamp.latency_seconds.denominator}};
        ++queue.count;
    }
    void append_samples() noexcept {
        const auto& source=value();
        for(std::size_t row=0;row<source.actual[0];++row){
            float sample=0;const auto status=load_element(source,{0,row,0},sample);
            if(status!=Status::ok){result=status;break;}
            append_sample(sample,row);
        }
    }
    std::int64_t deadline() noexcept {
        const auto start=frame.stamp.sample_begin-std::int64_t(delay);
        if(!cohort.aligned_start_valid || start>cohort.aligned_start_watermark)cohort.aligned_start_watermark=start;
        cohort.aligned_start_valid=true;
        // Source timeline deadline is independent of prepared FIFO capacity.
        const auto samples=(std::uint64_t(value().contract->clock.sample_rate)*100+999)/1000;
        return cohort.aligned_start_watermark<INT64_MIN+std::int64_t(samples)?INT64_MIN:cohort.aligned_start_watermark-std::int64_t(samples);
    }
    bool expire_lane(MixerQueue& lane,std::int64_t cutoff) noexcept {
        std::uint64_t discarded=0;
        while(lane.count && lane.aligned[lane.head].position<cutoff){lane.head=(lane.head+1)%lane.samples.size();--lane.count;++discarded;}
        if(!discarded)return false;
        lane.dropped+=discarded;lane.underrun+=discarded;lane.discontinuity=true;return true;
    }
    void expire_samples() noexcept {
        const auto cutoff=deadline();bool expired=false;
        for(auto& lane:state.mixer_queues)if(expire_lane(lane,cutoff))expired=true;
        if(expired){state.pending_discontinuity=true;invalidate_comparison();}
    }
    void discard_unmatched() noexcept {
        for(auto& lane:state.mixer_queues){lane.dropped+=lane.count;lane.head=lane.count=lane.stamp_head=lane.stamp_count=0;lane.discontinuity=true;}
        state.pending_discontinuity=true;cohort.boundary_marker=false;invalidate_comparison();
    }
    bool inspect_heads(Match& match,bool& mismatch) const noexcept {
        bool first=true;
        for(const auto& lane:state.mixer_queues) {
            if(!lane.count)return false;
            const auto point=lane.aligned[lane.head];match.begin=std::max(match.begin,point.position);
            if(first){match.fraction=point.fractional_delay;first=false;}
            else if(!same_fraction(match.fraction,point.fractional_delay))mismatch=true;
        }
        return true;
    }
    bool match_heads(Match& match) noexcept {
        bool mismatch=false;if(!inspect_heads(match,mismatch))return false;
        if(mismatch){discard_unmatched();return false;}
        return true;
    }
    void align_heads(std::int64_t begin) noexcept {
        for(auto& lane:state.mixer_queues)while(lane.count && lane.aligned[lane.head].position<begin){
            lane.head=(lane.head+1)%lane.samples.size();--lane.count;++lane.dropped;lane.discontinuity=true;state.pending_discontinuity=true;
        }
    }
    bool match_capacity(Match& match) const noexcept {
        match.count=SIZE_MAX;
        for(const auto& lane:state.mixer_queues){if(!lane.count)return false;match.count=std::min(match.count,lane.count);}
        if(comparison()){if(match.count<consumer.hop_base)return false;match.count=consumer.hop_base;return true;}
        const auto& contract=std::get<NumericContract>(consumer.numeric_outputs[0]);
        match.count=std::min(match.count,std::size_t(contract.shape.extents[0].maximum));return match.count>=contract.shape.extents[0].minimum;
    }
    bool prefix_agrees(const Match& match,std::size_t row) const noexcept {
        bool agrees=match.begin<=INT64_MAX-std::int64_t(row);
        for(const auto& lane:state.mixer_queues){
            const auto point=lane.aligned[(lane.head+row)%lane.samples.size()];
            agrees=agrees && point.position==match.begin+std::int64_t(row) && same_fraction(match.fraction,point.fractional_delay);
        }
        return agrees;
    }
    bool match_prefix(Match& match) noexcept {
        std::size_t matched=0;for(;matched<match.count;++matched)if(!prefix_agrees(match,matched))break;
        if(matched==match.count)return true;
        const auto& contract=std::get<NumericContract>(consumer.numeric_outputs[0]);
        const auto minimum=comparison()?consumer.hop_base:std::size_t(contract.shape.extents[0].minimum);
        if(!matched || matched<minimum){discard_unmatched();return false;}
        match.count=matched;return true;
    }
    FrameStamp matched_stamp() const noexcept {
        auto stamp=cohort.last_input;stamp.stream_id={};stamp.discontinuity=state.pending_discontinuity;stamp.latency_seconds={0,1};
        for(const auto& lane:state.mixer_queues){
            stamp.discontinuity=stamp.discontinuity || lane.discontinuity;const auto latency=lane.provenance[lane.head].latency_seconds;
            if(latency_greater(latency,stamp.latency_seconds))stamp.latency_seconds=latency;
        }
        return stamp;
    }
    bool render_comparison(MutableNumericView& output,std::size_t count) const noexcept {
        double energies[2]{};
        for(std::size_t lane=0;lane<2;++lane)for(std::size_t row=0;row<count;++row){
            const auto& queue=state.mixer_queues[lane];const auto sample=double(queue.samples[(queue.head+row)%queue.samples.size()]);energies[lane]+=sample*sample;
        }
        const auto sum=energies[0]+energies[1];
        const float balance=consumer.node.enabled && sum>consumer.comparison_threshold?float((energies[1]-energies[0])/sum):0.f;
        return store_element(output,{0,0,0},balance)==Status::ok;
    }
    bool render_mixture(MutableNumericView& output,std::size_t count) const noexcept {
        const auto& parameters=std::get<MixerProcessingParameters>(graph.nodes[consumer.configured].parameters);
        for(std::size_t row=0;row<count;++row){
            const auto mixed=consumer.node.enabled?mixed_sample(parameters,consumer,state.mixer_queues,row):0;
            if(!std::isfinite(mixed) || std::fabs(mixed)>std::numeric_limits<float>::max())return false;
            if(store_element(output,{0,row,0},float(mixed))!=Status::ok)return false;
        }
        return true;
    }
    bool stamp_interval(FrameStamp& stamp,const Match& match) noexcept {
        std::uint64_t nominal=0;
        if(!divide_products(std::uint64_t(stamp.latency_seconds.numerator),std::get<NumericContract>(consumer.numeric_input).clock.sample_rate,stamp.latency_seconds.denominator,1,nominal) || nominal>std::uint64_t(INT64_MAX) || match.begin>INT64_MAX-std::int64_t(nominal) || match.begin+std::int64_t(nominal)>INT64_MAX-std::int64_t(match.count)){result=Status::overflow;return false;}
        stamp.sample_begin=match.begin+std::int64_t(nominal);stamp.sample_count=match.count;stamp.timestamp_ticks=stamp.sample_begin+std::int64_t(match.count)-1;stamp.has_sample_interval=true;stamp.latency_samples=nominal;
        if(state.output_sequence==UINT64_MAX){result=Status::overflow;return false;}
        stamp.sequence=++state.output_sequence;return true;
    }
    void consume_match(std::size_t count) noexcept {
        for(auto& lane:state.mixer_queues){lane.head=(lane.head+count)%lane.samples.size();lane.count-=count;lane.discontinuity=false;}
        state.pending_discontinuity=false;cohort.boundary_marker=false;
    }
    bool publish_match(const Match& match) noexcept {
        auto output=mutable_output(consumer,state,0);output.actual={comparison()?1:match.count,1};auto stamp=matched_stamp();
        const bool finite=comparison()?render_comparison(output,match.count):render_mixture(output,match.count);
        if(!stamp_interval(stamp,match))return false;
        stamp.available=finite;consume_match(match.count);auto produced=output.view();
        if(!finite){produced.storage={};result=Status::overflow;++state.stats.errors;}
        const auto sent=send_consumer({stamp,produced},finite?Status::ok:Status::overflow);if(sent!=Status::ok)result=sent;
        return true;
    }
    void process_aligned() noexcept {
        expire_samples();std::size_t windows=0;
        while(true){
            Match match;if(!match_heads(match))break;
            align_heads(match.begin);
            if(!match_capacity(match) || !match_prefix(match))break;
            if(++windows>profile.max_windows_per_ingress){result=Status::exhausted;break;}
            if(!publish_match(match))break;
        }
        charge_consumer();
    }
public:
    MixerAcceptance(const PlanNode& node,State& context,std::size_t lane,const DataFrameView& input,const NumericProcessingGraph& definition,const NumericRuntimeProfile& runtime,MixerDelivery consumer_delivery) noexcept
        :consumer(node),state(context),input_lane(lane),frame(input),graph(definition),profile(runtime),delivery(consumer_delivery),aligned(aligned_operation(consumer,graph)),queue(state.mixer_queues[input_lane]),cohort(state.mixer_queues.front()),computation_started(now()){}
    Status accept() noexcept {
        accept_boundary();if(!frame.stamp.available)return result;
        // Whole-frame validation precedes writes to either contribution FIFO.
        if(!valid_contribution()){++state.stats.errors;return Status::incompatible_contract;}
        if(!retain_spectral_stamp() || !resolve_delay())return result;
        append_samples();if(aligned)process_aligned();return result;
    }
};
}
Status accept_mixer_contribution(const PlanNode& node,State& state,std::size_t lane,const DataFrameView& input,const NumericProcessingGraph& graph,const NumericRuntimeProfile& profile,MixerDelivery delivery) noexcept {
    return MixerAcceptance(node,state,lane,input,graph,profile,delivery).accept();
}
}
