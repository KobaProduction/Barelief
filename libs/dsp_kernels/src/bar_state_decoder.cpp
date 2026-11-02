// Adapted from madmom features/downbeats.py DBNBarTrackingProcessor at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/detail/metrical_storage.h>
#include <dsp_kernels/bar_state_decoder.h>
#include <dsp_kernels/metrical_observations.h>
#include <dsp_kernels/metrical_transitions.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace dsp_kernels {
bool PreparedBarStateDecoder::prepare_model(std::size_t maximum,std::span<const std::uint32_t> meters,std::span<const float> switches) {
    std::vector<MetricalPatternConfig> patterns;std::vector<TempoTransitionRule> rules;
    patterns.reserve(meters.size());std::size_t rule_count=0;
    for(const auto beats:meters)if(!prepared_storage::add_bytes(rule_count,beats))return false;
    rules.reserve(rule_count);
    for(const auto beats:meters){
        if(!beats)return false;
        patterns.push_back({beats,1,1,0});
        rules.insert(rules.end(),beats,TempoTransitionRule{1,false});
    }
    if(!space_.prepare(patterns))return false;
    PreparedMetricalTransitions transitions;
    if(!transitions.prepare(space_,rules,switches))return false;
    std::vector<std::uint32_t> pointers(space_.states());
    return metrical_observation_pointers(space_,config_.observation_weight,MetricalObservation::BarStart,pointers) &&
        model_.prepare(std::max(std::size_t{1},maximum-1),2,pointers,transitions.edges());
}
bool PreparedBarStateDecoder::configure(std::size_t maximum,std::span<const std::uint32_t> meters,
                                        BarStateDecoderConfig config,std::span<const float> custom_switches) {
    if(!maximum || meters.empty() || !std::isfinite(config.observation_weight) || config.observation_weight<=1 ||
       !std::isfinite(config.meter_change_probability) || config.meter_change_probability<0 || config.meter_change_probability>1)return false;
    config_=config;
    std::vector<float> switches;
    if(custom_switches.empty() && !uniform_pattern_switches(meters.size(),config.meter_change_probability,switches))return false;
    if(!prepare_model(maximum,meters,custom_switches.empty()?std::span<const float>(switches):custom_switches))return false;
    maximum_beats_=maximum;numbers_.resize(maximum);return true;
}
bool PreparedBarStateDecoder::prepare(std::size_t maximum,std::span<const std::uint32_t> meters,
                                      BarStateDecoderConfig config,std::span<const float> switches) {
    std::size_t bound=0;if(!storage_bytes(maximum,meters,config,switches,bound))return false;
    PreparedBarStateDecoder next;
    if(!next.configure(maximum,meters,config,switches))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedBarStateDecoder::read(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& input=*static_cast<const Input*>(context);
    if(frame>=input.probability.size() || output.size()!=2)return false;
    const float value=input.probability[frame];
    if(!std::isfinite(value) || value<0 || value>1)return false;
    output[0]=beat_activation_density(value,input.weight,false,true);
    output[1]=beat_activation_density(value,input.weight,true,true);return true;
}
bool PreparedBarStateDecoder::process(std::span<const float> beats,std::span<const float> probabilities) noexcept {
    count_=0;model_.reset();
    if(!maximum_beats_ || beats.size()!=probabilities.size() || beats.size()>maximum_beats_ ||
       !std::all_of(beats.begin(),beats.end(),[](float x){return std::isfinite(x);}))return false;
    if(beats.size()<2)return true;
    const Input input{probabilities.first(probabilities.size()-1),config_.observation_weight};
    if(!model_.viterbi({&input,read},input.probability.size()))return false;
    const auto path=model_.state_path();
    if(path.empty())return true;
    const auto positions=space_.positions();const auto patterns=space_.state_patterns();
    for(std::size_t i=0;i<path.size();++i)numbers_[i]=std::uint32_t(positions[path[i]])+1;
    const auto meter=std::uint32_t(space_.beats(patterns[path.back()]));
    numbers_[path.size()]=numbers_[path.size()-1]%meter+1;count_=beats.size();return true;
}
}


namespace dsp_kernels {
bool PreparedBarStateDecoder::storage_bytes(std::size_t maximum,std::span<const std::uint32_t> meters,BarStateDecoderConfig c,std::span<const float> switches,std::size_t& out) noexcept {
    if(!maximum || meters.empty() || !std::isfinite(c.observation_weight) || c.observation_weight<=1 ||
       !std::isfinite(c.meter_change_probability) || c.meter_change_probability<0 || c.meter_change_probability>1)return false;
    std::size_t square=0;
    if(!prepared_storage::mul_bytes(meters.size(),meters.size(),square) ||
       (!switches.empty() && (switches.size()!=square || !std::all_of(switches.begin(),switches.end(),[](float v){return std::isfinite(v) && v>=0 && v<=1;}))))return false;
    detail::MetricalStorage s;
    const auto read=[&](std::size_t p,MetricalPatternConfig& v){v={meters[p],1,1,0};return meters[p]!=0;};
    if(!detail::metrical_storage(meters.size(),read,std::max(std::size_t{1},maximum-1),2,true,s) ||
       !prepared_storage::add_array(s.total,maximum,sizeof(std::uint32_t)) ||
       (switches.empty() && !prepared_storage::add_array(s.total,square,sizeof(float))))return false;
    out=s.total;return true;
}
}
