// Adapted from madmom.audio.spectrogram._diff_frames at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception,
// Johannes Kepler University Linz and OFAI. BSD-style license,
// full notice: packaging/licenses/madmom.txt.
#include <dsp_kernels/onset_strength.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace dsp_kernels {
namespace {
std::size_t threshold_sample(std::span<const float> window,float ratio) noexcept {
    const float threshold=ratio * (*std::max_element(window.begin(),window.end()));
    for(std::size_t i=0;i<window.size();++i)if(window[i]>threshold)return i;
    return 0;
}
}
bool spectral_difference_lag(std::span<const float> window,float hop,float ratio,std::size_t& output) noexcept {
    if(window.empty() || !std::isfinite(hop) || hop<=0 || !std::isfinite(ratio))return false;
    if(!std::all_of(window.begin(),window.end(),[](float x){return std::isfinite(x);}))return false;
    const auto sample=threshold_sample(window,ratio);
    const float frames=(float(window.size())*.5f-float(sample))/hop;
    if(!std::isfinite(frames) || frames>=float(SIZE_MAX))return false;
    if(frames<=1){output=1;return true;}
    const float low=std::floor(frames),fraction=frames-low;
    const auto integer=std::size_t(low);
    output=integer+std::size_t(fraction>.5f || (fraction==.5f && integer%2));return true;
}
}
