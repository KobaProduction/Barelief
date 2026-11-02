// Adapted from madmom audio/filters.py, revision27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/log_frequency_filter_bank.h>
#include <dsp_kernels/frequency_bins.h>
#include <dsp_kernels/discrete_triangle.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
namespace dsp_kernels {
namespace {
bool valid(const LogFrequencyFilterConfig& c) noexcept {
    return std::isfinite(c.bands_per_octave) && c.bands_per_octave>0 &&
        std::isfinite(c.fmin) && c.fmin>0 && std::isfinite(c.fmax) && c.fmax>c.fmin &&
        std::isfinite(c.reference) && c.reference>0;
}
bool ordered(std::span<const float> grid) noexcept {
    if(grid.empty() || grid.front()<0)return false;
    for(std::size_t i=0;i<grid.size();++i)
        if(!std::isfinite(grid[i]) || (i && grid[i]<=grid[i-1]))return false;
    return true;
}
}
bool LogFrequencyFilterBank::prepare(std::span<const float> grid,LogFrequencyFilterConfig config) {
    LogFrequencyFilterBank next;if(!next.configure(grid,config))return false;
    *this=std::move(next);return true;
}
std::size_t LogFrequencyFilterBank::nearest(std::span<const float> grid,double hz) noexcept {
    return nearest_frequency_bin(grid,hz);
}
bool LogFrequencyFilterBank::band_count(FrequencyGridView grid,const LogFrequencyFilterConfig& c,std::size_t& out) noexcept {
    if(!grid.size || !grid.at || !valid(c))return false;
    const double low=std::floor(std::log2(double(c.fmin)/c.reference)*c.bands_per_octave);
    const double high=std::ceil(std::log2(double(c.fmax)/c.reference)*c.bands_per_octave);
    if(!std::isfinite(low) || !std::isfinite(high) || high-low>double(SIZE_MAX/sizeof(float)) || (low<high && (low+1<=low || high-1>=high)))return false;
    std::size_t count=0,previous=SIZE_MAX;
    for(double i=low;i<high;++i){
        const auto hz=double(c.reference)*std::exp2(i/double(c.bands_per_octave));
        if(hz<c.fmin || hz>c.fmax)continue;
        const auto bin=nearest_frequency_bin(grid,hz);
        if(bin==SIZE_MAX)return false;
        if(!c.unique_filters || previous!=bin){++count;previous=bin;}
    }
    if(count<3)return false;
    out=count-2;return true;
}
bool LogFrequencyFilterBank::corners(std::span<const float> grid,const LogFrequencyFilterConfig& c) {
    const double low=std::floor(std::log2(double(c.fmin)/c.reference)*c.bands_per_octave);
    const double high=std::ceil(std::log2(double(c.fmax)/c.reference)*c.bands_per_octave);
    if(!std::isfinite(low) || !std::isfinite(high) || high-low>double(SIZE_MAX/sizeof(float)))return false;
    if(low<high && (low+1<=low || high-1>=high))return false;
    for(double i=low;i<high;++i){
        const double hz=double(c.reference)*std::exp2(i/double(c.bands_per_octave));
        if(hz<c.fmin || hz>c.fmax)continue;
        const auto bin=nearest(grid,hz);
        if(!c.unique_filters || corners_.empty() || corners_.back()!=bin)corners_.push_back(bin);
    }
    return corners_.size()>=3;
}
bool LogFrequencyFilterBank::triangle(std::span<const float> grid,std::size_t band,const LogFrequencyFilterConfig& c) {
    DiscreteTriangleGeometry geometry;
    if(!discrete_triangle_filter(corners_[band],corners_[band+1],corners_[band+2],c.normalize,
        std::span(weights_).subspan(band*bins_,bins_),geometry))return false;
    supports_.push_back(geometry.nonzero);centers_.push_back(grid[geometry.center]);return true;
}
bool LogFrequencyFilterBank::configure(std::span<const float> grid,LogFrequencyFilterConfig c) {
    if(!valid(c) || !ordered(grid) || !corners(grid,c))return false;
    bins_=grid.size();const auto count=corners_.size()-2;
    if(count>SIZE_MAX/bins_)return false;
    weights_.resize(count*bins_);result_.resize(count);
    for(std::size_t band=0;band<count;++band)if(!triangle(grid,band,c))return false;
    return true;
}
bool LogFrequencyFilterBank::process(std::span<const float> input,std::span<float> output) const noexcept {
    if(!bins_ || input.size()!=bins_ || output.size()!=bands())return false;
    if(!std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t band=0;band<bands();++band)
        if(!project_filter_weights(input,std::span(weights_).subspan(band*bins_,bins_),result_[band]))return false;
    std::copy(result_.begin(),result_.end(),output.begin());return true;
}
}
