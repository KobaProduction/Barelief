// Adapted from madmom features/downbeats.py PatternTrackingProcessor and
// features/beats_hmm.py GMMPatternTrackingObservationModel at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/detail/metrical_storage.h>
#include <dsp_kernels/pattern_state_decoder.h>
#include <dsp_kernels/metrical_transitions.h>
#include <dsp_kernels/metrical_events.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_kernels {
bool PreparedPatternStateDecoder::states(std::span<const GmmPatternData> data,std::span<const PatternTempoParameters> parameters) {
    std::vector<MetricalPatternConfig> patterns;
    patterns.reserve(data.size());std::size_t rule_count=0;
    for(const auto& pattern:data)if(!prepared_storage::add_bytes(rule_count,pattern.beats))return false;
    rules_.reserve(rule_count);
    for(std::size_t p=0;p<data.size();++p){
        const auto& tempo=parameters[parameters.size()==1?0:p];
        if(!data[p].beats || !std::isfinite(tempo.minimum_bpm) || tempo.minimum_bpm<=0 ||
           !std::isfinite(tempo.maximum_bpm) || tempo.maximum_bpm<tempo.minimum_bpm)return false;
        patterns.push_back({data[p].beats,60.f*rate_/tempo.maximum_bpm,60.f*rate_/tempo.minimum_bpm,tempo.num_tempi});
        if(tempo.beat_transitions.empty())rules_.insert(rules_.end(),data[p].beats,tempo.transition);
        else{
            if(tempo.beat_transitions.size()!=data[p].beats)return false;
            rules_.insert(rules_.end(),tempo.beat_transitions.begin(),tempo.beat_transitions.end());
        }
    }
    return space_.prepare(patterns);
}
bool PreparedPatternStateDecoder::mixtures(std::span<const GmmPatternData> data) {
    std::size_t count=0;for(const auto& p:data)if(!prepared_storage::add_bytes(count,p.gmms.size()))return false;
    gmms_.reserve(count);profile_offsets_.reserve(data.size()+1);
    profile_offsets_.push_back(0);
    for(const auto& pattern:data){
        if(pattern.gmms.empty() || pattern.gmms.size()>std::size_t(UINT32_MAX)-gmms_.size())return false;
        for(const auto& parameters:pattern.gmms){
            if(features_ && features_!=parameters.features)return false;
            PreparedGaussianMixture gmm;
            if(!gmm.prepare(parameters.parameters()))return false;
            features_=parameters.features;gmms_.push_back(std::move(gmm));
        }
        profile_offsets_.push_back(gmms_.size());
    }
    return true;
}
bool PreparedPatternStateDecoder::model(std::size_t maximum) {
    PreparedMetricalTransitions transitions;
    if(!transitions.prepare(space_,rules_))return false;
    std::vector<std::uint32_t> pointers(space_.states());
    const auto positions=space_.positions();const auto patterns=space_.state_patterns();
    for(std::size_t state=0;state<space_.states();++state){
        const auto p=patterns[state];const auto offset=profile_offsets_[p],profiles=profile_offsets_[p+1]-offset;
        const float density=positions[state]*float(profiles)/float(space_.beats(p))+float(offset);
        if(!std::isfinite(density) || density<float(offset) || density>=float(profile_offsets_[p+1]))return false;
        pointers[state]=std::uint32_t(density);
    }
    return model_.prepare(maximum,gmms_.size(),pointers,transitions.edges());
}
bool PreparedPatternStateDecoder::configure(std::size_t maximum,std::span<const GmmPatternData> data,
                                            std::span<const PatternTempoParameters> tempo,float rate) {
    if(!maximum || maximum>UINT32_MAX || data.empty() || (tempo.size()!=1 && tempo.size()!=data.size()) ||
       !std::isfinite(rate) || rate<=0 || !std::isfinite(float(maximum)/rate))return false;
    maximum_frames_=maximum;rate_=rate;
    if(!states(data,tempo) || !mixtures(data) || !model(maximum))return false;
    row_.resize(features_);times_.resize(maximum);frame_indices_.resize(maximum);numbers_.resize(maximum);return true;
}
bool PreparedPatternStateDecoder::prepare(std::size_t maximum,std::span<const GmmPatternData> data,
                                          std::span<const PatternTempoParameters> tempo,float rate) {
    std::size_t bound=0;if(!storage_bytes(maximum,data,tempo,rate,bound))return false;
    PreparedPatternStateDecoder next;
    if(!next.configure(maximum,data,tempo,rate))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedPatternStateDecoder::read(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& input=*static_cast<const Input*>(context);auto& self=*input.self;
    if(output.size()!=self.gmms_.size() || !input.features.read(input.features.context,frame,self.row_))return false;
    for(std::size_t mixture=0;mixture<output.size();++mixture)
        if(!self.gmms_[mixture].process(self.row_,output[mixture]))return false;
    return true;
}
bool PreparedPatternStateDecoder::process(FloatMatrixSource features,std::size_t frames) noexcept {
    count_=0;
    if(!maximum_frames_ || !features.read || frames>maximum_frames_)return false;
    const Input input{this,features};
    if(!model_.viterbi({&input,read},frames) ||
       !metrical_change_events(model_.state_path(),space_.positions(),frame_indices_,numbers_,count_))return false;
    for(std::size_t i=0;i<count_;++i)times_[i]=float(frame_indices_[i])/rate_;
    return true;
}
}


namespace dsp_kernels {
bool PreparedPatternStateDecoder::storage_bytes(std::size_t maximum,std::span<const GmmPatternData> data,
    std::span<const PatternTempoParameters> tempi,float rate,std::size_t& out) noexcept {
    if(!maximum || maximum>UINT32_MAX || data.empty() || (tempi.size()!=1 && tempi.size()!=data.size()) ||
       !std::isfinite(rate) || rate<=0 || !std::isfinite(float(maximum)/rate))return false;
    std::size_t bytes=0,models=0,features=0;
    for(const auto& p:data){
        if(p.gmms.empty() || !prepared_storage::add_bytes(models,p.gmms.size()))return false;
        for(const auto& g:p.gmms){
            std::size_t nested=0;
            if((features && features!=g.features) || !PreparedGaussianMixture::storage_bytes(g.parameters(),1e-7f,nested) ||
               !prepared_storage::add_bytes(bytes,nested))return false;
            features=g.features;
        }
    }
    detail::MetricalStorage s;
    const auto read=[&](std::size_t p,MetricalPatternConfig& v){
        const auto& t=tempi[tempi.size()==1?0:p];
        if(!std::isfinite(t.minimum_bpm) || t.minimum_bpm<=0 || !std::isfinite(t.maximum_bpm) || t.maximum_bpm<t.minimum_bpm ||
           (!t.beat_transitions.empty() && t.beat_transitions.size()!=data[p].beats))return false;
        v={data[p].beats,60.f*rate/t.maximum_bpm,60.f*rate/t.minimum_bpm,t.num_tempi};return true;
    };
    if(!detail::metrical_storage(data.size(),read,maximum,models,false,s) || !prepared_storage::add_bytes(bytes,s.total) ||
       !prepared_storage::add_array(bytes,models,sizeof(PreparedGaussianMixture)) ||
       !prepared_storage::add_array(bytes,data.size()+1,sizeof(std::size_t)) ||
       !prepared_storage::add_array(bytes,features,sizeof(float)) ||
       !prepared_storage::add_array(bytes,maximum,sizeof(float)+2*sizeof(std::uint32_t)))return false;
    out=bytes;return true;
}
std::size_t PreparedPatternStateDecoder::workspace_bytes() const noexcept {
    std::size_t bytes=space_.workspace_bytes()+model_.workspace_bytes()+rules_.capacity()*sizeof(TempoTransitionRule)+
        profile_offsets_.capacity()*sizeof(std::size_t)+gmms_.capacity()*sizeof(PreparedGaussianMixture)+
        (row_.capacity()+times_.capacity())*sizeof(float)+(frame_indices_.capacity()+numbers_.capacity())*sizeof(std::uint32_t);
    for(const auto& g:gmms_)bytes+=g.workspace_bytes();
    return bytes;
}
}
