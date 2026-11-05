// SPDX-License-Identifier: GPL-3.0-or-later
// Port of BTrack@9d6127618a5679e9caa74c594b88f1d74f0e035f/src/BTrack.cpp.
// Copyright (C) 2008-2014 Queen Mary University of London, Adam Stark.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// GPL version3 or any later version; no warranty. See ../COPYING.
#include <rhythm_reference_ports/predictive_beat_tracker.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
namespace {
bool normalized(std::span<float> values) noexcept {
    const float total=std::accumulate(values.begin(),values.end(),0.f);
    if(!std::isfinite(total))return false;
    if(total>0)for(auto& value:values)value/=total;
    return true;
}
float mean(std::span<const float> values,std::size_t begin,std::size_t end) noexcept {
    if(end<=begin)return 0;
    float result=0;
    return dsp_kernels::arithmetic_mean_float32(values.subspan(begin,end-begin),result)?
        result:std::numeric_limits<float>::quiet_NaN();
}
void fill_thresholds(std::span<const float> values,std::span<float> thresholds) noexcept {
    const auto count=values.size(),boundary=std::min(count,std::size_t{7});
    for(std::size_t i=0;i<=boundary && i<count;++i)thresholds[i]=mean(values,1,std::min(i+8,count));
    for(std::size_t i=boundary+1;i+7<count;++i)thresholds[i]=mean(values,i-8,i+7);
    for(std::size_t i=count>7?count-7:0;i<count;++i)thresholds[i]=mean(values,i>7?i-7:1,count);
}
bool prediction_geometry(std::size_t count,float period) noexcept {
    if(count>std::size_t(PTRDIFF_MAX)/2 || !std::isfinite(period) || period<1)return false;
    const float width=std::round(2.f*period);
    return std::isfinite(width) && width<float(SIZE_MAX) && std::size_t(width)<=count;
}
bool hop_geometry(std::size_t hop,std::size_t& count,float& period) noexcept {
    if(!hop || hop>262144)return false;
    count=std::size_t{262144}/hop;period=std::round(60.f*44100.f/(float(hop)*120.f));
    return prediction_geometry(count,period);
}
dsp_kernels::SpectralAutocorrelationConfig predictive_correlation() noexcept {
    dsp_kernels::SpectralAutocorrelationConfig config;config.fft_size=1024;
    config.normalization=dsp_kernels::CorrelationNormalization::Unbiased;
    config.inverse_output=dsp_kernels::AutocorrelationInverseOutput::ComplexMagnitude;
    config.transform=dsp_kernels::AutocorrelationTransform::Complex;return config;
}
float folded_tempo(float bpm) noexcept {
    if(!std::isfinite(bpm) || bpm<=0)return 0;
    while(bpm>160)bpm*=.5f;
    while(bpm<80)bpm*=2.f;
    return bpm;
}
}
bool PreparedCombTempoTransition::prepare(std::size_t hop,dsp_kernels::NumericalPrecision precision,
                                          const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t bytes=0;if(!storage_bytes(hop,bytes,precision,factory))return false;
    PreparedCombTempoTransition next;
    if(!next.configure(hop,precision,factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedCombTempoTransition::configure(std::size_t hop,dsp_kernels::NumericalPrecision precision,
                                            const dsp_kernels::FourierBackendFactory* factory) {
    if(precision==dsp_kernels::NumericalPrecision::DesktopReference){
        if(!reference_.prepare(hop,factory))return false;
        hop_=hop;publish_reference();return true;
    }
    if(!correlation_.prepare(512,predictive_correlation()))return false;
    hop_=hop;previous_.fill(1);fixed_previous_.fill(0);prepare_weights();return true;
}
bool PreparedCombTempoTransition::storage_bytes(std::size_t hop,std::size_t& output,dsp_kernels::NumericalPrecision precision,
                                                const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(precision==dsp_kernels::NumericalPrecision::DesktopReference)
        return detail::PreparedPredictiveReferenceComb::storage_bytes(hop,output,factory);
    if(precision!=dsp_kernels::NumericalPrecision::PortableFloat32 || factory)return false;
    return hop && hop<=262144 && dsp_kernels::PreparedSpectralAutocorrelation::storage_bytes(512,predictive_correlation(),output);
}
void PreparedCombTempoTransition::prepare_weights() noexcept {
    for(std::size_t i=0;i<rayleigh_.size();++i)
        rayleigh_[i]=float(i)/(43.f*43.f)*std::exp(-float(i*i)/(2.f*43.f*43.f));
    for(std::size_t from=0;from<41;++from)for(std::size_t to=0;to<41;++to){
        const float distance=float(to)-float(from);
        transitions_[from*41+to]=std::exp(-distance*distance/50.f)/(5.f*std::sqrt(2.f*std::numbers::pi_v<float>));
    }
}
void PreparedCombTempoTransition::reset() noexcept {
    reference_.reset();
    fixed_=false;bpm_=120;previous_.fill(1);fixed_previous_.fill(0);current_.fill(0);
    onset_.fill(0);acf_.fill(0);threshold_.fill(0);comb_.fill(0);observation_.fill(0);
}
bool PreparedCombTempoTransition::set_hop_samples(std::size_t hop) noexcept {
    if(!hop_ || !hop || hop>262144)return false;
    if(reference_.ready() && !reference_.set_hop_samples(hop))return false;
    hop_=hop;return true;
}
void PreparedCombTempoTransition::threshold(std::span<float> values) noexcept {
    fill_thresholds(values,threshold_);
    for(std::size_t i=0;i<values.size();++i)values[i]=std::max(0.f,values[i]-threshold_[i]);
}
bool PreparedCombTempoTransition::balanced_acf() noexcept {
    return correlation_.process(onset_,acf_);
}
void PreparedCombTempoTransition::comb_filter() noexcept {
    comb_.fill(0);
    for(std::size_t period=2;period<=127;++period)
        for(std::ptrdiff_t harmonic=1;harmonic<=4;++harmonic)
            for(std::ptrdiff_t offset=1-harmonic;offset<=harmonic-1;++offset){
                const auto index=std::size_t(std::ptrdiff_t(period)*harmonic+offset-1);
                comb_[period-1]+=acf_[index]*rayleigh_[period-1]/float(2*harmonic-1);
            }
}
void PreparedCombTempoTransition::observations() noexcept {
    constexpr float factor=60.f*44100.f/512.f;
    for(std::size_t i=0;i<observation_.size();++i){
        const auto first=std::size_t(std::round(factor/float(80+2*i)));
        const auto second=std::size_t(std::round(factor/float(160+4*i)));
        observation_[i]=comb_[first-1]+comb_[second-1];
    }
}
void PreparedCombTempoTransition::transition() noexcept {
    if(fixed_)previous_=fixed_previous_;
    for(std::size_t to=0;to<41;++to){
        float maximum=0;
        for(std::size_t from=0;from<41;++from)maximum=std::max(maximum,previous_[from]*transitions_[from*41+to]);
        current_[to]=maximum*observation_[to];
    }
}
float PreparedCombTempoTransition::select() noexcept {
    const auto selected=std::size_t(std::max_element(current_.begin(),current_.end())-current_.begin());
    previous_=current_;const float period=std::round(60.f*44100.f/(float(80+2*selected)*float(hop_)));
    bpm_=period>0?60.f*44100.f/(float(hop_)*period):0;return period;
}
bool PreparedCombTempoTransition::process(std::span<const float> values,float& period) noexcept {
    if(reference_.ready()){
        if(!reference_.process(values,period))return false;
        publish_reference();return true;
    }
    return process_portable(values,period);
}
bool PreparedCombTempoTransition::process_portable(std::span<const float> values,float& period) noexcept {
    return analyze_observations(values) && advance_observations(observation_,period);
}
bool PreparedCombTempoTransition::analyze_observations(std::span<const float> values) noexcept {
    if(reference_.ready())return false;
    if(!hop_ || values.size()!=onset_.size() || !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}))return false;
    std::copy(values.begin(),values.end(),onset_.begin());threshold(onset_);
    if(!balanced_acf())return false;
    comb_filter();threshold(comb_);observations();return true;
}
bool PreparedCombTempoTransition::advance_observations(std::span<const float> values,float& period) noexcept {
    if(reference_.ready() || !hop_ || values.size()!=observation_.size() ||
       !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x) && x>=0;}))return false;
    std::copy(values.begin(),values.end(),observation_.begin());transition();
    if(!normalized(current_))return false;
    period=select();return period>0;
}
std::size_t PreparedCombTempoTransition::tempo_index(float bpm) const noexcept {
    const float folded=folded_tempo(bpm);return folded>0?std::size_t(std::round((folded-80.f)/2.f)):41;
}
bool PreparedCombTempoTransition::set_tempo(float bpm) noexcept {
    if(reference_.ready()){
        if(!reference_.set_tempo(bpm))return false;
        publish_reference();return true;
    }
    if(!hop_)return false;
    const auto index=tempo_index(bpm);if(index>=41)return false;
    previous_.fill(0);previous_[index]=1;return true;
}
bool PreparedCombTempoTransition::fix_tempo(float bpm) noexcept {
    if(reference_.ready())return reference_.fix_tempo(bpm);
    if(!hop_)return false;
    const auto index=tempo_index(bpm);if(index>=41)return false;
    fixed_previous_.fill(0);fixed_previous_[index]=1;fixed_=true;return true;
}
void PreparedCombTempoTransition::release_tempo() noexcept {fixed_=false;reference_.release_tempo();}
void PreparedCombTempoTransition::publish_reference() noexcept {
    std::copy(reference_.response().begin(),reference_.response().end(),comb_.begin());
    std::copy(reference_.observations().begin(),reference_.observations().end(),observation_.begin());
    std::copy(reference_.probabilities().begin(),reference_.probabilities().end(),previous_.begin());
    bpm_=float(reference_.bpm());
}
bool PreparedBeatPrediction::prepare(std::size_t count,float period,float tightness,float alpha) {
    std::size_t bytes=0;if(!storage_bytes(count,period,tightness,alpha,bytes))return false;
    PreparedBeatPrediction next;if(!next.score_.prepare(count))return false;
    next.period_=period;next.tightness_=tightness;next.alpha_=alpha;
    next.future_.resize(2*count);next.weight_.resize(2*count);if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedBeatPrediction::storage_bytes(std::size_t count,float period,float tightness,float alpha,std::size_t& output) noexcept {
    if(!prediction_geometry(count,period) || !std::isfinite(tightness) || tightness<=0 || !std::isfinite(alpha) || alpha<0 || alpha>1)return false;
    return dsp_kernels::prepared_storage::mul_bytes(count,6*sizeof(float),output);
}
std::size_t PreparedBeatPrediction::weighting(float period) noexcept {
    const auto width=std::size_t(std::round(2.f*period)-std::round(period*.5f))+1;
    for(std::size_t i=0;i<width;++i){
        const float value=-2.f*period+float(i),distance=tightness_*std::log(-value/period);
        weight_[i]=std::exp(-distance*distance*.5f);
    }
    return width;
}
float PreparedBeatPrediction::cumulative(std::span<const float> values,std::size_t start,std::size_t end,float onset,float alpha,std::span<const float> future) const noexcept {
    float maximum=0;
    for(std::size_t i=start;i<=end;++i){
        const float score=i<values.size()?values[i]:future[i-values.size()];
        maximum=std::max(maximum,score*weight_[i-start]);
    }
    return (1.f-alpha)*onset+alpha*maximum;
}
bool PreparedBeatPrediction::predict() noexcept {
    const auto history=score_.values();const auto count=history.size(),steps=std::size_t(period_);
    const auto start=count-std::size_t(std::round(2.f*period_)),end=count-std::size_t(std::round(.5f*period_));
    weighting(period_);
    for(std::size_t i=0;i<steps;++i)future_[i]=cumulative(history,start+i,end+i,0,1,future_);
    select_next_beat(steps);
    next_prediction_=next_beat_+std::ptrdiff_t(std::round(period_*.5f));return true;
}
void PreparedBeatPrediction::select_next_beat(std::size_t steps) noexcept {
    float maximum=0;
    for(std::size_t i=0;i<steps;++i){
        const float delta=float(i+1)-period_*.5f;
        const float weighted=future_[i]*std::exp(-delta*delta/(2.f*period_*.5f*period_*.5f));
        if(weighted>maximum){maximum=weighted;next_beat_=std::ptrdiff_t(i);}
    }
}
bool PreparedBeatPrediction::set_period(float period) noexcept {
    if(!prediction_geometry(score_.values().size(),period))return false;
    period_=period;return true;
}
bool PreparedBeatPrediction::reset(float period) noexcept {
    if(!set_period(period))return false;
    score_.clear();std::fill(future_.begin(),future_.end(),0.f);std::fill(weight_.begin(),weight_.end(),0.f);
    next_beat_=-1;next_prediction_=10;return true;
}
bool PreparedBeatPrediction::resize_history(std::size_t count,float period) {
    if(score_.values().empty() || !prediction_geometry(count,period))return false;
    ScalarHistory score;if(!score.prepare(count))return false;
    std::vector<float> future(2*count),weight(2*count);
    score_=std::move(score);future_=std::move(future);weight_=std::move(weight);period_=period;return true;
}
bool PreparedBeatPrediction::indicate_tempo(float period) noexcept {
    if(!std::isfinite(period) || period<1 || period>float(score_.values().size()))return false;
    std::size_t position=1;const auto interval=std::size_t(period);
    for(std::size_t i=score_.values().size();i-->0;){score_.set(i,position==1?150.f:10.f);if(++position>interval)position=1;}
    next_beat_=0;next_prediction_=std::ptrdiff_t(std::round(period*.5f));return true;
}
bool PreparedBeatPrediction::process(float onset,BeatPredictionObservation& output) noexcept {
    if(score_.values().empty() || !std::isfinite(onset))return false;
    --next_prediction_;--next_beat_;
    const auto count=score_.values().size();const auto start=count-std::size_t(std::round(2.f*period_)),end=count-std::size_t(std::round(period_*.5f));
    weighting(period_);const float value=cumulative(score_.values(),start,end,onset,alpha_);
    if(!std::isfinite(value))return false;
    score_.push(value);if(next_prediction_==0 && !predict())return false;
    output={next_beat_==0,next_beat_,next_prediction_,period_};return true;
}
bool PreparedPredictiveBeatTracker::prepare(std::size_t hop,OnsetWindowResamplingPort resampler,
                                           dsp_kernels::NumericalPrecision precision,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t count=0;float period=0;
    std::size_t bytes=0;if(!resampler.process || !storage_bytes(hop,bytes,precision,factory) || !hop_geometry(hop,count,period))return false;
    PreparedPredictiveBeatTracker next;
    if(!next.tempo_.prepare(hop,precision,factory) || !next.prediction_.prepare(count,period) || !next.onset_.prepare(count))return false;
    next.resampler_=resampler;next.hop_=hop;
    seed_onsets(next.onset_,period);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPredictiveBeatTracker::storage_bytes(std::size_t hop,std::size_t& output,dsp_kernels::NumericalPrecision precision,
                                                 const dsp_kernels::FourierBackendFactory* factory) noexcept {
    std::size_t count=0;float period=0;if(!hop_geometry(hop,count,period))return false;
    std::size_t bytes=0,nested=0;
    if(!PreparedCombTempoTransition::storage_bytes(hop,bytes,precision,factory) || !PreparedBeatPrediction::storage_bytes(count,period,5,.9f,nested) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,nested) || !ScalarHistory::storage_bytes(count,nested) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,nested))return false;
    output=bytes;return true;
}
void PreparedPredictiveBeatTracker::seed_onsets(ScalarHistory& history,float period) noexcept {
    for(std::size_t i=0;i<history.values().size();++i)history.set(i,i%std::size_t(period)==0?1.f:0.f);
}
bool PreparedPredictiveBeatTracker::reset() noexcept {
    std::size_t count=0;float period=0;
    if(!hop_geometry(hop_,count,period) || !prediction_.reset(period))return false;
    tempo_.reset();onset_.clear();seed_onsets(onset_,period);resampled_.fill(0);return true;
}
bool PreparedPredictiveBeatTracker::set_hop_samples(std::size_t hop) {
    std::size_t count=0;float period=0;
    if(!hop_ || !hop_geometry(hop,count,period))return false;
    ScalarHistory onset;if(!onset.prepare(count) || !prediction_.resize_history(count,period))return false;
    if(!tempo_.set_hop_samples(hop))return false;
    seed_onsets(onset,period);onset_=std::move(onset);hop_=hop;return true;
}
bool PreparedPredictiveBeatTracker::process(float value,BeatPredictionObservation& output) noexcept {
    if(!hop_ || !std::isfinite(value))return false;
    return process_prepared(std::abs(value)+.0001f,output);
}
bool PreparedPredictiveBeatTracker::process_reference(double value,BeatPredictionObservation& output) noexcept {
    if(!hop_ || !std::isfinite(value))return false;
    const double adjusted=std::abs(value)+.0001;
    if(!std::isfinite(adjusted) || adjusted>std::numeric_limits<float>::max())return false;
    return process_prepared(float(adjusted),output);
}
bool PreparedPredictiveBeatTracker::process_prepared(float value,BeatPredictionObservation& output) noexcept {
    onset_.push(value);
    BeatPredictionObservation result;if(!prediction_.process(value,result))return false;
    if(result.beat_due){
        if(!resampler_.process(resampler_.context,onset_.values(),resampled_))return false;
        float period=0;if(!tempo_.process(resampled_,period) || !prediction_.set_period(period))return false;
        result.period_frames=period;
    }
    output=result;return true;
}
bool PreparedPredictiveBeatTracker::set_tempo(float value) noexcept {
    const float tempo=folded_tempo(value);if(!hop_ || tempo<=0 || !tempo_.set_tempo(tempo))return false;
    const float period=std::round(60.f*44100.f/(float(hop_)*tempo));
    if(!prediction_.indicate_tempo(period))return false;
    const auto interval=std::size_t(period);std::size_t position=1;
    for(std::size_t i=onset_.values().size();i-->0;){onset_.set(i,position==1?150.f:10.f);if(++position>interval)position=1;}
    return true;
}
}
