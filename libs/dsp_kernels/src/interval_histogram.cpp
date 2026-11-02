// Adapted from madmom features/tempo.py interval_histogram_acf/comb and
// ACFTempoHistogramProcessor/CombFilterTempoHistogramProcessor at revision
// 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/interval_histogram.h>
#include <dsp_kernels/correlation.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool interval_histogram_parameters(float rate,float low,float high,float seconds,IntervalHistogramConfig& output) noexcept {
    if(!std::isfinite(rate) || rate<=0 || !std::isfinite(low) || low<=0 || !std::isfinite(high) || high<low)return false;
    if(!std::isfinite(seconds) || seconds<=0)return false;
    const float first=std::floor(60.f*rate/high),last=std::ceil(60.f*rate/low),history=std::floor(rate*seconds);
    if(first<1 || !std::isfinite(last) || last>=float(SIZE_MAX) || history<1 || !std::isfinite(history) || history>=float(SIZE_MAX))return false;
    auto config=output;config.minimum_interval=std::size_t(first);config.maximum_interval=std::size_t(last);config.history_frames=std::size_t(history);
    output=config;return true;
}
bool PreparedIntervalHistogram::prepare(std::size_t frames,IntervalHistogramConfig config,std::span<const float> alpha) {
    std::size_t bound=0;if(!storage_bytes(frames,config,alpha,bound))return false;
    PreparedIntervalHistogram next;if(!next.configure(frames,config,alpha))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedIntervalHistogram::configure(std::size_t frames,IntervalHistogramConfig config,std::span<const float> alpha) {
    if(!frames || !config.minimum_interval || config.maximum_interval<config.minimum_interval || config.maximum_interval==SIZE_MAX || !config.history_frames)return false;
    if(config.method<IntervalHistogramMethod::Autocorrelation || config.method>IntervalHistogramMethod::Comb || !config.channels)return false;
    if(config.method==IntervalHistogramMethod::Autocorrelation && (config.channels!=1 || !alpha.empty()))return false;
    const auto count=config.maximum_interval-config.minimum_interval+1;
    if(config.channels>SIZE_MAX/count || config.history_frames>SIZE_MAX/(count*config.channels))return false;
    config_=config;maximum_frames_=frames;intervals_.resize(count);
    for(std::size_t i=0;i<count;++i)intervals_[i]=config.minimum_interval+i;
    if(config.method==IntervalHistogramMethod::Comb && !prepare_comb(alpha))return false;
    allocate();return true;
}
bool PreparedIntervalHistogram::prepare_comb(std::span<const float> alpha) {
    if(alpha.empty())alpha=std::span(&config_.alpha,1);
    return comb_.prepare(intervals_,alpha,CombDirection::Backward,config_.channels);
}
std::span<const float> PreparedIntervalHistogram::channel_bins(std::size_t channel) const noexcept {
    return ready_ && channel<config_.channels?std::span(bins_).subspan(channel*intervals_.size(),intervals_.size()):std::span<const float>{};
}
void PreparedIntervalHistogram::allocate() {
    bins_.resize(config_.channels*intervals_.size());response_.resize(bins_.size());
    activations_.resize(config_.maximum_interval+1);history_.resize(config_.history_frames*bins_.size());
    absolute_.resize(maximum_frames_);acf_.resize(config_.maximum_interval+1);
}
void PreparedIntervalHistogram::reset() noexcept {
    comb_.reset();std::fill(bins_.begin(),bins_.end(),0.f);std::fill(response_.begin(),response_.end(),0.f);
    std::fill(activations_.begin(),activations_.end(),0.f);std::fill(history_.begin(),history_.end(),0.f);
    activation_cursor_=0;histogram_cursor_=0;ready_=false;streaming_=false;
}
bool PreparedIntervalHistogram::validate(std::span<const float> input) const noexcept {
    return maximum_frames_ && input.size()%config_.channels==0 && input.size()/config_.channels<=maximum_frames_ &&
        std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);});
}
bool PreparedIntervalHistogram::offline_acf(std::span<const float> input) noexcept {
    std::transform(input.begin(),input.end(),absolute_.begin(),[](float x){return std::abs(x);});
    std::fill(acf_.begin(),acf_.end(),0.f);
    const auto count=std::min(input.size(),acf_.size());
    if(!linear_autocorrelation(std::span(absolute_).first(input.size()),std::span(acf_).first(count),CorrelationNormalization::Raw))return false;
    for(std::size_t i=0;i<intervals_.size();++i)bins_[i]=acf_[intervals_[i]];
    return true;
}
bool PreparedIntervalHistogram::retain_maxima() noexcept {
    for(std::size_t channel=0;channel<config_.channels;++channel){
        auto values=std::span(response_).subspan(channel*intervals_.size(),intervals_.size());
        const float maximum=*std::max_element(values.begin(),values.end());
        if(!std::isfinite(maximum))return false;
        for(auto& value:values)if(value!=maximum)value=0;
    }
    return true;
}
bool PreparedIntervalHistogram::response(std::span<const float> input) noexcept {
    if(config_.method==IntervalHistogramMethod::Comb)
        return comb_.process(input,response_) && retain_maxima();
    const float value=input[0];
    for(std::size_t i=0;i<intervals_.size();++i){
        const auto delay=intervals_[i],row=activation_cursor_>=delay?activation_cursor_-delay:activations_.size()-(delay-activation_cursor_);
        response_[i]=value*activations_[row];
        if(!std::isfinite(response_[i]))return false;
    }
    activations_[activation_cursor_]=value;
    if(++activation_cursor_==activations_.size())activation_cursor_=0;
    return true;
}
bool PreparedIntervalHistogram::process_offline(std::span<const float> input) noexcept {
    ready_=false;if(!validate(input))return false;reset();
    if(config_.method==IntervalHistogramMethod::Autocorrelation){ready_=offline_acf(input);return ready_;}
    for(std::size_t frame=0;frame<input.size()/config_.channels;++frame){
        if(!response(input.subspan(frame*config_.channels,config_.channels)))return false;
        for(std::size_t i=0;i<bins_.size();++i)bins_[i]+=response_[i];
    }
    ready_=std::all_of(bins_.begin(),bins_.end(),[](float x){return std::isfinite(x);});return ready_;
}
bool PreparedIntervalHistogram::append(std::span<const float> input) noexcept {
    if(!response(input))return false;
    std::copy(response_.begin(),response_.end(),history_.begin()+std::ptrdiff_t(histogram_cursor_*bins_.size()));
    if(++histogram_cursor_==config_.history_frames)histogram_cursor_=0;
    return true;
}
bool PreparedIntervalHistogram::summarize() noexcept {
    std::fill(bins_.begin(),bins_.end(),0.f);
    auto row=histogram_cursor_;
    for(std::size_t frame=0;frame<config_.history_frames;++frame){
        for(std::size_t i=0;i<bins_.size();++i)bins_[i]+=history_[row*bins_.size()+i];
        if(++row==config_.history_frames)row=0;
    }
    return std::all_of(bins_.begin(),bins_.end(),[](float x){return std::isfinite(x);});
}
bool PreparedIntervalHistogram::process_online(std::span<const float> input,bool reset_state) noexcept {
    ready_=false;if(!validate(input))return false;
    if(reset_state || !streaming_)reset();
    for(std::size_t frame=0;frame<input.size()/config_.channels;++frame)
        if(!append(input.subspan(frame*config_.channels,config_.channels)))return false;
    streaming_=true;ready_=summarize();return ready_;
}
}


namespace dsp_kernels {
bool PreparedIntervalHistogram::storage_bytes(std::size_t frames,IntervalHistogramConfig c,std::span<const float> alpha,std::size_t& out) noexcept {
    if(!frames || !c.minimum_interval || c.maximum_interval<c.minimum_interval || c.maximum_interval==SIZE_MAX || !c.history_frames ||
       unsigned(c.method)>unsigned(IntervalHistogramMethod::Comb) || !c.channels ||
       (c.method==IntervalHistogramMethod::Autocorrelation && (c.channels!=1 || !alpha.empty())))return false;
    const auto count=c.maximum_interval-c.minimum_interval+1;std::size_t bins=0,history=0,bytes=0;
    if(!prepared_storage::mul_bytes(count,c.channels,bins) || !prepared_storage::mul_bytes(c.history_frames,bins,history))return false;
    if(c.method==IntervalHistogramMethod::Comb){
        const auto factors=alpha.empty()?std::span(&c.alpha,1):alpha;
        if((factors.size()!=1 && factors.size()!=count) || !std::all_of(factors.begin(),factors.end(),[](float v){return std::isfinite(v);}) ||
           !PreparedCombFilterBank::storage_bytes(count,c.maximum_interval,CombDirection::Backward,c.channels,bytes))return false;
    }
    if(!prepared_storage::add_array(bytes,count,sizeof(std::size_t)) || !prepared_storage::add_array(bytes,bins,2*sizeof(float)) ||
       !prepared_storage::add_array(bytes,c.maximum_interval+1,2*sizeof(float)) || !prepared_storage::add_array(bytes,history,sizeof(float)) ||
       !prepared_storage::add_array(bytes,frames,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedIntervalHistogram::workspace_bytes() const noexcept {
    return comb_.workspace_bytes()+intervals_.capacity()*sizeof(std::size_t)+
        (bins_.capacity()+response_.capacity()+activations_.capacity()+history_.capacity()+absolute_.capacity()+acf_.capacity())*sizeof(float);
}
}
