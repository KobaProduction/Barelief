// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia SuperFluxExtractor at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/trajectory_onset_recipe.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <dsp_kernels/spectral_descriptors.h>
#include <cmath>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedTrajectoryOnsetRecipe::prepare_spectrum(TrajectoryOnsetRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    if(config.frame_size<2 || !config.audio_hop || !std::isfinite(config.sample_rate) || config.sample_rate<=0)return false;
    dsp_kernels::TemporalWindowConfig window;window.length=config.frame_size;window.hop=config.audio_hop;
    window.center=true;window.periodic=false;window.odd_center_ceil=true;window.tail=dsp_kernels::TemporalTail::CenterCoverEnd;
    std::vector<float> coefficients(config.frame_size);
    SpectralWindowConfig shape;shape.precision=config.window_precision;
    if(!spectral_window(coefficients,shape) || !spectrum_.prepare(window,coefficients,0,true,factory))return false;
    complex_.resize(2*spectrum_.bins());magnitude_.resize(spectrum_.bins());return true;
}
bool PreparedTrajectoryOnsetRecipe::prepare_features(TrajectoryOnsetRecipeConfig config) {
    TriangularSpectralBandConfig bands;bands.sample_rate=config.band_sample_rate;bands.logarithm=false;
    if(!bands_.prepare(spectrum_.bins(),trajectory_band_edges(),bands) ||
       !novelty_.prepare(bands_.bands(),3,{8,2}))return false;
    history_.resize(bands_.bands());
    for(auto& history:history_)if(!history.prepare(3))return false;
    band_values_.resize(bands_.bands());delayed_.resize(bands_.bands());return true;
}
bool PreparedTrajectoryOnsetRecipe::prepare_peaks(std::size_t capacity,TrajectoryOnsetRecipeConfig config) {
    ContrastPeakPickerConfig peaks;peaks.frame_rate=config.sample_rate/float(config.audio_hop);
    peaks.threshold=config.threshold;peaks.ratio_threshold=config.ratio_threshold;peaks.combine_ms=config.combine_ms;
    if(!config.novelty_chunk_size || !peaks_.prepare(config.novelty_chunk_size,capacity,peaks))return false;
    configured_chunk_=config.novelty_chunk_size;acquisition_size_=configured_chunk_;
    chunk_.resize(configured_chunk_);onsets_.resize(capacity);return true;
}
bool PreparedTrajectoryOnsetRecipe::configure(std::size_t maximum,std::size_t capacity,TrajectoryOnsetRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    if(!maximum || !capacity || !prepare_spectrum(config,factory) || !prepare_features(config) || !prepare_peaks(capacity,config))return false;
    if(!spectrum_.frames(maximum))return false;
    maximum_pcm_=maximum;config_=config;return true;
}
bool PreparedTrajectoryOnsetRecipe::prepare(std::size_t maximum,std::size_t capacity,TrajectoryOnsetRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    PreparedTrajectoryOnsetRecipe next;std::size_t bytes=0;
    if(!storage_bytes(maximum,capacity,config,bytes,factory) || !next.configure(maximum,capacity,config,factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
void PreparedTrajectoryOnsetRecipe::reset(bool clear_flux,bool restore_chunk) noexcept {
    onset_count_=0;band_frames_=0;pending_=0;frame_ready_=false;novelty_available_=false;
    for(auto& history:history_)history.clear();
    peaks_.reset();novelty_.reset(clear_flux,BufferedMaximumReset::ConfiguredStart);
    if(restore_chunk)acquisition_size_=configured_chunk_;
}
bool PreparedTrajectoryOnsetRecipe::flush(bool final) noexcept {
    if(!pending_)return true;
    if(!peaks_.consume(std::span(chunk_).first(pending_)))return false;
    if(final)acquisition_size_=pending_;
    pending_=0;return true;
}
bool PreparedTrajectoryOnsetRecipe::delay_bands() noexcept {
    for(std::size_t band=0;band<history_.size();++band)history_[band].push(band_values_[band]);
    if(++band_frames_<3)return false;
    for(std::size_t band=0;band<history_.size();++band)delayed_[band]=history_[band].values()[0];
    return true;
}
bool PreparedTrajectoryOnsetRecipe::novelty_frame() noexcept {
    novelty_available_=false;
    if(!delay_bands())return true;
    float value=0;
    if(!novelty_.process_pair(band_values_,delayed_,value))return false;
    frame_novelty_=value;novelty_available_=true;
    chunk_[pending_++]=value;
    return pending_!=acquisition_size_ || flush(false);
}
bool PreparedTrajectoryOnsetRecipe::frame(std::span<const float> pcm,std::size_t index) noexcept {
    frame_ready_=false;
    if(!(spectrum_.process_frame(pcm,index,complex_) &&
        dsp_kernels::cartesian_to_polar(complex_,magnitude_,{},dsp_kernels::ZeroMagnitudePhase::Atan2,
                                      dsp_kernels::ComplexMagnitudeNorm::SquaredNorm) &&
        bands_.process(magnitude_,band_values_) && novelty_frame()))return false;
    frame_index_=index;frame_ready_=true;return true;
}
bool PreparedTrajectoryOnsetRecipe::process(std::span<const float> pcm,FrameStageOutputPort<TrajectoryOnsetFrameStages> output) noexcept {
    reset();
    if(!maximum_pcm_ || pcm.size()>maximum_pcm_)return false;
    const auto count=spectrum_.frames(pcm.size());
    for(std::size_t i=0;i<count;++i){
        if(!frame(pcm,i))return false;
        output.publish(frame_stages());
    }
    if(!flush(true) || !peaks_.finish(onsets_,onset_count_)){onset_count_=0;frame_ready_=false;return false;}
    return true;
}
TrajectoryOnsetFrameStages PreparedTrajectoryOnsetRecipe::frame_stages() const noexcept {
    if(!frame_ready_)return {};
    return {spectral_frame_stages(spectrum_,spectrum_.config(),frame_index_,config_.sample_rate,true,magnitude_),
        band_values_,novelty_available_?std::span<const float>(delayed_):std::span<const float>{},
        bands_.centers_hz(),config_.band_sample_rate,novelty_available_,
        novelty_available_?frame_index_-2:0,novelty_available_?frame_novelty_:0};
}
}
