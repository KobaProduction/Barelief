// Adapted from madmom tempo.py TCNTempoHistogramProcessor at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/tempo_probability_histogram.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dsp_kernels {
namespace {
std::size_t tempo_boundary(std::span<const float> values,float boundary,bool strict) noexcept {
    for(std::size_t i=0;i<values.size();++i)
        if(strict?float(i)>boundary:float(i)>=boundary)return i;
    return 0;
}
}
bool tempo_probability_range(std::span<const float> values,float minimum,float maximum,TempoProbabilityRange& output) noexcept {
    if(values.empty() || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum<=0 || maximum<minimum ||
       !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x) && x>=0 && x<=1;}))return false;
    const auto first=tempo_boundary(values,minimum,false),last=tempo_boundary(values,maximum,true);
    output={first,last>first?last-first:0};return true;
}
bool tempo_probability_coordinates(TempoProbabilityRange range,std::span<float> output) noexcept {
    if(output.size()!=range.count || range.count>std::numeric_limits<std::size_t>::max()-range.first)return false;
    for(std::size_t i=0;i<output.size();++i)output[i]=float(range.first+i);
    return true;
}
}
