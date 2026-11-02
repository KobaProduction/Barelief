// Adapted from madmom features/onsets.py complex_flux and audio/stft.py
// local_group_delay at revision27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/complex_flux.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
namespace dsp_kernels {
namespace {
bool valid_temporal_filter(ComplexFluxConfig c) noexcept {
    if(!c.temporal_maximum)return true; // Source ignores origin when filtering is disabled.
    const auto width=c.temporal_maximum;
    return width<=std::size_t(std::numeric_limits<std::ptrdiff_t>::max())/2 &&
        c.temporal_origin>=-std::ptrdiff_t(width/2) && c.temporal_origin<=std::ptrdiff_t((width-1)/2);
}
bool valid_supports(std::span<const FilterBandSupport> supports,std::size_t bands,std::size_t bins) noexcept {
    if(supports.empty())return bands==bins;
    if(supports.size()!=bands)return false;
    return std::all_of(supports.begin(),supports.end(),[bins](const auto& support){
        return support.count && support.first<bins && support.count<=bins-support.first;
    });
}
}
bool PreparedComplexFlux::prepare(std::size_t frames,std::size_t bands,std::size_t bins,std::span<const FilterBandSupport> supports,ComplexFluxConfig c) {
    PreparedComplexFlux next;if(!next.configure(frames,bands,bins,supports,c))return false;
    *this=std::move(next);return true;
}
bool PreparedComplexFlux::configure(std::size_t frames,std::size_t bands,std::size_t bins,std::span<const FilterBandSupport> supports,ComplexFluxConfig c) {
    if(!frames || !bands || !bins || frames>std::size_t(std::numeric_limits<std::ptrdiff_t>::max())/2 || !c.lag)return false;
    if(!std::isfinite(c.phase_step) || !valid_temporal_filter(c) || !valid_supports(supports,bands,bins))return false;
    config_=c;maximum_frames_=frames;bands_=bands;phase_bins_=bins;supports_.assign(supports.begin(),supports.end());
    OnsetStrengthConfig difference;difference.lag=c.lag;difference.max_size=std::max(std::size_t{1},c.frequency_maximum);
    difference.center=false;difference.aggregate=OnsetAggregate::Sum;
    if(!difference_.prepare(bands,frames,difference))return false;
    allocate();return true;
}
void PreparedComplexFlux::allocate() {
    current_.resize(bands_);previous_.resize(bands_);difference_values_.resize(bands_);
    phase_.resize(phase_bins_);delay_.resize(phase_bins_);maximum_.resize(phase_bins_);
}
std::size_t PreparedComplexFlux::reflected(std::ptrdiff_t index,std::size_t count) const noexcept {
    const auto period=2*std::ptrdiff_t(count);auto folded=index%period;if(folded<0)folded+=period;
    return std::size_t(folded<std::ptrdiff_t(count)?folded:period-1-folded);
}
bool PreparedComplexFlux::load(FloatMatrixSource source,std::size_t frame,std::span<float> values) noexcept {
    return source.read && source.read(source.context,frame,values) &&
        std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);});
}
bool PreparedComplexFlux::group_delay(FloatMatrixSource phase,std::size_t frame) noexcept {
    if(!load(phase,frame,phase_))return false;
    for(std::size_t bin=0;bin+1<phase_bins_;++bin)
        delay_[bin]=std::abs(std::remainder(phase_[bin]-phase_[bin+1]-config_.phase_step,2.f*std::numbers::pi_v<float>))/std::numbers::pi_v<float>;
    delay_.back()=0;return true;
}
bool PreparedComplexFlux::phase_mask(FloatMatrixSource phase,std::size_t frames,std::size_t frame) noexcept {
    std::fill(maximum_.begin(),maximum_.end(),0.f);
    const auto width=std::max(std::size_t{1},config_.temporal_maximum);
    const auto start=std::ptrdiff_t(frame)-std::ptrdiff_t(width/2)-(config_.temporal_maximum?config_.temporal_origin:0);
    for(std::size_t i=0;i<width;++i){
        if(!group_delay(phase,reflected(start+std::ptrdiff_t(i),frames)))return false;
        for(std::size_t bin=0;bin<phase_bins_;++bin)maximum_[bin]=std::max(maximum_[bin],delay_[bin]);
    }
    return true;
}
float PreparedComplexFlux::band_mask(std::size_t band) const noexcept {
    if(supports_.empty()){
        float result=1;
        for(std::ptrdiff_t offset=-1;offset<=1;++offset)result=std::min(result,maximum_[reflected(std::ptrdiff_t(band)+offset,phase_bins_)]);
        return result;
    }
    const auto& support=supports_[band];const auto first=support.first?support.first-1:0;
    const auto last=support.first+support.count+std::size_t(support.first+support.count<phase_bins_);
    return *std::min_element(maximum_.begin()+std::ptrdiff_t(first),maximum_.begin()+std::ptrdiff_t(last));
}
bool PreparedComplexFlux::process_frame(FloatMatrixSource magnitude,FloatMatrixSource phase,std::size_t frames,std::size_t frame,float& output) noexcept {
    if(!maximum_frames_ || !frames || frames>maximum_frames_ || frame>=frames)return false;
    if(frame<config_.lag){output=0;return true;}
    if(!load(magnitude,frame,current_) || !load(magnitude,frame-config_.lag,previous_))return false;
    if(!difference_.positive_differences(current_,previous_,difference_values_) || !phase_mask(phase,frames,frame))return false;
    float result=0,error=0;
    for(std::size_t band=0;band<bands_;++band){
        const float value=difference_values_[band]*band_mask(band);
        if(!config_.compensated_sum){result+=value;continue;}
        const float corrected=value-error,next=result+corrected;
        error=(next-result)-corrected;result=next;
    }
    if(!std::isfinite(result))return false;
    output=result;return true;
}
}
