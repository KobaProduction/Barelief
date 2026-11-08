// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia TempoTapDegara's Davies period path at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// AGPL version3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/period_path.h>
#include <rhythm_reference_ports/periodic_comb.h>
#include <dsp_kernels/vector_normalization.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <numeric>
#include <utility>
namespace rhythm_reference_ports {
namespace {
bool normalize(std::span<float> values) noexcept {
    return dsp_kernels::normalize_vector(values,dsp_kernels::VectorNormalization::Sum);
}
bool nonnegative(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x) && x>=0;});
}
bool geometry(PeriodPathConfig& c) noexcept {
    if(!std::isfinite(c.frame_rate) || c.frame_rate<1 || !std::isfinite(c.frame_duration_s) || c.frame_duration_s<=0)return false;
    if(!std::isfinite(c.min_bpm) || !std::isfinite(c.max_bpm) || c.min_bpm<=0 || c.max_bpm<c.min_bpm+20)return false;
    if(!std::isfinite(c.noise_amount) || c.noise_amount<0)return false;
    const float length=c.frame_duration_s*c.frame_rate,mean=std::round(c.frame_rate*.5f);
    if(!std::isfinite(length) || length<16 || length>=float(UINT32_MAX) || !std::isfinite(mean*mean))return false;
    if(!c.frame_length)c.frame_length=std::size_t(std::round(length));
    if(!c.hop)c.hop=c.frame_length/4;
    return c.frame_length>=16 && c.frame_length<std::size_t(PTRDIFF_MAX)/8 &&
        c.hop>=2 && c.hop<UINT32_MAX && c.hop<=SIZE_MAX/c.hop;
}
float gaussian(float index,float sigma,float step,float scale=1) noexcept {
    const float distance=index*step;
    const float probability=scale*std::exp(-distance*distance/(2.f*sigma*sigma))/(sigma*std::sqrt(2.f*std::numbers::pi_v<float>));
    return probability<1e-12f?0:probability;
}
}
bool PreparedPeriodPath::prepare(std::size_t frames,PeriodPathConfig config) {
    std::size_t admitted=0;if(!storage_bytes(frames,config,admitted))return false;
    PreparedPeriodPath next;if(!next.configure(frames,config))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedPeriodPath::configure(std::size_t frames,PeriodPathConfig c) {
    if(!frames || !geometry(c) || frames>SIZE_MAX/c.hop)return false;
    config_=c;maximum_frames_=frames;periods_=c.hop;
    if((c.frame_length-3)/4-1>=periods_)return false;
    allocate();if(!prior())return false;transitions();return true;
}
void PreparedPeriodPath::allocate() {
    filtered_.resize(periods_);
    weights_.resize(periods_);transition_.resize(periods_*periods_);observations_.resize(maximum_frames_*periods_);
    previous_.resize(periods_);current_.resize(periods_);predecessor_.resize(maximum_frames_*periods_);path_.resize(maximum_frames_);
}
bool PreparedPeriodPath::prior() noexcept {
    const float mean=std::round(60.f*config_.frame_rate/120.f);
    return dsp_kernels::rayleigh_weights(weights_,mean) && normalize(weights_);
}
void PreparedPeriodPath::transitions() noexcept {
    const float scale=config_.frame_rate/(44100.f/512.f),sigma=8.f*scale;
    const float low=std::floor(28.f*scale)-1.f,high=std::ceil(108.f*scale)-1.f;
    if(low>float(periods_-1) || high<0)return;
    const auto radius=std::ptrdiff_t(std::min(std::ceil(4.f*sigma),float(periods_)));
    const auto first=std::ptrdiff_t(std::max(0.f,low));
    const auto last=std::ptrdiff_t(std::min(float(periods_-1),high));
    for(auto to=first;to<=last;++to)
        for(auto from=std::max(first,to-radius);from<=std::min(last,to+radius);++from)
            transition_[std::size_t(to)*periods_+std::size_t(from)]=gaussian(float(from-to),sigma,1);
}
bool PreparedPeriodPath::observations(dsp_kernels::FloatMatrixSource source,std::size_t frames) noexcept {
    observations_max_=0;
    for(std::size_t frame=0;frame<frames;++frame){
        if(!source.read(source.context,frame,filtered_) || !nonnegative(filtered_))return false;
        observations_max_=std::max(observations_max_,*std::max_element(filtered_.begin(),filtered_.end()));
        std::copy(filtered_.begin(),filtered_.end(),observations_.begin()+std::ptrdiff_t(frame*periods_));
    }
    return true;
}
bool PreparedPeriodPath::noise(std::size_t frames) noexcept {
    for(std::size_t i=0;i<frames*periods_;++i){
        const float uniform=config_.noise.next?config_.noise.next(config_.noise.context):float(std::rand())/float(RAND_MAX);
        if(!std::isfinite(uniform) || uniform<0 || uniform>1)return false;
        observations_[i]+=config_.noise_amount*observations_max_*uniform;
    }
    return true;
}
float PreparedPeriodPath::transition_maximum(std::size_t to,std::uint32_t& best) const noexcept {
    float maximum=-1;best=0;
    for(std::size_t from=0;from<periods_;++from){
        const float value=previous_[from]*transition_[to*periods_+from];
        if(value>maximum){maximum=value;best=std::uint32_t(from);}
    }
    return maximum;
}
void PreparedPeriodPath::backtrack(std::size_t frames) noexcept {
    auto state=std::uint32_t(std::max_element(previous_.begin(),previous_.end())-previous_.begin());
    for(std::size_t frame=frames;frame-->0;){path_[frame]=state;if(frame)state=predecessor_[frame*periods_+state];}
}

bool PreparedPeriodPath::advance(std::size_t frame) noexcept {
    for(std::size_t to=0;to<periods_;++to){
        std::uint32_t best=0;const float maximum=transition_maximum(to,best);
        current_[to]=maximum*observations_[frame*periods_+to];predecessor_[frame*periods_+to]=best;
    }
    if(!normalize(current_))return false;
    previous_.swap(current_);return true;
}
bool PreparedPeriodPath::decode(std::size_t frames) noexcept {
    for(std::size_t period=0;period<periods_;++period)previous_[period]=weights_[period]*observations_[period];
    if(!normalize(previous_))return false;
    for(std::size_t frame=1;frame<frames;++frame)if(!advance(frame))return false;
    backtrack(frames);return true;
}
bool PreparedPeriodPath::process(dsp_kernels::FloatMatrixSource source,std::size_t frames,std::span<float> periods,std::span<float> ends) noexcept {
    if(!maximum_frames_ || !source.read || !frames || frames>maximum_frames_ || periods.size()!=frames || ends.size()!=frames)return false;
    if(!observations(source,frames) || !noise(frames) || !decode(frames))return false;
    for(std::size_t frame=0;frame<frames;++frame){
        periods[frame]=float(path_[frame]+1)/config_.frame_rate;
        ends[frame]=float(frame+1)*config_.frame_duration_s/4.f;
    }
    return true;
}
}
