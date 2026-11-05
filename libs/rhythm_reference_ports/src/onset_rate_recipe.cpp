// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia OnsetRate at b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/onset_rate_recipe.h>
#include <rhythm_reference_ports/spectral_window.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace rhythm_reference_ports {
bool PreparedOnsetRateRecipe::prepare_spectrum(OnsetRateRecipe recipe,dsp_kernels::NumericalPrecision precision,const dsp_kernels::FourierBackendFactory* factory) {
    dsp_kernels::TemporalWindowConfig window;window.length=1024;window.hop=512;window.periodic=false;
    window.center=recipe==OnsetRateRecipe::Streaming;window.odd_center_ceil=true;
    window.tail=window.center?dsp_kernels::TemporalTail::CenterCoverEnd:dsp_kernels::TemporalTail::CoverEnd;
    std::vector<float> coefficients(window.length);
    SpectralWindowConfig shape;shape.precision=precision;
    if(!spectral_window(coefficients,shape) || !spectrum_.prepare(window,coefficients,0,true,factory))return false;
    complex_.resize(2*spectrum_.bins());magnitude_.resize(spectrum_.bins());
    phase_.resize(spectrum_.bins());distances_.resize(spectrum_.bins());return true;
}
bool PreparedOnsetRateRecipe::prepare_features(std::size_t frames) {
    dsp_kernels::SpectralDescriptorConfig hfc;hfc.method=dsp_kernels::SpectralDescriptor::HighFrequencyContent;
    hfc.bin_offset=0;hfc.frequency_step=22050.f/float(spectrum_.bins()-1);
    hfc.aggregation=dsp_kernels::SpectralAggregation::Sum;hfc.compensated_sum=false;
    if(!hfc_.prepare(spectrum_.bins(),hfc) || !complex_novelty_.prepare(spectrum_.bins()) ||
       !events_.prepare(2,frames))return false;
    hfc_curve_.resize(frames);complex_curve_.resize(frames);times_.resize(frames);decisions_.resize(frames);return true;
}
bool PreparedOnsetRateRecipe::configure(std::size_t maximum,OnsetRateRecipe recipe,dsp_kernels::NumericalPrecision precision,const dsp_kernels::FourierBackendFactory* factory) {
    if(!maximum || maximum>std::size_t(std::numeric_limits<int>::max()) ||
       (recipe!=OnsetRateRecipe::Standard && recipe!=OnsetRateRecipe::Streaming))return false;
    if(!prepare_spectrum(recipe,precision,factory) || !prepare_features(spectrum_.frames(maximum)))return false;
    maximum_pcm_=maximum;recipe_=recipe;return true;
}
bool PreparedOnsetRateRecipe::prepare(std::size_t maximum,OnsetRateRecipe recipe,dsp_kernels::NumericalPrecision precision,const dsp_kernels::FourierBackendFactory* factory) {
    PreparedOnsetRateRecipe next;std::size_t bytes=0;
    if(!storage_bytes(maximum,recipe,bytes,precision,factory) || !next.configure(maximum,recipe,precision,factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
void PreparedOnsetRateRecipe::reset() noexcept {
    ready_=false;frame_count_=0;onset_count_=0;rate_=0;duration_=0;
    hfc_.reset();complex_novelty_.reset();events_.reset();
}
bool PreparedOnsetRateRecipe::frame(std::span<const float> pcm,std::size_t index) noexcept {
    if(!spectrum_.process_frame(pcm,index,complex_) ||
       !dsp_kernels::cartesian_to_polar(complex_,magnitude_,phase_,dsp_kernels::ZeroMagnitudePhase::Atan2,dsp_kernels::ComplexMagnitudeNorm::SquaredNorm) ||
       !hfc_.process_values(magnitude_,hfc_curve_[index]) ||
       !complex_novelty_.process(magnitude_,phase_,distances_))return false;
    const float value=std::accumulate(distances_.begin(),distances_.end(),0.f);
    if(!std::isfinite(value))return false;
    complex_curve_[index]=value;return true;
}
bool PreparedOnsetRateRecipe::read_feature(const void* context,std::size_t feature,std::span<float> output) noexcept {
    const auto& self=*static_cast<const PreparedOnsetRateRecipe*>(context);
    if(feature>1 || output.size()!=self.frame_count_)return false;
    const auto& values=feature==0?self.hfc_curve_:self.complex_curve_;
    std::copy_n(values.begin(),output.size(),output.begin());return true;
}
bool PreparedOnsetRateRecipe::select(std::size_t samples) noexcept {
    constexpr std::array<float,2> weights{1,1};
    if(!events_.process({this,read_feature},2,frame_count_,weights,std::span(decisions_).first(frame_count_)) ||
       !events_.onset_times(times_,onset_count_))return false;
    duration_=recipe_==OnsetRateRecipe::Standard?float(samples)/44100.f:float(frame_count_*512)/44100.f;
    rate_=float(onset_count_)/duration_;
    ready_=std::isfinite(rate_) && std::isfinite(duration_);return ready_;
}
bool PreparedOnsetRateRecipe::process(std::span<const float> pcm) noexcept {
    reset();
    if(!maximum_pcm_ || pcm.empty() || pcm.size()>maximum_pcm_ ||
       !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    frame_count_=spectrum_.frames(pcm.size());
    for(std::size_t i=0;i<frame_count_;++i)if(!frame(pcm,i))return false;
    return select(pcm.size());
}
std::span<const float> PreparedOnsetRateRecipe::onset_times() const noexcept {
    return ready_?std::span<const float>(times_).first(onset_count_):std::span<const float>{};
}
std::span<const float> PreparedOnsetRateRecipe::hfc_curve() const noexcept {
    return ready_?std::span<const float>(hfc_curve_).first(frame_count_):std::span<const float>{};
}
std::span<const float> PreparedOnsetRateRecipe::complex_curve() const noexcept {
    return ready_?std::span<const float>(complex_curve_).first(frame_count_):std::span<const float>{};
}
}
