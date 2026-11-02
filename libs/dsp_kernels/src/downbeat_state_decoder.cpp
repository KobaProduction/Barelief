// Adapted from madmom features/downbeats.py DBNDownBeatTrackingProcessor at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/detail/metrical_storage.h>
#include <dsp_kernels/downbeat_state_decoder.h>
#include <dsp_kernels/metrical_observations.h>
#include <dsp_kernels/metrical_events.h>
#include <dsp_kernels/metrical_transitions.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace dsp_kernels {
bool PreparedDownbeatStateDecoder::pattern(std::size_t index,const DownbeatPatternConfig& pattern) {
    if(!pattern.beats || !std::isfinite(pattern.minimum_bpm) || pattern.minimum_bpm<=0 ||
       !std::isfinite(pattern.maximum_bpm) || pattern.maximum_bpm<pattern.minimum_bpm)return false;
    const std::array<MetricalPatternConfig,1> layout{MetricalPatternConfig{pattern.beats,
        60.f*config_.frame_rate/pattern.maximum_bpm,60.f*config_.frame_rate/pattern.minimum_bpm,pattern.num_tempi}};
    if(!spaces_[index].prepare(layout))return false;
    const std::vector<TempoTransitionRule> rules(pattern.beats,{pattern.transition_lambda,pattern.fixed_interval});
    PreparedMetricalTransitions transitions;
    if(!transitions.prepare(spaces_[index],rules))return false;
    std::vector<std::uint32_t> pointers(spaces_[index].states());
    return metrical_observation_pointers(spaces_[index],config_.observation_lambda,MetricalObservation::BeatAndDownbeat,pointers) &&
        models_[index].prepare(maximum_frames_,3,pointers,transitions.edges());
}
bool PreparedDownbeatStateDecoder::configure(std::size_t maximum,std::span<const DownbeatPatternConfig> patterns,DownbeatStateDecoderConfig config) {
    if(!maximum || maximum>UINT32_MAX || patterns.empty() || !std::isfinite(config.frame_rate) || config.frame_rate<=0 ||
       !std::isfinite(float(maximum)/config.frame_rate) || !std::isfinite(config.observation_lambda) || config.observation_lambda<=1 ||
       !std::isfinite(config.activation_threshold) || config.activation_threshold<0 || config.activation_threshold>1)return false;
    maximum_frames_=maximum;config_=config;
    spaces_.resize(patterns.size());models_.resize(patterns.size());likelihoods_.resize(patterns.size());
    for(std::size_t i=0;i<patterns.size();++i)if(!pattern(i,patterns[i]))return false;
    times_.resize(maximum);numbers_.resize(maximum);frame_indices_.resize(maximum);return true;
}
bool PreparedDownbeatStateDecoder::prepare(std::size_t maximum,std::span<const DownbeatPatternConfig> patterns,DownbeatStateDecoderConfig config) {
    std::size_t bound=0;if(!storage_bytes(maximum,patterns,config,bound))return false;
    PreparedDownbeatStateDecoder next;
    if(!next.configure(maximum,patterns,config))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedDownbeatStateDecoder::valid(std::span<const float> input) const noexcept {
    if(!maximum_frames_ || input.size()%2 || input.size()/2>maximum_frames_)return false;
    for(std::size_t frame=0;frame<input.size()/2;++frame){
        const float beat=input[2*frame],downbeat=input[2*frame+1];
        if(!std::isfinite(beat) || !std::isfinite(downbeat) || beat<0 || downbeat<0 || beat+downbeat>1)return false;
    }
    return true;
}
void PreparedDownbeatStateDecoder::crop(std::span<const float> input,std::size_t& first,std::size_t& end) const noexcept {
    if(config_.activation_threshold<=0)return;
    const auto active=[&](std::size_t i){return input[2*i]>=config_.activation_threshold || input[2*i+1]>=config_.activation_threshold;};
    while(first<end && !active(first))++first;
    while(end>first && !active(end-1))--end;
    if(first==0 && end==1)end=0;
}
bool PreparedDownbeatStateDecoder::read(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& input=*static_cast<const Input*>(context);
    if(frame>=input.activations.size()/2)return false;
    return downbeat_activation_log_densities(input.activations.subspan(2*frame,2),input.lambda,output);
}
bool PreparedDownbeatStateDecoder::decode(std::span<const float> input) noexcept {
    const Input rows{input,config_.observation_lambda};
    float best=-std::numeric_limits<float>::infinity();selected_=0;
    for(std::size_t model=0;model<models_.size();++model){
        if(!models_[model].viterbi({&rows,read},input.size()/2))return false;
        likelihoods_[model]=models_[model].log_probability();
        if(likelihoods_[model]>best){best=likelihoods_[model];selected_=model;}
    }
    return true;
}
bool PreparedDownbeatStateDecoder::emit(std::size_t frame,std::span<const std::uint32_t> path) noexcept {
    if(count_==times_.size())return false;
    times_[count_]=float(frame+first_)/config_.frame_rate;
    numbers_[count_]=std::uint32_t(spaces_[selected_].positions()[path[frame]])+1;
    ++count_;return true;
}
bool PreparedDownbeatStateDecoder::corrected(std::span<const float> input,std::span<const std::uint32_t> path) noexcept {
    const auto pointers=models_[selected_].observation_pointers();std::size_t left=0;
    while(left<path.size()){
        if(pointers[path[left]]<1){++left;continue;}
        auto right=left+1;
        while(right<path.size() && pointers[path[right]]>=1)++right;
        const auto begin=input.begin()+std::ptrdiff_t(2*left),end=input.begin()+std::ptrdiff_t(2*right);
        const auto peak=std::size_t(std::max_element(begin,end)-input.begin())/2;
        if(!emit(peak,path))return false;
        left=right;
    }
    return true;
}
bool PreparedDownbeatStateDecoder::changed(std::span<const std::uint32_t> path) noexcept {
    if(!metrical_change_events(path,spaces_[selected_].positions(),frame_indices_,numbers_,count_))return false;
    for(std::size_t i=0;i<count_;++i)times_[i]=float(std::size_t(frame_indices_[i])+first_)/config_.frame_rate;
    return true;
}
bool PreparedDownbeatStateDecoder::process(std::span<const float> input) noexcept {
    selected_=std::numeric_limits<std::size_t>::max();first_=0;count_=0;
    std::fill(likelihoods_.begin(),likelihoods_.end(),-std::numeric_limits<float>::infinity());
    if(!valid(input))return false;
    std::size_t end=input.size()/2;crop(input,first_,end);
    const auto values=input.subspan(2*first_,2*(end-first_));
    if(!std::any_of(values.begin(),values.end(),[](float value){return value!=0;}))return true;
    if(!decode(values))return false;
    const auto path=models_[selected_].state_path();
    return config_.correct?corrected(values,path):changed(path);
}
std::span<const std::uint32_t> PreparedDownbeatStateDecoder::state_path() const noexcept {
    return selected_<models_.size()?models_[selected_].state_path():std::span<const std::uint32_t>{};
}
}


namespace dsp_kernels {
bool PreparedDownbeatStateDecoder::storage_bytes(std::size_t maximum,std::span<const DownbeatPatternConfig> patterns,DownbeatStateDecoderConfig c,std::size_t& out) noexcept {
    if(!maximum || maximum>UINT32_MAX || patterns.empty() || !std::isfinite(c.frame_rate) || c.frame_rate<=0 ||
       !std::isfinite(float(maximum)/c.frame_rate) || !std::isfinite(c.observation_lambda) || c.observation_lambda<=1 ||
       !std::isfinite(c.activation_threshold) || c.activation_threshold<0 || c.activation_threshold>1)return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,patterns.size(),sizeof(PreparedMetricalStateSpace)+sizeof(PreparedSparseMarkovModel)+sizeof(float)) ||
       !prepared_storage::add_array(bytes,maximum,sizeof(float)+2*sizeof(std::uint32_t)))return false;
    for(const auto& p:patterns){
        if(!p.beats || !std::isfinite(p.minimum_bpm) || p.minimum_bpm<=0 || !std::isfinite(p.maximum_bpm) || p.maximum_bpm<p.minimum_bpm)return false;
        detail::MetricalStorage s;
        const auto read=[&](std::size_t,MetricalPatternConfig& v){v={p.beats,60.f*c.frame_rate/p.maximum_bpm,60.f*c.frame_rate/p.minimum_bpm,p.num_tempi};return true;};
        if(!detail::metrical_storage(1,read,maximum,3,false,s) || !prepared_storage::add_bytes(bytes,s.total))return false;
    }
    out=bytes;return true;
}
std::size_t PreparedDownbeatStateDecoder::workspace_bytes() const noexcept {
    std::size_t bytes=spaces_.capacity()*sizeof(PreparedMetricalStateSpace)+models_.capacity()*sizeof(PreparedSparseMarkovModel)+
        (likelihoods_.capacity()+times_.capacity())*sizeof(float)+(numbers_.capacity()+frame_indices_.capacity())*sizeof(std::uint32_t);
    for(const auto& s:spaces_)bytes+=s.workspace_bytes();
    for(const auto& m:models_)bytes+=m.workspace_bytes();
    return bytes;
}
}
