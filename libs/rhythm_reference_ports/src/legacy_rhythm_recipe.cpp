// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia RhythmExtractor at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/legacy_rhythm_recipe.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
constexpr std::array<float,9> frequency_borders{40,413.16f,974.51f,1818.94f,3089.19f,5000,7874.4f,12198.29f,17181.13f};
}
std::size_t PreparedLegacyRhythmRecipe::feature_count() const noexcept {
    return std::size_t(config_.use_onset)*3+std::size_t(config_.use_bands)*8;
}
bool PreparedLegacyRhythmRecipe::prepare_frames(const dsp_kernels::FourierBackendFactory* factory) {
    dsp_kernels::TemporalWindowConfig frame;frame.length=config_.audio_frame;frame.hop=config_.audio_hop;
    frame.function=dsp_kernels::WindowFunction::Rectangular;frame.odd_center_ceil=true;
    frame.tail=dsp_kernels::TemporalTail::CenterCoverEnd;
    if(!framing_.prepare(frame) || !silence_.prepare(frame.length,config_.silence))return false;
    frame.center=false;frame.tail=dsp_kernels::TemporalTail::Drop;
    std::vector<float> window(frame.length);
    SpectralWindowConfig shape;shape.precision=config_.window_precision;
    if(!spectral_window(window,shape) || !spectrum_.prepare(frame,window,0,true,factory))return false;
    allocate_frame_buffers(frame.length);
    return true;
}
void PreparedLegacyRhythmRecipe::allocate_frame_buffers(std::size_t length) {
    maximum_frames_=framing_.frames(limits_.maximum_pcm_samples);
    frame_.resize(length);complex_.resize(2*spectrum_.bins());
    magnitude_.resize(spectrum_.bins());phase_.resize(spectrum_.bins());features_.resize(feature_count());
}
bool PreparedLegacyRhythmRecipe::prepare_features() {
    if(config_.use_onset){
        const std::array<float,2> numerator{1,-1};const std::array<float,1> denominator{1};
        if(!hfc_.prepare(spectrum_.bins(),{SpectralOnsetMethod::Hfc,config_.sample_rate}) ||
           !complex_onset_.prepare(spectrum_.bins(),{SpectralOnsetMethod::Complex,config_.sample_rate}) ||
           !derivative_.prepare(numerator,denominator,dsp_kernels::SubnormalStatePolicy::Preserve))return false;
    }
    if(config_.use_bands){
        if(!bands_.prepare(spectrum_.bins(),44100,frequency_borders) || !scaled_bands_.prepare())return false;
        band_power_.resize(8);
    }
    return true;
}
bool PreparedLegacyRhythmRecipe::prepare_tracking(std::span<const float> hints,const dsp_kernels::FourierBackendFactory* factory) {
    CombPeriodPhaseConfig period;period.number_frames=config_.period_window;period.audio_frame_samples=config_.audio_hop;
    period.sample_rate=config_.sample_rate;period.minimum_bpm=config_.minimum_bpm;period.maximum_bpm=config_.maximum_bpm;
    if(!periods_.prepare(feature_count(),config_.evaluation_hop,period,hints,factory))return false;
    PeriodPhaseTickConfig tick;tick.evaluation_hop=config_.evaluation_hop;tick.audio_hop=config_.audio_hop;
    tick.sample_rate=config_.sample_rate;tick.maximum_candidate_frames=float(config_.period_window);
    return ticks_.prepare(feature_count(),limits_.maximum_pool_values,limits_.maximum_ticks,tick);
}
bool PreparedLegacyRhythmRecipe::prepare_results() {
    const auto rows=std::max(std::size_t{1},maximum_frames_/config_.evaluation_hop);
    if(!pool_.prepare(rows,limits_.maximum_pool_values,limits_.maximum_pool_values))return false;
    TickSequenceRefinementConfig refine;refine.frame_rate=config_.sample_rate/float(config_.audio_hop);
    refine.minimum_interval_seconds=config_.minimum_interval_seconds;refine.last_beat_interval_seconds=config_.last_beat_interval_seconds;
    LagTempoVoteConfig vote;vote.sample_rate=config_.sample_rate;vote.audio_hop=config_.audio_hop;
    vote.maximum_bpm=std::max(float(config_.maximum_bpm),60.f*config_.sample_rate/float(config_.audio_hop));
    if(!refinement_.prepare(limits_.maximum_ticks,refine) || !vote_.prepare(limits_.maximum_pool_values,vote))return false;
    candidates_.resize(limits_.maximum_ticks);return true;
}
bool PreparedLegacyRhythmRecipe::configure(LegacyRhythmRecipeLimits limits,LegacyRhythmRecipeConfig config,std::span<const float> hints,const dsp_kernels::FourierBackendFactory* factory) {
    const auto bound=std::size_t(std::numeric_limits<int>::max());
    if(!limits.maximum_pcm_samples || limits.maximum_pcm_samples>bound || !limits.maximum_ticks ||
       !limits.maximum_pool_values || config.audio_frame<2 || config.audio_frame>bound ||
       !config.audio_hop || config.audio_hop>bound || !config.evaluation_hop ||
       !std::isfinite(config.sample_rate) || config.sample_rate<=0 || (!config.use_onset && !config.use_bands))return false;
    config_=config;limits_=limits;
    return prepare_frames(factory) && prepare_features() && prepare_tracking(hints,factory) && prepare_results();
}
bool PreparedLegacyRhythmRecipe::prepare(LegacyRhythmRecipeLimits limits,LegacyRhythmRecipeConfig config,std::span<const float> hints,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t admitted=0;if(!storage_bytes(limits,config,hints,admitted,factory))return false;
    PreparedLegacyRhythmRecipe next;
    if(!next.configure(limits,config,hints,factory))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
void PreparedLegacyRhythmRecipe::reset(bool clear_matches) noexcept {
    status_=LegacyRhythmStatus::NotProcessed;candidate_count_=0;frame_ready_=false;
    silence_.reset();hfc_.reset();complex_onset_.reset();derivative_.reset();scaled_bands_.reset();
    periods_.reset();ticks_.reset(clear_matches);pool_.reset();
}
bool PreparedLegacyRhythmRecipe::load_frame(std::span<const float> pcm,std::size_t index) noexcept {
    frame_ready_=false;
    const auto start=std::ptrdiff_t(index*config_.audio_hop)-std::ptrdiff_t((config_.audio_frame+1)/2);
    const auto leading=start<0?std::size_t(-start):0,first=start<0?0:std::size_t(start);
    const auto acquired=first<pcm.size()?std::min(config_.audio_frame-leading,pcm.size()-first):0;
    return framing_.read(pcm,index,frame_) && silence_.process(frame_,leading,acquired) &&
        spectrum_.process_frame(frame_,0,complex_) &&
        dsp_kernels::cartesian_to_polar(complex_,magnitude_,config_.use_onset?std::span(phase_):std::span<float>{},
            dsp_kernels::ZeroMagnitudePhase::Atan2,dsp_kernels::ComplexMagnitudeNorm::SquaredNorm);
}
bool PreparedLegacyRhythmRecipe::onset_features() noexcept {
    float difference=0;
    if(!hfc_.process(magnitude_,phase_,features_[0]) || !derivative_.process(features_[0],difference) ||
       !complex_onset_.process(magnitude_,phase_,features_[2]))return false;
    features_[1]=std::max(0.f,difference);return true;
}
bool PreparedLegacyRhythmRecipe::band_features() noexcept {
    float cumulative=0;
    return bands_.process(magnitude_,band_power_) &&
        scaled_bands_.process(band_power_,std::span(features_).subspan(config_.use_onset?3:0),cumulative);
}
bool PreparedLegacyRhythmRecipe::features() noexcept {
    if(config_.use_onset && !onset_features())return false;
    return !config_.use_bands || band_features();
}
bool PreparedLegacyRhythmRecipe::track() noexcept {
    if(!periods_.push(features_)){status_=LegacyRhythmStatus::PeriodFailure;return false;}
    if(!ticks_.process(periods_.periods(),periods_.phases())){status_=LegacyRhythmStatus::TickFailure;return false;}
    const auto values=ticks_.ticks();
    if(values.size()>candidates_.size()-candidate_count_){status_=LegacyRhythmStatus::TickFailure;return false;}
    std::copy(values.begin(),values.end(),candidates_.begin()+std::ptrdiff_t(candidate_count_));
    candidate_count_+=values.size();
    if(!pool_.push(ticks_.matching_periods())){status_=LegacyRhythmStatus::PoolCapacity;return false;}
    return true;
}
bool PreparedLegacyRhythmRecipe::finish() noexcept {
    if(!silence_.finish()){status_=LegacyRhythmStatus::FeatureFailure;return false;}
    const auto bounds=silence_.bounds();
    if(!refinement_.process(std::span(candidates_).first(candidate_count_),bounds.frames,float(bounds.start),float(bounds.stop))){
        status_=LegacyRhythmStatus::RefinementFailure;return false;
    }
    if(!vote_.process(pool_.source(),pool_.flattened_size())){status_=LegacyRhythmStatus::VoteFailure;return false;}
    status_=LegacyRhythmStatus::Ready;return true;
}
bool PreparedLegacyRhythmRecipe::process(std::span<const float> pcm,FrameStageOutputPort<LegacyRhythmFrameStages> output) noexcept {
    reset();
    if(!maximum_frames_ || pcm.size()>limits_.maximum_pcm_samples ||
       !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);})){
        status_=LegacyRhythmStatus::InvalidInput;return false;
    }
    if(!process_frames(pcm,output))return false;
    if(!finish()){frame_ready_=false;return false;}
    return true;
}
bool PreparedLegacyRhythmRecipe::process_frames(std::span<const float> pcm,FrameStageOutputPort<LegacyRhythmFrameStages> output) noexcept {
    const auto frames=framing_.frames(pcm.size());
    for(std::size_t i=0;i<frames;++i){
        if(!load_frame(pcm,i) || !features()){status_=LegacyRhythmStatus::FeatureFailure;return false;}
        if(!track())return false;
        frame_index_=i;frame_ready_=true;output.publish(frame_stages());
    }
    return true;
}
LegacyRhythmFrameStages PreparedLegacyRhythmRecipe::frame_stages() const noexcept {
    if(!frame_ready_)return {};
    return {spectral_frame_stages(spectrum_,framing_.config(),frame_index_,config_.sample_rate,true,magnitude_,
                config_.use_onset?std::span<const float>(phase_):std::span<const float>{}),
        frame_,features_,band_power_,config_.use_bands?std::span<const float>(frequency_borders):std::span<const float>{},
        44100,512,config_.evaluation_hop,periods_.periods(),periods_.phases(),ticks_.ticks(),ticks_.matching_periods()};
}
std::span<const float> PreparedLegacyRhythmRecipe::ticks() const noexcept {
    return status_==LegacyRhythmStatus::Ready?refinement_.ticks():std::span<const float>{};
}
std::span<const float> PreparedLegacyRhythmRecipe::intervals() const noexcept {
    return status_==LegacyRhythmStatus::Ready?refinement_.intervals():std::span<const float>{};
}
std::span<const float> PreparedLegacyRhythmRecipe::bpm_estimates() const noexcept {
    return status_==LegacyRhythmStatus::Ready?vote_.estimates():std::span<const float>{};
}
}
