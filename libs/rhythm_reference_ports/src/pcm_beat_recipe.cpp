// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia BeatTrackerDegara/BeatTrackerMultiFeature at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/pcm_beat_recipe.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedPcmBeatRecipe::prepare_spectrum(std::size_t maximum,PcmBeatRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    dsp_kernels::TemporalWindowConfig window;window.length=2048;window.hop=1024;
    window.center=false;window.periodic=false;window.tail=dsp_kernels::TemporalTail::CoverEnd;
    std::vector<float> coefficients(window.length);
    SpectralWindowConfig shape;shape.precision=config.window_precision;
    if(!spectral_window(coefficients,shape) || !spectrum_.prepare(window,coefficients,0,true,factory))return false;
    return prepare_spectral_features(maximum,config);
}
bool PreparedPcmBeatRecipe::prepare_spectral_features(std::size_t maximum,PcmBeatRecipeConfig config) {
    const std::array<SpectralOnsetMethod,3> methods{SpectralOnsetMethod::Complex,SpectralOnsetMethod::Rms,SpectralOnsetMethod::MelFlux};
    const auto count=config.method==PcmBeatMethod::SingleComplex?1u:3u;
    for(std::size_t i=0;i<count;++i){
        if(!spectral_[i].prepare(spectrum_.bins(),{methods[i],44100}))return false;
        curves_[i].resize(spectrum_.frames(maximum));
    }
    complex_.resize(2*spectrum_.bins());magnitude_.resize(spectrum_.bins());phase_.resize(spectrum_.bins());return true;
}
bool PreparedPcmBeatRecipe::prepare_global(std::size_t maximum,PcmBeatRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    GlobalOnsetRecipeConfig emphasis;emphasis.window_precision=config.window_precision;emphasis.method=GlobalOnsetMethod::BeatEmphasis;
    GlobalOnsetRecipeConfig gain;gain.window_precision=config.window_precision;
    return emphasis_.prepare(maximum,emphasis,factory) && gain_.prepare(maximum,gain,factory);
}
bool PreparedPcmBeatRecipe::prepare_tracker(PcmBeatRecipeConfig config) {
    const auto frames=curves_[0].size();
    if(method_==PcmBeatMethod::SingleComplex){
        return prepare_single_tracker(frames,config);
    }
    MultiFeaturePeriodConfig tracker;tracker.minimum_bpm=float(config.minimum_bpm);
    tracker.maximum_bpm=float(config.maximum_bpm);tracker.noise=config.noise;
    const std::array<std::size_t,5> sizes{frames,frames,frames,emphasis_.frames(maximum_pcm_),gain_.frames(maximum_pcm_)};
    maximum_ticks_=std::max(2*(frames-1)+1,sizes[3]);
    return multi_.prepare(sizes,tracker);
}
bool PreparedPcmBeatRecipe::prepare_single_tracker(std::size_t frames,PcmBeatRecipeConfig config) {
    PeriodBeatTrackingConfig tracker;tracker.frame_rate=44100.f/1024.f;tracker.upsampling=2;
    tracker.min_bpm=float(config.minimum_bpm);tracker.max_bpm=float(config.maximum_bpm);tracker.noise=config.noise;
    if(!single_.prepare(frames,tracker))return false;
    maximum_ticks_=single_.output_samples(frames);mask_.resize(maximum_ticks_);ticks_.resize(maximum_ticks_);return true;
}
bool PreparedPcmBeatRecipe::configure(std::size_t maximum,PcmBeatRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    if(!maximum || maximum>std::size_t(std::numeric_limits<int>::max()) ||
       unsigned(config.method)>unsigned(PcmBeatMethod::MultiFeature) ||
       config.minimum_bpm<40 || config.minimum_bpm>180 || config.maximum_bpm<60 ||
       config.maximum_bpm>250 || config.maximum_bpm<config.minimum_bpm+20)return false;
    maximum_pcm_=maximum;method_=config.method;
    if(!prepare_spectrum(maximum,config,factory))return false;
    if(method_==PcmBeatMethod::MultiFeature && !prepare_global(maximum,config,factory))return false;
    return prepare_tracker(config);
}
bool PreparedPcmBeatRecipe::prepare(std::size_t maximum,PcmBeatRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t admitted=0;if(!storage_bytes(maximum,config,admitted,factory))return false;
    PreparedPcmBeatRecipe next;
    if(!next.configure(maximum,config,factory))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmBeatRecipe::spectral_frame(std::span<const float> pcm,std::size_t frame) noexcept {
    frame_ready_=false;
    if(!spectrum_.process_frame(pcm,frame,complex_) ||
       !dsp_kernels::cartesian_to_polar(complex_,magnitude_,phase_,dsp_kernels::ZeroMagnitudePhase::Atan2,
                                       dsp_kernels::ComplexMagnitudeNorm::SquaredNorm))return false;
    const auto count=method_==PcmBeatMethod::SingleComplex?1u:3u;
    for(std::size_t i=0;i<count;++i){
        if(!spectral_[i].process(magnitude_,phase_,curves_[i][frame]))return false;
        frame_onsets_[i]=curves_[i][frame];
    }
    frame_index_=frame;frame_ready_=true;
    return true;
}
bool PreparedPcmBeatRecipe::track_single() noexcept {
    const auto curve=std::span<const float>(curves_[0]).first(frame_count_);
    return single_.process(curve,std::span(mask_).first(single_.output_samples(frame_count_))) &&
        single_.beat_times(ticks_,tick_count_);
}
bool PreparedPcmBeatRecipe::track_multi(std::span<const float> pcm,PcmBeatStageOutputs outputs) noexcept {
    if(!emphasis_.process(pcm,outputs.emphasis) || !gain_.process(pcm,outputs.information_gain))return false;
    const std::array<OnsetCurveView,5> curves{
        OnsetCurveView{std::span(curves_[0]).first(frame_count_)},
        OnsetCurveView{std::span(curves_[1]).first(frame_count_)},
        OnsetCurveView{std::span(curves_[2]).first(frame_count_)},
        OnsetCurveView{emphasis_.curve()},OnsetCurveView{gain_.curve()}};
    return multi_.process(curves);
}
bool PreparedPcmBeatRecipe::process(std::span<const float> pcm,PcmBeatStageOutputs outputs) noexcept {
    ready_=false;frame_ready_=false;frame_count_=0;tick_count_=0;
    for(auto& onset:spectral_)onset.reset();
    if(!maximum_pcm_ || pcm.size()>maximum_pcm_ ||
       !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    if(!process_spectral_frames(pcm,outputs.spectral))return false;
    ready_=method_==PcmBeatMethod::SingleComplex?track_single():track_multi(pcm,outputs);
    if(!ready_)frame_ready_=false;
    return ready_;
}
bool PreparedPcmBeatRecipe::process_spectral_frames(std::span<const float> pcm,FrameStageOutputPort<PcmBeatFrameStages> output) noexcept {
    frame_count_=spectrum_.frames(pcm.size());
    for(std::size_t i=0;i<frame_count_;++i){
        if(!spectral_frame(pcm,i))return false;
        output.publish(frame_stages());
    }
    return true;
}
PcmBeatFrameStages PreparedPcmBeatRecipe::frame_stages() const noexcept {
    if(!frame_ready_)return {};
    const auto count=method_==PcmBeatMethod::SingleComplex?1u:3u;
    return {spectral_frame_stages(spectrum_,spectrum_.config(),frame_index_,44100,true,magnitude_,phase_),
        std::span<const float>(frame_onsets_).first(count)};
}
GlobalOnsetFrameStages PreparedPcmBeatRecipe::global_frame_stages(std::size_t method) const noexcept {
    if(!ready_ || method_!=PcmBeatMethod::MultiFeature || method<3 || method>4)return {};
    return method==3?emphasis_.frame_stages():gain_.frame_stages();
}
std::span<const float> PreparedPcmBeatRecipe::ticks() const noexcept {
    if(!ready_)return {};
    return method_==PcmBeatMethod::SingleComplex?std::span<const float>(ticks_).first(tick_count_):multi_.ticks();
}
std::span<const float> PreparedPcmBeatRecipe::onset_curve(std::size_t method) const noexcept {
    if(!ready_ || method>4 || (method_==PcmBeatMethod::SingleComplex && method))return {};
    if(method<3)return std::span<const float>(curves_[method]).first(frame_count_);
    return method==3?emphasis_.curve():gain_.curve();
}
std::span<const float> PreparedPcmBeatRecipe::candidate_times(std::size_t method) const noexcept {
    if(!ready_)return {};
    if(method_==PcmBeatMethod::SingleComplex)return method?std::span<const float>{}:ticks();
    return multi_.candidate_times(method);
}
std::span<const float> PreparedPcmBeatRecipe::method_scores() const noexcept {
    return ready_ && method_==PcmBeatMethod::MultiFeature?multi_.method_scores():std::span<const float>{};
}
float PreparedPcmBeatRecipe::confidence() const noexcept {
    return ready_ && method_==PcmBeatMethod::MultiFeature?multi_.confidence():0;
}
}
