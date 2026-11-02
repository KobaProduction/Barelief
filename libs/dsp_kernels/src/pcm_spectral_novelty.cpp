// madmom features/onsets.py SpectralOnsetProcessor composition,
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/pcm_spectral_novelty.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace dsp_kernels {
bool PreparedPcmSpectralNovelty::prepare(std::size_t maximum,PcmSpectralNoveltyConfig config,
                                        std::span<const float> window,ExplicitSpectralFilters filters) {
    std::size_t bound=0;if(!storage_bytes(maximum,config,window,filters,bound))return false;
    PreparedPcmSpectralNovelty next;
    if(spectral_novelty_uses_phase(config.novelty.method))config.spectrum.circular_shift=true;
    if(!next.spectrum_.prepare(maximum,config.spectrum,window,filters))return false;
    auto novelty=config.novelty;
    if(config.automatic_difference_lag && spectral_novelty_uses_overlap_lag(novelty.method) &&
        !spectral_difference_lag(next.spectrum_.window_coefficients(),float(next.spectrum_.hop_samples()),config.difference_overlap,novelty.lag))return false;
    if(!next.novelty_.prepare(std::max(std::size_t{1},next.frames(maximum)),next.spectrum_.features(),
        next.spectrum_.phase_bins(),novelty,next.spectrum_.supports()))return false;
    next.maximum_samples_=maximum;if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmSpectralNovelty::read_features(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& input=*static_cast<const Input*>(context);
    return input.self->spectrum_.process_frame(input.pcm,frame,output);
}
bool PreparedPcmSpectralNovelty::read_phase(const void* context,std::size_t frame,std::span<float> output) noexcept {
    const auto& input=*static_cast<const Input*>(context);
    return input.self->spectrum_.process_frame(input.pcm,frame,{},output);
}
bool PreparedPcmSpectralNovelty::process(std::span<const float> pcm,FloatMatrixDestination output) noexcept {
    spectrum_.invalidate_values();novelty_.reset();
    if(!maximum_samples_ || pcm.size()>maximum_samples_ ||
        !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    Input input{this,pcm};
    return novelty_.process({&input,read_features},{&input,read_phase},frames(pcm.size()),output);
}
}
