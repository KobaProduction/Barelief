// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia TempoTapDegara's Davies period path at
// MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// C++/Float32 decomposition Copyright (C) 2026 Koba Production.
// AGPL version3 or later; no warranty. See ../AGPL-COPYING.
#include <rhythm_reference_ports/period_path.h>
#include <rhythm_reference_ports/periodic_comb.h>
#include <dsp_kernels/correlation.h>
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
    const float total=std::accumulate(values.begin(),values.end(),0.f);
    if(!std::isfinite(total))return false;
    if(total!=0)for(auto& value:values)value/=total;
    return true;
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
std::size_t lag_bound(float bpm,float rate,std::size_t maximum,bool round_up) noexcept {
    const float value=60.f*rate/bpm;
    const float index=std::max(0.f,(round_up?std::ceil(value):std::floor(value))-1.f);
    return index>=float(maximum)?maximum:std::size_t(index);
}
float gaussian(float index,float sigma,float step,float scale=1) noexcept {
    const float distance=index*step;
    const float probability=scale*std::exp(-distance*distance/(2.f*sigma*sigma))/(sigma*std::sqrt(2.f*std::numbers::pi_v<float>));
    return probability<1e-12f?0:probability;
}
}
bool PreparedMovingThreshold::prepare(std::size_t maximum,std::size_t half) {
    std::size_t admitted=0;if(!storage_bytes(maximum,half,admitted))return false;
    if(!maximum || half>(SIZE_MAX-maximum)/2)return false;
    PreparedMovingThreshold next;next.maximum_values_=maximum;next.half_=half;next.values_.resize(maximum);
    if(!next.average_.prepare(2*half+1))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
float PreparedMovingThreshold::padded(std::span<const float> input,std::size_t frame) const noexcept {
    if(frame<half_)return input.front();
    frame-=half_;return frame<input.size()?input[frame]:input.back();
}
bool PreparedMovingThreshold::process(std::span<const float> input,std::span<float> output) noexcept {
    average_.reset();return process(input,output,average_);
}
bool PreparedMovingThreshold::process(std::span<const float> input,std::span<float> output,PreparedCausalAverage& average) noexcept {
    if(!maximum_values_ || !average.length() || input.size()>maximum_values_ || input.size()!=output.size() || !nonnegative(input))return false;
    if(input.empty())return true;
    for(std::size_t i=0;i<input.size()+2*half_;++i){
        float mean=0;if(!average.process(padded(input,i),mean))return false;
        if(i<2*half_)continue;
        const auto at=i-2*half_;values_[at]=std::max(0.f,input[at]-mean);
        if(!std::isfinite(values_[at]))return false;
    }
    std::copy_n(values_.begin(),input.size(),output.begin());return true;
}
bool PreparedPeriodPath::prepare(std::size_t frames,PeriodPathConfig config) {
    std::size_t admitted=0;if(!storage_bytes(frames,config,admitted))return false;
    PreparedPeriodPath next;if(!next.configure(frames,config))return false;
    if(next.workspace_bytes()>admitted)return false;
    *this=std::move(next);return true;
}
bool PreparedPeriodPath::configure(std::size_t frames,PeriodPathConfig c) {
    if(!frames || !geometry(c) || frames>SIZE_MAX/c.hop)return false;
    config_=c;maximum_frames_=frames;periods_=c.hop;min_period_=3;max_period_=(c.frame_length-3)/4-1;
    if(max_period_>=periods_ || !threshold_.prepare(periods_,8) || !prepare_average())return false;
    minimum_user_=lag_bound(c.max_bpm,c.frame_rate,periods_-1,false);
    maximum_user_=lag_bound(c.min_bpm,c.frame_rate,periods_-1,true);
    allocate();prior();transitions();return true;
}
bool PreparedPeriodPath::prepare_average() {
    const float half=std::floor(.1f*config_.frame_rate);
    if(!std::isfinite(half) || half<1 || half>=float((SIZE_MAX-1)/2))return false;
    return average_.prepare(2*std::size_t(half)+1);
}
void PreparedPeriodPath::allocate() {
    frame_.resize(config_.frame_length);acf_.resize(config_.frame_length);comb_.resize(periods_);filtered_.resize(periods_);
    weights_.resize(periods_);transition_.resize(periods_*periods_);observations_.resize(maximum_frames_*periods_);
    previous_.resize(periods_);current_.resize(periods_);predecessor_.resize(maximum_frames_*periods_);path_.resize(maximum_frames_);
}
void PreparedPeriodPath::prior() noexcept {
    const float mean=std::round(60.f*config_.frame_rate/120.f),variance=mean*mean;
    for(std::size_t i=0;i<periods_;++i){const float period=float(i+1);weights_[i]=period/variance*std::exp(-.5f*period*period/variance);}
    normalize(weights_);
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
bool PreparedPeriodPath::comb() noexcept {
    return harmonic_comb_response(acf_,weights_,min_period_,max_period_+1,4,comb_);
}
bool PreparedPeriodPath::observation(std::size_t frame,PreparedCausalAverage& average) noexcept {
    if(!comb() || !threshold_.process(comb_,filtered_,average))return false;
    std::fill(filtered_.begin(),filtered_.begin()+std::ptrdiff_t(minimum_user_+1),0.f);
    std::fill(filtered_.begin()+std::ptrdiff_t(maximum_user_+1),filtered_.end(),0.f);
    if(!normalize(filtered_))return false;
    observations_max_=std::max(observations_max_,*std::max_element(filtered_.begin(),filtered_.end()));
    std::copy(filtered_.begin(),filtered_.end(),observations_.begin()+std::ptrdiff_t(frame*periods_));return true;
}
bool PreparedPeriodPath::observations(dsp_kernels::FloatMatrixSource source,std::size_t frames,PreparedCausalAverage& average) noexcept {
    observations_max_=0;
    for(std::size_t frame=0;frame<frames;++frame){
        if(!source.read(source.context,frame,frame_) || !nonnegative(frame_))return false;
        if(!dsp_kernels::linear_autocorrelation(frame_,acf_,dsp_kernels::CorrelationNormalization::Unbiased) || !observation(frame,average))return false;
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
bool PreparedPeriodPath::advance(std::size_t frame) noexcept {
    for(std::size_t to=0;to<periods_;++to){
        float maximum=-1;std::uint32_t best=0;
        for(std::size_t from=0;from<periods_;++from){
            const float value=previous_[from]*transition_[to*periods_+from];
            if(value>maximum){maximum=value;best=std::uint32_t(from);}
        }
        current_[to]=maximum*observations_[frame*periods_+to];predecessor_[frame*periods_+to]=best;
    }
    if(!normalize(current_))return false;
    previous_.swap(current_);return true;
}
bool PreparedPeriodPath::decode(std::size_t frames) noexcept {
    for(std::size_t period=0;period<periods_;++period)previous_[period]=weights_[period]*observations_[period];
    if(!normalize(previous_))return false;
    for(std::size_t frame=1;frame<frames;++frame)if(!advance(frame))return false;
    auto state=std::uint32_t(std::max_element(previous_.begin(),previous_.end())-previous_.begin());
    for(std::size_t frame=frames;frame-->0;){path_[frame]=state;if(frame)state=predecessor_[frame*periods_+state];}
    return true;
}
bool PreparedPeriodPath::process(dsp_kernels::FloatMatrixSource source,std::size_t frames,std::span<float> periods,std::span<float> ends) noexcept {
    average_.reset();return process(source,frames,periods,ends,average_);
}
bool PreparedPeriodPath::process(dsp_kernels::FloatMatrixSource source,std::size_t frames,std::span<float> periods,std::span<float> ends,PreparedCausalAverage& average) noexcept {
    if(!maximum_frames_ || !source.read || !frames || frames>maximum_frames_ || periods.size()!=frames || ends.size()!=frames)return false;
    if(!observations(source,frames,average) || !noise(frames) || !decode(frames))return false;
    for(std::size_t frame=0;frame<frames;++frame){
        periods[frame]=float(path_[frame]+1)/config_.frame_rate;
        ends[frame]=float(frame+1)*config_.frame_duration_s/4.f;
    }
    return true;
}
}
