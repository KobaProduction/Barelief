// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia PercivalBpmEstimator's Gaussian accumulation/octave decider.
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/lag_tempo_consensus.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>

namespace rhythm_reference_ports {
namespace {
bool valid_classifier(const TempoOctaveClassifier& model) noexcept {
    for(std::size_t i=0;i<3;++i){
        if(!std::isfinite(model.minimum[i]) || !std::isfinite(model.maximum[i]) || model.maximum[i]<=model.minimum[i])return false;
        if(!std::all_of(model.weights[i].begin(),model.weights[i].end(),[](float value){return std::isfinite(value);}))return false;
    }
    return true;
}
float linear_score(const std::array<float,4>& weights,const std::array<float,3>& values) noexcept {
    return weights[3]+values[0]*weights[0]+values[1]*weights[1]+values[2]*weights[2];
}
}
bool PreparedLagTempoConsensus::storage_bytes(float rate,TempoOctaveClassifier model,std::size_t& out) noexcept {
    if(!std::isfinite(rate) || rate<=0 || !valid_classifier(model))return false;
    out=0;return true;
}
std::size_t PreparedLagTempoConsensus::workspace_bytes() const noexcept {return 0;}
bool PreparedLagTempoConsensus::prepare(float rate,TempoOctaveClassifier classifier) noexcept {
    std::size_t bytes=0;if(!storage_bytes(rate,classifier,bytes))return false;
    frame_rate_=rate;classifier_=classifier;reset();
    const float factor=1.f/(10.f*std::sqrt(2.f*std::numbers::pi_v<float>));
    for(std::size_t i=0;i<gaussian_.size();++i){
        const float difference=float(i)-1000.f;
        gaussian_[i]=factor*std::exp(difference*difference/-200.f);
    }
    return true;
}
void PreparedLagTempoConsensus::reset() noexcept {
    histogram_.fill(0);observations_=0;
}
float PreparedLagTempoConsensus::gaussian(std::int64_t index) const noexcept {
    return index>=0 && std::uint64_t(index)<gaussian_.size()?gaussian_[std::size_t(index)]:0;
}
bool PreparedLagTempoConsensus::add(std::int32_t lag) noexcept {
    if(frame_rate_<=0)return false;
    if(lag<0)return true;
    if(observations_==std::numeric_limits<std::uint64_t>::max())return false;
    for(std::size_t i=0;i<histogram_.size();++i)histogram_[i]+=gaussian(1000-std::int64_t(lag)+std::int64_t(i));
    ++observations_;return true;
}
float PreparedLagTempoConsensus::range_energy(float low,float high,float scale) const noexcept {
    const auto first=std::max(std::ptrdiff_t(0),std::ptrdiff_t(std::round(low)));
    const auto last=std::min(std::ptrdiff_t(histogram_.size()-1),std::ptrdiff_t(std::round(high)));
    float result=0;
    for(auto i=first;i<=last;++i)result+=histogram_[std::size_t(i)];
    return scale*result;
}
bool PreparedLagTempoConsensus::features(LagTempoConsensusResult& result) const noexcept {
    result.lag=std::size_t(std::max_element(histogram_.begin(),histogram_.end())-histogram_.begin());
    if(!result.lag)return false;
    const float total=std::accumulate(histogram_.begin(),histogram_.end(),0.f);
    if(!std::isfinite(total) || total<=0)return false;
    result.uncorrected_bpm=frame_rate_*60.f/float(result.lag);
    result.features={range_energy(0,float(result.lag)-10.f,1.f/total),
        range_energy(.5f*float(result.lag)-10.f,.5f*float(result.lag)+10.f,1.f/total),result.uncorrected_bpm};
    return std::isfinite(result.uncorrected_bpm);
}
float PreparedLagTempoConsensus::octave_multiplier(std::array<float,3> values) const noexcept {
    for(std::size_t i=0;i<values.size();++i)values[i]=(values[i]-classifier_.minimum[i])/(classifier_.maximum[i]-classifier_.minimum[i]);
    const float sum51=linear_score(classifier_.weights[0],values),sum52=linear_score(classifier_.weights[1],values);
    const float sum12=linear_score(classifier_.weights[2],values);
    float multiplier=1;
    if(sum52>0 && sum12>0)multiplier=2;
    if(sum51<=0 && sum52<=0)multiplier=.5f;
    return multiplier;
}
bool PreparedLagTempoConsensus::finish(LagTempoConsensusResult& output) const noexcept {
    if(frame_rate_<=0)return false;
    LagTempoConsensusResult next;
    if(observations_){
        if(!features(next))return false;
        next.octave_multiplier=octave_multiplier(next.features);next.bpm=next.uncorrected_bpm*next.octave_multiplier;
        if(!std::isfinite(next.bpm))return false;
    }
    output=next;return true;
}
}
