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
namespace {
bool comb_geometry(std::span<const std::size_t> delays,std::span<const float> alpha,CombDirection direction,std::size_t channels,std::size_t& capacity) noexcept {
    if(delays.empty() || !channels || (alpha.size()!=1 && alpha.size()!=delays.size()))return false;
    if(direction<CombDirection::Forward || direction>CombDirection::Backward)return false;
    if(!std::all_of(delays.begin(),delays.end(),[](auto d){return d && d<SIZE_MAX;}))return false;
    if(!std::all_of(alpha.begin(),alpha.end(),[](float a){return std::isfinite(a);}))return false;
    capacity=*std::max_element(delays.begin(),delays.end())+1;
    return channels<=SIZE_MAX/delays.size() && channels*delays.size()<=SIZE_MAX/capacity;
}
}
bool PreparedCombFilterBank::prepare(std::span<const std::size_t> delays,std::span<const float> alpha,CombDirection direction,std::size_t channels,NumericalPrecision precision) {
    std::size_t required=0;if(!storage_bytes(delays,alpha,direction,channels,precision,required))return false;
    PreparedCombFilterBank next;if(!next.configure(delays,alpha,direction,channels,precision))return false;
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedCombFilterBank::storage_bytes(std::size_t filters,std::size_t maximum,CombDirection direction,std::size_t channels,std::size_t& out) noexcept {
    return storage_bytes(filters,maximum,direction,channels,NumericalPrecision::PortableFloat32,out);
}
bool PreparedCombFilterBank::storage_bytes(std::size_t filters,std::size_t maximum,CombDirection direction,std::size_t channels,NumericalPrecision precision,std::size_t& out) noexcept {
    if(unsigned(precision)>unsigned(NumericalPrecision::DesktopReference))return false;
    const auto width=precision==NumericalPrecision::DesktopReference?sizeof(double):sizeof(float);
    if(!filters || !maximum || maximum==SIZE_MAX || !channels || unsigned(direction)>unsigned(CombDirection::Backward))return false;
    std::size_t bytes=0,values=0,history=0;
    if(!prepared_storage::mul_bytes(channels,filters,values) || !prepared_storage::mul_bytes(maximum+1,values,history) ||
       !prepared_storage::add_array(bytes,filters,sizeof(std::size_t)+sizeof(float)) || !prepared_storage::add_array(bytes,values,width) ||
       !prepared_storage::add_array(bytes,history,width))return false;
    out=bytes;return true;
}
bool PreparedCombFilterBank::storage_bytes(std::span<const std::size_t> delays,std::span<const float> alpha,
    CombDirection direction,std::size_t channels,std::size_t& out) noexcept {
    return storage_bytes(delays,alpha,direction,channels,NumericalPrecision::PortableFloat32,out);
}
bool PreparedCombFilterBank::storage_bytes(std::span<const std::size_t> delays,std::span<const float> alpha,
    CombDirection direction,std::size_t channels,NumericalPrecision precision,std::size_t& out) noexcept {
    if(delays.empty() || (alpha.size()!=1 && alpha.size()!=delays.size()) ||
       !std::all_of(delays.begin(),delays.end(),[](auto d){return d && d<SIZE_MAX;}) ||
       !std::all_of(alpha.begin(),alpha.end(),[](float a){return std::isfinite(a);}))return false;
    return storage_bytes(delays.size(),*std::max_element(delays.begin(),delays.end()),direction,channels,precision,out);
}
bool PreparedCombFilterBank::configure(std::span<const std::size_t> delays,std::span<const float> alpha,CombDirection direction,std::size_t channels,NumericalPrecision precision) {
    if(!comb_geometry(delays,alpha,direction,channels,capacity_))return false;
    channels_=channels;direction_=direction;precision_=precision;delays_.assign(delays.begin(),delays.end());
    allocate(alpha);return true;
}
void PreparedCombFilterBank::allocate(std::span<const float> alpha) {
    alpha_.resize(filters());
    if(precision_==NumericalPrecision::DesktopReference){
        reference_values_.resize(channels_*filters());reference_history_.resize(capacity_*reference_values_.size());
    }else{values_.resize(channels_*filters());history_.resize(capacity_*values_.size());}
    for(std::size_t i=0;i<alpha_.size();++i)alpha_[i]=alpha[alpha.size()==1?0:i];
}
void PreparedCombFilterBank::reset() noexcept {
    std::fill(reference_history_.begin(),reference_history_.end(),0.);std::fill(reference_values_.begin(),reference_values_.end(),0.);
    std::fill(history_.begin(),history_.end(),0.f);std::fill(values_.begin(),values_.end(),0.f);cursor_=0;
}
template<typename Scalar> Scalar PreparedCombFilterBank::delayed(std::size_t channel,std::size_t filter,std::span<const Scalar> history,std::size_t width) const noexcept {
    const auto delay=delays_[filter],row=cursor_>=delay?cursor_-delay:capacity_-(delay-cursor_);
    return history[row*width+channel*filters()+filter];
}
template<typename Scalar> void PreparedCombFilterBank::advance(std::span<const Scalar> input,std::span<const Scalar> values,std::span<Scalar> history) noexcept {
    for(std::size_t channel=0;channel<channels_;++channel)
        for(std::size_t filter=0;filter<filters();++filter){
            const auto index=channel*filters()+filter;
            history[cursor_*values.size()+index]=direction_==CombDirection::Forward?input[channel]:values[index];
        }
    if(++cursor_==capacity_)cursor_=0;
}
template<typename Scalar> bool PreparedCombFilterBank::respond(std::span<const Scalar> input,std::span<Scalar> values,std::span<const Scalar> history) noexcept {
    for(std::size_t channel=0;channel<channels_;++channel)
        for(std::size_t filter=0;filter<filters();++filter){
            const Scalar value=input[channel]+alpha_[filter]*delayed<Scalar>(channel,filter,history,values.size());
            if(!std::isfinite(value))return false;
            values[channel*filters()+filter]=value;
        }
    return true;
}
template<typename Scalar> bool PreparedCombFilterBank::evaluate(std::span<const Scalar> input,std::span<Scalar> output,std::span<Scalar> values,std::span<Scalar> history) noexcept {
    if(!channels_ || input.size()!=channels_ || output.size()!=values.size() || values.empty())return false;
    if(!std::all_of(input.begin(),input.end(),[](Scalar x){return std::isfinite(x);}))return false;
    if(!respond<Scalar>(input,values,history))return false;
    advance<Scalar>(input,values,history);std::copy(values.begin(),values.end(),output.begin());return true;
}
bool PreparedCombFilterBank::process(std::span<const float> input,std::span<float> output) noexcept {
    return evaluate<float>(input,output,values_,history_);
}
bool PreparedCombFilterBank::process(std::span<const double> input,std::span<double> output) noexcept {
    return evaluate<double>(input,output,reference_values_,reference_history_);
}
}
