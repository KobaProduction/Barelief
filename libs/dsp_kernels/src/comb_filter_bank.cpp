// Adapted from madmom audio/comb_filters.pyx at revision
// 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/comb_filter_bank.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool PreparedCombFilterBank::prepare(std::span<const std::size_t> delays,std::span<const float> alpha,CombDirection direction,std::size_t channels) {
    std::size_t required=0;if(!storage_bytes(delays,alpha,direction,channels,required))return false;
    PreparedCombFilterBank next;if(!next.configure(delays,alpha,direction,channels))return false;
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedCombFilterBank::storage_bytes(std::size_t filters,std::size_t maximum,CombDirection direction,std::size_t channels,std::size_t& out) noexcept {
    if(!filters || !maximum || maximum==SIZE_MAX || !channels || unsigned(direction)>unsigned(CombDirection::Backward))return false;
    std::size_t bytes=0,values=0,history=0;
    if(!prepared_storage::mul_bytes(channels,filters,values) || !prepared_storage::mul_bytes(maximum+1,values,history) ||
       !prepared_storage::add_array(bytes,filters,sizeof(std::size_t)+sizeof(float)) || !prepared_storage::add_array(bytes,values,sizeof(float)) ||
       !prepared_storage::add_array(bytes,history,sizeof(float)))return false;
    out=bytes;return true;
}
bool PreparedCombFilterBank::storage_bytes(std::span<const std::size_t> delays,std::span<const float> alpha,
    CombDirection direction,std::size_t channels,std::size_t& out) noexcept {
    if(delays.empty() || (alpha.size()!=1 && alpha.size()!=delays.size()) ||
       !std::all_of(delays.begin(),delays.end(),[](auto d){return d && d<SIZE_MAX;}) ||
       !std::all_of(alpha.begin(),alpha.end(),[](float a){return std::isfinite(a);}))return false;
    return storage_bytes(delays.size(),*std::max_element(delays.begin(),delays.end()),direction,channels,out);
}
bool PreparedCombFilterBank::configure(std::span<const std::size_t> delays,std::span<const float> alpha,CombDirection direction,std::size_t channels) {
    if(delays.empty() || !channels || (alpha.size()!=1 && alpha.size()!=delays.size()))return false;
    if(direction<CombDirection::Forward || direction>CombDirection::Backward)return false;
    if(!std::all_of(delays.begin(),delays.end(),[](auto d){return d && d<SIZE_MAX;}))return false;
    if(!std::all_of(alpha.begin(),alpha.end(),[](float a){return std::isfinite(a);}))return false;
    capacity_=*std::max_element(delays.begin(),delays.end())+1;
    if(channels>SIZE_MAX/delays.size() || channels*delays.size()>SIZE_MAX/capacity_)return false;
    channels_=channels;direction_=direction;delays_.assign(delays.begin(),delays.end());
    allocate(alpha);return true;
}
void PreparedCombFilterBank::allocate(std::span<const float> alpha) {
    alpha_.resize(filters());values_.resize(channels_*filters());history_.resize(capacity_*values_.size());
    for(std::size_t i=0;i<alpha_.size();++i)alpha_[i]=alpha[alpha.size()==1?0:i];
}
void PreparedCombFilterBank::reset() noexcept {
    std::fill(history_.begin(),history_.end(),0.f);std::fill(values_.begin(),values_.end(),0.f);cursor_=0;
}
float PreparedCombFilterBank::delayed(std::size_t channel,std::size_t filter) const noexcept {
    const auto delay=delays_[filter],row=cursor_>=delay?cursor_-delay:capacity_-(delay-cursor_);
    return history_[row*values_.size()+channel*filters()+filter];
}
void PreparedCombFilterBank::advance(std::span<const float> input) noexcept {
    for(std::size_t channel=0;channel<channels_;++channel)
        for(std::size_t filter=0;filter<filters();++filter){
            const auto index=channel*filters()+filter;
            history_[cursor_*values_.size()+index]=direction_==CombDirection::Forward?input[channel]:values_[index];
        }
    if(++cursor_==capacity_)cursor_=0;
}
bool PreparedCombFilterBank::process(std::span<const float> input,std::span<float> output) noexcept {
    if(!channels_ || input.size()!=channels_ || output.size()!=values_.size())return false;
    if(!std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t channel=0;channel<channels_;++channel)
        for(std::size_t filter=0;filter<filters();++filter){
            const float value=input[channel]+alpha_[filter]*delayed(channel,filter);
            if(!std::isfinite(value))return false;
            values_[channel*filters()+filter]=value;
        }
    advance(input);std::copy(values_.begin(),values_.end(),output.begin());return true;
}
}
