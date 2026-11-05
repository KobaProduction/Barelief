// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia OnsetDetectionGlobal at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/global_onset_recipe.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
bool valid_capacity(std::size_t maximum,GlobalOnsetRecipeConfig config) noexcept {
    const auto limit=std::size_t(std::numeric_limits<int>::max());
    return maximum && maximum<=limit && config.frame_size>=2 && config.frame_size<=limit &&
       config.hop && config.hop<=limit && std::isfinite(config.sample_rate) && config.sample_rate>0 &&
       unsigned(config.method)<=unsigned(GlobalOnsetMethod::BeatEmphasis);
}
}
bool PreparedGlobalOnsetRecipe::prepare_spectrum(GlobalOnsetRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    dsp_kernels::TemporalWindowConfig window;window.length=config.frame_size;window.hop=config.hop;
    window.center=false;window.periodic=false;window.tail=dsp_kernels::TemporalTail::CoverEnd;
    std::vector<float> coefficients(config.frame_size);
    SpectralWindowConfig shape;shape.precision=config.window_precision;
    if(!spectral_window(coefficients,shape) || !spectrum_.prepare(window,coefficients,0,true,factory))return false;
    complex_.resize(2*spectrum_.bins());magnitude_.resize(spectrum_.bins());
    phase_.resize(spectrum_.bins());distances_.resize(spectrum_.bins());return true;
}
bool PreparedGlobalOnsetRecipe::prepare_gain(GlobalOnsetRecipeConfig config) {
    const float first=std::round(40.f*float(config.frame_size)/config.sample_rate);
    const float stop=std::round(5000.f*float(config.frame_size)/config.sample_rate)+1.f;
    if(!std::isfinite(first) || !std::isfinite(stop) || first<0 || stop<=first || stop>float(spectrum_.bins()))return false;
    first_bin_=std::size_t(first);stop_bin_=std::size_t(stop);
    return gain_.prepare(spectrum_.bins(),first_bin_,stop_bin_);
}
bool PreparedGlobalOnsetRecipe::prepare_emphasis(std::size_t frames,GlobalOnsetRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    if(!std::isfinite(config.band_sample_rate) || config.band_sample_rate<=0 ||
       config.sample_rate>config.band_sample_rate)return false;
    // The source forwards highFrequencyBound, but omits the ERBBands sampleRate.
    ErbFilterBankConfig erb;erb.sample_rate=config.band_sample_rate;erb.high_hz=config.sample_rate/2.f;
    if(!novelty_.prepare(spectrum_.bins(),PolarNovelty::BeatEmphasis) || !erb_.prepare(spectrum_.bins(),erb) ||
       !emphasis_.prepare(frames,{config.sample_rate/float(config.hop),40},factory))return false;
    erb_values_.resize(erb.bands);return true;
}
bool PreparedGlobalOnsetRecipe::configure(std::size_t maximum,GlobalOnsetRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    if(!valid_capacity(maximum,config))return false;
    if(!prepare_spectrum(config,factory))return false;
    const auto frames=spectrum_.frames(maximum);
    if(config.method==GlobalOnsetMethod::InformationGain){
        if(!prepare_gain(config))return false;
    }else if(!prepare_emphasis(frames,config,factory))return false;
    maximum_pcm_=maximum;config_=config;method_=config.method;curve_.resize(frames);return true;
}
bool PreparedGlobalOnsetRecipe::prepare(std::size_t maximum,GlobalOnsetRecipeConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t admitted=0;if(!storage_bytes(maximum,config,admitted,factory))return false;
    PreparedGlobalOnsetRecipe next;
    if(!next.configure(maximum,config,factory))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedGlobalOnsetRecipe::spectrum(std::span<const float> pcm,std::size_t frame) noexcept {
    frame_ready_=false;
    return spectrum_.process_frame(pcm,frame,complex_) &&
        dsp_kernels::cartesian_to_polar(complex_,magnitude_,phase_,dsp_kernels::ZeroMagnitudePhase::Atan2,
                                       dsp_kernels::ComplexMagnitudeNorm::SquaredNorm);
}
bool PreparedGlobalOnsetRecipe::read_erb(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& input=*static_cast<const BorrowedPcm*>(context);
    return input.self->erb_frame(input.pcm,frame,output,input.output);
}
bool PreparedGlobalOnsetRecipe::erb_frame(std::span<const float> pcm,std::size_t frame,std::span<float> output,
    FrameStageOutputPort<GlobalOnsetFrameStages> port) noexcept {
    if(output.size()!=erb_values_.size() || !spectrum(pcm,frame) || !novelty_.process(magnitude_,phase_,distances_) ||
       !erb_.process(distances_,erb_values_))return false;
    std::copy(erb_values_.begin(),erb_values_.end(),output.begin());
    frame_index_=frame;frame_ready_=true;port.publish(frame_stages());return true;
}
bool PreparedGlobalOnsetRecipe::information_gain(std::span<const float> pcm,std::size_t frames,
    FrameStageOutputPort<GlobalOnsetFrameStages> output) noexcept {
    for(std::size_t i=0;i<frames;++i){
        if(!spectrum(pcm,i) || !gain_.process(magnitude_,curve_[i]))return false;
        frame_index_=i;frame_ready_=true;output.publish(frame_stages());
    }
    return true;
}
bool PreparedGlobalOnsetRecipe::process(std::span<const float> pcm,FrameStageOutputPort<GlobalOnsetFrameStages> output) noexcept {
    ready_=false;frame_ready_=false;count_=0;gain_.reset();novelty_.reset();
    if(!maximum_pcm_ || pcm.size()>maximum_pcm_ ||
       !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    const auto count=spectrum_.frames(pcm.size());
    const BorrowedPcm input{this,pcm,output};
    const bool ok=method_==GlobalOnsetMethod::InformationGain?information_gain(pcm,count,output):
        emphasis_.process({&input,read_erb},count,std::span(curve_).first(count));
    if(!ok){frame_ready_=false;return false;}
    count_=count;ready_=true;return true;
}
GlobalOnsetFrameStages PreparedGlobalOnsetRecipe::frame_stages() const noexcept {
    if(!frame_ready_)return {};
    GlobalOnsetFrameStages result;
    result.spectrum=spectral_frame_stages(spectrum_,spectrum_.config(),frame_index_,config_.sample_rate,true,magnitude_,phase_);
    if(method_==GlobalOnsetMethod::InformationGain){
        result.information_magnitude=std::span<const float>(magnitude_).subspan(first_bin_,stop_bin_-first_bin_);
        result.information_first_bin=first_bin_;
        result.information_gain=curve_[frame_index_];
    }else{
        result.distances=distances_;result.erb_values=erb_values_;result.erb_centers_hz=erb_.centers_hz();
        result.erb_sample_rate=config_.band_sample_rate;result.erb_high_hz=config_.sample_rate/2.f;
        result.erb_bin_step_hz=double(config_.band_sample_rate)/double(2*(spectrum_.bins()-1));
    }
    return result;
}
std::span<const float> PreparedGlobalOnsetRecipe::curve() const noexcept {
    return ready_?std::span<const float>(curve_).first(count_):std::span<const float>{};
}
}
