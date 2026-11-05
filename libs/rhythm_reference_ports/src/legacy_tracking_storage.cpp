// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/legacy_rhythm_recipe.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <climits>
namespace rhythm_reference_ports {
namespace {
using namespace dsp_kernels::prepared_storage;
template<class T> std::size_t bytes(const std::vector<T>& v) noexcept {return v.capacity()*sizeof(T);}
template<class... T> std::size_t owned(const T&... v) noexcept {return (bytes(v)+...+std::size_t{});}
bool positive(float v) noexcept {return std::isfinite(v) && v>0;}
bool floats(std::size_t& b,std::size_t n,std::size_t m=1) noexcept {std::size_t count=0;return mul_bytes(n,m,count) && add_array(b,count,sizeof(float));}
InterpolatedPeakConfig period_phase_peak_config(std::size_t comb) noexcept {
    InterpolatedPeakConfig peak;peak.range=float(comb-1);peak.maximum=float(comb);
    peak.ordering=PeakOrdering::Amplitude;peak.maximum_peaks=1;return peak;
}
bool valid_tempo_hints(std::span<const float> hints,float frame_time,float minimum,float maximum) noexcept {
    if(!std::all_of(hints.begin(),hints.end(),[](float v){return std::isfinite(v);}))return false;
    if(hints.size()>2){const float period=(hints.back()-hints.front())/float(hints.size()-1)/frame_time;if(!std::isfinite(period) || period<=minimum || period>=maximum)return false;}
    return true;
}
}
bool PreparedBandTempoFeatures::storage_bytes(std::span<const float> gains,float time,std::size_t& b) noexcept {
    b=0;return !gains.empty() && positive(time) && std::isfinite(std::sqrt(256.f/time)) && std::all_of(gains.begin(),gains.end(),[](float v){return std::isfinite(v);}) && floats(b,gains.size(),4);
}
std::size_t PreparedBandTempoFeatures::workspace_bytes() const noexcept {return owned(gains_,previous_,current_,result_);}
bool PreparedCombPeriodPhase::storage_bytes(CombPeriodPhaseConfig c,std::span<const float> hints,std::size_t& b,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    b=0;PreparedCombPeriodPhase shape;if(!shape.geometry(c))return false;
    if(!valid_tempo_hints(hints,shape.frame_time_,shape.minimum_lag_,shape.maximum_lag_))return false;
    dsp_kernels::SpectralAutocorrelationConfig acf;acf.normalization=dsp_kernels::CorrelationNormalization::Unbiased;
    const auto comb=c.number_frames/4,phase=std::size_t(std::round(float(c.number_frames)/4.f));
    const auto peak=period_phase_peak_config(comb);
    std::size_t part=0;return dsp_kernels::PreparedSpectralAutocorrelation::storage_bytes(c.number_frames,acf,b,factory) && PreparedInterpolatedPeaks::storage_bytes(std::max(comb,phase),peak,part) && add_bytes(b,part) && floats(b,c.number_frames) && floats(b,comb,2) && floats(b,phase);
}
std::size_t PreparedCombPeriodPhase::workspace_bytes() const noexcept {return owned(weights_,acf_,comb_,phase_response_)+autocorrelation_.workspace_bytes()+detector_.workspace_bytes();}
bool PreparedFeaturePeriodTracker::storage_bytes(std::size_t n,std::size_t hop,CombPeriodPhaseConfig c,std::span<const float> hints,std::size_t& b,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    b=0;std::size_t history=0,all=0;
    return n && hop && hop<=c.number_frames && PreparedCombPeriodPhase::storage_bytes(c,hints,b,factory) && ScalarHistory::storage_bytes(c.number_frames,history) &&
        mul_bytes(n,history,all) && add_bytes(b,all) && add_array(b,n,sizeof(ScalarHistory)) && floats(b,n,2);
}
std::size_t PreparedFeaturePeriodTracker::workspace_bytes() const noexcept {
    std::size_t b=owned(history_,periods_,phases_)+estimator_.workspace_bytes();for(const auto& h:history_)b+=h.workspace_bytes();return b;
}
bool PreparedPeriodPhaseTicks::storage_bytes(std::size_t n,std::size_t matches,std::size_t ticks,PeriodPhaseTickConfig c,std::size_t& b) noexcept {
    b=0;if(!n || n>std::size_t(PTRDIFF_MAX)-12 || !matches || !ticks || !c.evaluation_hop || c.evaluation_hop>INT_MAX || !c.audio_hop || !positive(c.sample_rate) || !positive(c.maximum_candidate_frames) || c.maximum_candidate_frames>std::nextafter(float(INT_MAX),0.f))return false;
    const auto count=std::size_t(c.maximum_candidate_frames/2.f+.5f)+1;
    return positive(float(c.audio_hop)/c.sample_rate) && floats(b,n) && floats(b,n+12) && floats(b,matches) && floats(b,ticks) && add_array(b,count,sizeof(std::uint64_t));
}
std::size_t PreparedPeriodPhaseTicks::workspace_bytes() const noexcept {return owned(periods_,phases_,matches_,ticks_,counts_);}
bool PreparedFrameSilencePolicy::storage_bytes(std::size_t n,FrameSilenceConfig c,std::size_t& b) noexcept {
    b=0;return n && std::isfinite(c.threshold_power) && c.threshold_power>=0 && std::isfinite(c.noise_trigger_power) && c.noise_trigger_power>=0 && std::isfinite(c.noise_gain) && c.noise_gain>=0 && unsigned(c.boundaries)<=1 && floats(b,n,2);
}
std::size_t PreparedFrameSilencePolicy::workspace_bytes() const noexcept {return owned(ones_,scratch_);}
bool PreparedLagObservationPool::storage_bytes(std::size_t rows,std::size_t values,std::size_t flat,std::size_t& b) noexcept {
    b=0;return rows && values && flat && floats(b,values) && add_array(b,rows,sizeof(Row));
}
std::size_t PreparedLagObservationPool::workspace_bytes() const noexcept {return owned(values_,rows_);}
bool PreparedTickSequenceRefinement::storage_bytes(std::size_t n,TickSequenceRefinementConfig c,std::size_t& b) noexcept {
    b=0;return n && positive(c.frame_rate) && std::isfinite(c.minimum_interval_seconds) && c.minimum_interval_seconds>=0 && std::isfinite(c.last_beat_interval_seconds) && c.last_beat_interval_seconds>=0 && floats(b,n,2);
}
std::size_t PreparedTickSequenceRefinement::workspace_bytes() const noexcept {return owned(ticks_,intervals_);}
namespace {
bool legacy_frames(LegacyRhythmRecipeLimits l,LegacyRhythmRecipeConfig c,std::size_t& frames,std::size_t& b,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(!l.maximum_pcm_samples || l.maximum_pcm_samples>INT_MAX || !l.maximum_ticks || !l.maximum_pool_values || c.audio_frame<2 || c.audio_frame>INT_MAX || !c.audio_hop || c.audio_hop>INT_MAX || !c.evaluation_hop || !positive(c.sample_rate) || (!c.use_onset && !c.use_bands))return false;
    dsp_kernels::TemporalWindowConfig w;w.length=c.audio_frame;w.hop=c.audio_hop;w.function=dsp_kernels::WindowFunction::Rectangular;w.odd_center_ceil=true;w.tail=dsp_kernels::TemporalTail::CenterCoverEnd;
    std::size_t part=0;if(!dsp_kernels::PreparedTemporalWindow::frame_count(w,l.maximum_pcm_samples,frames) || !frames || !dsp_kernels::PreparedTemporalWindow::storage_bytes(w,{},b))return false;
    w.center=false;w.tail=dsp_kernels::TemporalTail::Drop;
    return dsp_kernels::PreparedFourierTempogram::storage_bytes(w,{},c.audio_frame,true,part,factory) && add_bytes(b,part) && floats(b,c.audio_frame,2) && floats(b,c.audio_frame/2+1,4) && floats(b,11);
}
bool legacy_features(LegacyRhythmRecipeConfig c,std::size_t& b) noexcept {
    std::size_t part=0;const auto bins=c.audio_frame/2+1;
    if(c.use_onset){
        if(!PreparedSpectralOnset::storage_bytes(bins,{SpectralOnsetMethod::Hfc,c.sample_rate},part) || !add_bytes(b,part) || !PreparedSpectralOnset::storage_bytes(bins,{SpectralOnsetMethod::Complex,c.sample_rate},part) || !add_bytes(b,part))return false;
        if(!dsp_kernels::PreparedRecursiveFilter::storage_bytes(2,1,{},part) || !add_bytes(b,part))return false;
    }
    if(!c.use_bands)return true;
    constexpr float borders[]{40,413.16f,974.51f,1818.94f,3089.19f,5000,7874.4f,12198.29f,17181.13f};
    return PreparedFrequencyBandEnergy::storage_bytes(bins,44100,borders,part) && add_bytes(b,part) && PreparedBandTempoFeatures::storage_bytes(default_tempo_band_gains,512,part) && add_bytes(b,part) && floats(b,8);
}
}
bool PreparedLegacyRhythmRecipe::storage_bytes(LegacyRhythmRecipeLimits l,LegacyRhythmRecipeConfig c,std::span<const float> hints,std::size_t& b,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(unsigned(c.window_precision)>unsigned(dsp_kernels::NumericalPrecision::DesktopReference))return false;
    b=0;std::size_t frames=0,part=0;if(!legacy_frames(l,c,frames,b,factory) || !legacy_features(c,b))return false;
    const auto features=std::size_t(c.use_onset)*3+std::size_t(c.use_bands)*8;
    CombPeriodPhaseConfig period{c.period_window,c.audio_hop,c.sample_rate,c.minimum_bpm,c.maximum_bpm};
    if(!PreparedFrameSilencePolicy::storage_bytes(c.audio_frame,c.silence,part) || !add_bytes(b,part) || !PreparedFeaturePeriodTracker::storage_bytes(features,c.evaluation_hop,period,hints,part,factory) || !add_bytes(b,part))return false;
    PeriodPhaseTickConfig tick{c.evaluation_hop,c.audio_hop,c.sample_rate,float(c.period_window)};
    if(!PreparedPeriodPhaseTicks::storage_bytes(features,l.maximum_pool_values,l.maximum_ticks,tick,part) || !add_bytes(b,part) || !PreparedLagObservationPool::storage_bytes(std::max(std::size_t{1},frames/c.evaluation_hop),l.maximum_pool_values,l.maximum_pool_values,part) || !add_bytes(b,part))return false;
    TickSequenceRefinementConfig refinement{c.sample_rate/float(c.audio_hop),c.minimum_interval_seconds,c.last_beat_interval_seconds};
    LagTempoVoteConfig vote{c.sample_rate,std::max(float(c.maximum_bpm),60.f*c.sample_rate/float(c.audio_hop)),c.audio_hop};
    return PreparedTickSequenceRefinement::storage_bytes(l.maximum_ticks,refinement,part) && add_bytes(b,part) && PreparedLagTempoVote::storage_bytes(l.maximum_pool_values,vote,part) && add_bytes(b,part) && floats(b,l.maximum_ticks);
}
std::size_t PreparedLegacyRhythmRecipe::workspace_bytes() const noexcept {
    return owned(frame_,complex_,magnitude_,phase_,features_,band_power_,candidates_)+framing_.workspace_bytes()+spectrum_.workspace_bytes()+derivative_.storage_bytes()+silence_.workspace_bytes()+
        hfc_.workspace_bytes()+complex_onset_.workspace_bytes()+bands_.workspace_bytes()+scaled_bands_.workspace_bytes()+periods_.workspace_bytes()+ticks_.workspace_bytes()+pool_.workspace_bytes()+refinement_.workspace_bytes()+vote_.workspace_bytes();
}
} // namespace rhythm_reference_ports
