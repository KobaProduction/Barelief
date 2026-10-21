#include "clock_mix_execution.h"
#include "mixer_execution.h"
#include "signal_time.h"
#include <algorithm>
#include <cmath>
namespace dataflow_runtime::detail {
namespace {
class ClockMixture {
    const PlanNode& node;
    State& state;
    const MixerProcessingParameters& parameters;
    FrameStamp& stamp;
    const FrameStamp& tick;
    const std::size_t count;
    const bool spectral,enabled;
    NumericBuffer& buffer;
    std::array<double,domain::max_variadic_inputs> samples{},weights{};
    std::array<std::size_t,domain::max_variadic_inputs> order{};
    std::uint64_t aperture=0;
    bool coordinates_selected=false;
    Status result=Status::ok;
    void retain_latency(Ratio latency,bool dependent) noexcept {
        if(latency_greater(latency,stamp.latency_seconds))stamp.latency_seconds=latency;
        stamp.frequency_dependent_latency=stamp.frequency_dependent_latency || dependent;
    }
    void select_coordinates(const std::vector<std::byte>& coordinates) noexcept {
        if(coordinates.empty())return;
        if(!coordinates_selected){std::copy(coordinates.begin(),coordinates.end(),buffer.coordinates[0].begin());coordinates_selected=true;return;}
        if(!std::equal(coordinates.begin(),coordinates.end(),buffer.coordinates[0].begin())){stamp.available=false;++state.stats.errors;result=Status::incompatible_contract;}
    }
    void consume_spectral_stamp(MixerQueue& queue) noexcept {
        if(!spectral || !queue.stamp_count || queue.count<count)return;
        select_coordinates(queue.coordinates[queue.stamp_head]);const auto& provenance=queue.stamps[queue.stamp_head];
        if(provenance.has_sample_interval)aperture=std::max(aperture,provenance.sample_count);
        retain_latency(provenance.latency_seconds,provenance.frequency_dependent_latency);
        queue.stamp_head=(queue.stamp_head+1)%queue.stamps.size();--queue.stamp_count;
    }
    void prepare_queue_weights() noexcept {
        for(std::size_t lane=0;lane<state.mixer_queues.size();++lane){
            auto& queue=state.mixer_queues[lane];consume_spectral_stamp(queue);
            stamp.discontinuity=stamp.discontinuity || queue.discontinuity;queue.discontinuity=false;
            for(const auto& weight:parameters.weights)if(weight.port==node.endpoint_plan->inputs[lane].port)weights[lane]=weight.weight;
        }
    }
    void prepare_population() noexcept {
        for(std::size_t lane=0;lane<state.mixer_queues.size();++lane)order[lane]=lane;
        std::sort(order.begin(),order.begin()+state.mixer_queues.size(),[&](auto a,auto b){return node.endpoint_plan->inputs[a].port<node.endpoint_plan->inputs[b].port;});
        prepare_queue_weights();
    }
    bool apply_aperture() noexcept {
        if(!spectral || !aperture)return true;
        if(tick.sample_count>std::uint64_t(INT64_MAX) || tick.sample_begin>INT64_MAX-std::int64_t(tick.sample_count) || aperture>std::uint64_t(INT64_MAX))return false;
        const auto end=tick.sample_begin+std::int64_t(tick.sample_count);if(end<INT64_MIN+std::int64_t(aperture))return false;
        stamp.sample_begin=end-std::int64_t(aperture);stamp.sample_count=aperture;stamp.timestamp_ticks=end-1;return true;
    }
    double consume_sample(MixerQueue& queue) noexcept {
        if(!queue.count){++queue.underrun;return 0;}
        const auto sample=queue.samples[queue.head];
        if(!spectral){const auto& provenance=queue.provenance[queue.head];retain_latency(provenance.latency_seconds,provenance.frequency_dependent_latency);}
        queue.head=(queue.head+1)%queue.samples.size();--queue.count;return sample;
    }
    dsp_kernels::WeightedMixAccumulator accumulate() noexcept {
        dsp_kernels::WeightedMixAccumulator population;
        for(std::size_t index=0;index<state.mixer_queues.size();++index){
            const auto lane=order[index];samples[lane]=consume_sample(state.mixer_queues[lane]);population.add_weighted(samples[lane],weights[lane]);
        }
        return population;
    }
    double maximum_weight() const noexcept {
        return dsp_kernels::maximum_mix_weight(std::span(weights).first(state.mixer_queues.size()));
    }
    void normalize_population(dsp_kernels::WeightedMixAccumulator& population,double weight_scale) const noexcept {
        if(parameters.mode!=MixerMode::WeightedMean && parameters.mode!=MixerMode::WeightedRMS)return;
        population.reset_sum();
        for(std::size_t lane=0;lane<state.mixer_queues.size();++lane)population.add_normalized(samples[lane],dsp_kernels::scaled_mix_weight(weights[lane],weight_scale));
    }
    double scaled_squares(double scale,double weight_scale) const noexcept {
        if(parameters.mode!=MixerMode::WeightedRMS || !scale)return 0;
        double squares=0;
        for(std::size_t lane=0;lane<state.mixer_queues.size();++lane){const auto sample=samples[lane]/scale;squares+=(dsp_kernels::scaled_mix_weight(weights[lane],weight_scale))*sample*sample;}
        return squares;
    }
    void render_samples() noexcept {
        for(std::size_t row=0;row<count;++row){
            auto population=accumulate();const auto weight_scale=maximum_weight();normalize_population(population,weight_scale);
            const auto squares=scaled_squares(population.amplitude_scale(),weight_scale);
            double value=population.value(weighted_mix_operation(parameters.mode),squares);if(!enabled)value=0;
            if(!std::isfinite(value) || std::fabs(value)>std::numeric_limits<float>::max()){++state.stats.errors;result=Status::overflow;value=0;stamp.available=false;}
            buffer.floats[row]=float(value);
        }
    }
public:
    ClockMixture(const PlanNode& prepared,State& context,const MixerProcessingParameters& operation,FrameStamp& publication,const FrameStamp& clock,std::size_t elements,bool is_spectral,bool is_enabled) noexcept
        :node(prepared),state(context),parameters(operation),stamp(publication),tick(clock),count(elements),spectral(is_spectral),enabled(is_enabled),buffer(state.numeric_outputs[0]){}
    ClockMixResult process() noexcept {
        if(state.mixer_queues.size()>samples.size())return {Status::exhausted,true};
        prepare_population();if(!apply_aperture())return {Status::overflow,true};
        render_samples();return {result,false};
    }
};
}
ClockMixResult mix_clock_frame(const PlanNode& node,State& state,const MixerProcessingParameters& parameters,FrameStamp& stamp,const FrameStamp& tick,std::size_t count,bool spectral,bool enabled) noexcept {
    return ClockMixture(node,state,parameters,stamp,tick,count,spectral,enabled).process();
}
}
