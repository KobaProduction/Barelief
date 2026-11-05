// madmom audio/spectrogram.py SuperFluxProcessor composition,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <rhythm_reference_ports/pcm_spectral_difference.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
dsp_kernels::SpectralFrameRecipeConfig maximum_difference_spectrum_defaults() noexcept {
    dsp_kernels::SpectralFrameRecipeConfig c;c.filtering=dsp_kernels::SpectralFilterMode::Logarithmic;
    c.logarithmic_filters.bands_per_octave=24;c.logarithmic_filters.normalize=false;c.logarithm=true;return c;
}
bool PreparedPcmSpectralDifference::prepare(std::size_t maximum,PcmSpectralDifferenceConfig config,
    std::span<const float> window,dsp_kernels::ExplicitSpectralFilters filters) {
    PreparedPcmSpectralDifference next;
    std::size_t bytes=0;if(!storage_bytes(maximum,config,window,filters,bytes))return false;
    if(!next.spectrum_.prepare(maximum,config.spectrum,window,filters))return false;
    auto difference=config.difference;
    if(config.automatic_lag && !dsp_kernels::spectral_difference_lag(next.spectrum_.window_coefficients(),
        float(next.spectrum_.hop_samples()),config.overlap_ratio,difference.lag))return false;
    if(!next.differences_.prepare(std::max(std::size_t{1},next.spectrum_.frames(maximum)),next.spectrum_.features(),difference))return false;
    if(next.workspace_bytes()>bytes)return false;
    next.maximum_samples_=maximum;*this=std::move(next);return true;
}
bool PreparedPcmSpectralDifference::storage_bytes(std::size_t maximum,PcmSpectralDifferenceConfig config,
    std::span<const float> window,dsp_kernels::ExplicitSpectralFilters filters,std::size_t& output) noexcept {
    std::size_t bytes=0,frames=0,features=0,nested=0;
    if(!dsp_kernels::PreparedSpectralFrameRecipe::geometry(maximum,config.spectrum,filters,frames,features) ||
       !dsp_kernels::PreparedSpectralFrameRecipe::storage_bytes(maximum,config.spectrum,window,filters,bytes))return false;
    auto difference=config.difference;
    if(config.automatic_lag){
        const double hop=config.spectrum.frame_rate>0?double(config.spectrum.sample_rate)/config.spectrum.frame_rate:config.spectrum.framing.hop_samples;
        const double bound=std::ceil(double(float(config.spectrum.framing.length))/double(float(hop)))+2;
        if(!std::isfinite(config.overlap_ratio) || !std::isfinite(bound) || bound<1 || bound>=double(SIZE_MAX))return false;
        difference.lag=std::size_t(bound);
    }
    if(!PreparedSpectralDifferenceSequence::storage_bytes(std::max(std::size_t{1},frames),features,difference,nested) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,nested))return false;
    output=bytes;return true;
}
bool PreparedPcmSpectralDifference::read_features(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& input=*static_cast<const Input*>(context);
    return input.self->spectrum_.process_frame(input.pcm,frame,output);
}
bool PreparedPcmSpectralDifference::process(std::span<const float> pcm,dsp_kernels::FloatMatrixDestination output,
    bool reset_state,dsp_kernels::FloatMatrixDestination current) noexcept {
    if(!maximum_samples_ || pcm.size()>maximum_samples_ ||
        !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    Input input{this,pcm};
    return differences_.process({&input,read_features},spectrum_.frames(pcm.size()),output,reset_state,current);
}
}
