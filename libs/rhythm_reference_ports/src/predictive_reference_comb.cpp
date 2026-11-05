// SPDX-License-Identifier: GPL-3.0-or-later
// Literal typed stages of BTrack.cpp at adamstark/BTrack@
// 9d6127618a5679e9caa74c594b88f1d74f0e035f.
// Copyright (C) 2008-2014 Queen Mary University of London, Adam Stark.
// GPL version 3 or later; no warranty. See ../COPYING.
#include <rhythm_reference_ports/detail/predictive_reference_comb.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <utility>
namespace rhythm_reference_ports::detail {
namespace {
double mean(std::span<const double> values,std::size_t first,std::size_t last) noexcept {
    const double sum=std::accumulate(values.begin()+std::ptrdiff_t(first),values.begin()+std::ptrdiff_t(last),0.);
    return last>first?sum/double(last-first):0.;
}
bool float_range(double value) noexcept {
    return std::isfinite(value) && std::abs(value)<=std::numeric_limits<float>::max();
}
bool float_range(std::span<const double> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](double value){return float_range(value);});
}
void fill_thresholds(std::span<const double> values,std::span<double> thresholds) noexcept {
    const auto size=values.size(),boundary=std::min(size,std::size_t{7});
    for(std::size_t i=0;i<=boundary;++i)thresholds[i]=mean(values,1,std::min(i+8,size));
    for(std::size_t i=boundary+1;i<size-7;++i)thresholds[i]=mean(values,i-8,i+7);
    for(std::size_t i=size-7;i<size;++i)thresholds[i]=mean(values,std::max(i-7,std::size_t{1}),size);
}
std::size_t maximum_probability(std::span<const double> current) noexcept {
    double maximum=-1;std::size_t index=0;
    for(std::size_t i=0;i<41;++i)if(current[i]>maximum){maximum=current[i];index=i;}
    return index;
}
double transition_maximum(std::span<const double> previous,std::span<const double> transitions,std::size_t to) noexcept {
    double maximum=-1;
    for(std::size_t from=0;from<41;++from){
        const double value=previous[from]*transitions[from*41+to];
        if(value>maximum)maximum=value;
    }
    return maximum;
}
}
bool PreparedPredictiveReferenceComb::storage_bytes(std::size_t hop,std::size_t& output,
                                                   const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(!factory || !hop || hop>262144)return false;
    dsp_kernels::FourierBackendResources forward,inverse;std::size_t bytes=0;
    if(!dsp_kernels::PreparedFourierBackend::query({1024,dsp_kernels::FourierBackendTransform::ComplexForward},*factory,forward) ||
       !dsp_kernels::PreparedFourierBackend::query({1024,dsp_kernels::FourierBackendTransform::ComplexInverse},*factory,inverse) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,forward.cold_peak_bytes) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,inverse.cold_peak_bytes) ||
       !dsp_kernels::prepared_storage::add_array(bytes,4096,sizeof(float)) ||
       !dsp_kernels::prepared_storage::add_array(bytes,3637,sizeof(double)))return false;
    output=bytes;return true;
}
bool PreparedPredictiveReferenceComb::prepare(std::size_t hop,const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t bytes=0;if(!storage_bytes(hop,bytes,factory))return false;
    PreparedPredictiveReferenceComb next;
    if(!next.configure(hop,*factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPredictiveReferenceComb::configure(std::size_t hop,const dsp_kernels::FourierBackendFactory& factory) {
    if(!forward_.prepare({1024,dsp_kernels::FourierBackendTransform::ComplexForward},factory) ||
       !inverse_.prepare({1024,dsp_kernels::FourierBackendTransform::ComplexInverse},factory))return false;
    prepare_buffers();prepare_weights();prepare_transitions();hop_=hop;reset();return true;
}
void PreparedPredictiveReferenceComb::prepare_buffers() {
    input_.resize(2048);spectrum_.resize(2048);onset_.resize(512);acf_.resize(512);threshold_.resize(512);
    rayleigh_.resize(128);comb_.resize(128);observation_.resize(41);previous_.resize(41);
    fixed_previous_.resize(41);current_.resize(41);transitions_.resize(41*41);
}
std::size_t PreparedPredictiveReferenceComb::workspace_bytes() const noexcept {
    return forward_.workspace_bytes()+inverse_.workspace_bytes()+(input_.capacity()+spectrum_.capacity())*sizeof(float)+
        (onset_.capacity()+acf_.capacity()+threshold_.capacity()+rayleigh_.capacity()+comb_.capacity()+observation_.capacity()+
         previous_.capacity()+fixed_previous_.capacity()+current_.capacity()+transitions_.capacity())*sizeof(double);
}
void PreparedPredictiveReferenceComb::prepare_weights() noexcept {
    for(std::size_t i=0;i<rayleigh_.size();++i)
        rayleigh_[i]=(double(i)/std::pow(43.,2))*std::exp((-1*std::pow(-double(i),2))/(2*std::pow(43.,2)));
}
void PreparedPredictiveReferenceComb::prepare_transitions() noexcept {
    const double sigma=41/8;
    for(std::size_t from=0;from<41;++from)for(std::size_t to=0;to<41;++to){
        const double x=double(to+1),mu=double(from+1);
        transitions_[from*41+to]=(1/(sigma*std::sqrt(2*std::numbers::pi_v<double>)))*
            std::exp((-1*std::pow(x-mu,2))/(2*std::pow(sigma,2)));
    }
}
void PreparedPredictiveReferenceComb::reset() noexcept {
    fixed_=false;bpm_=120;
    std::fill(onset_.begin(),onset_.end(),0.);std::fill(acf_.begin(),acf_.end(),0.);
    std::fill(threshold_.begin(),threshold_.end(),0.);std::fill(comb_.begin(),comb_.end(),0.);
    std::fill(observation_.begin(),observation_.end(),0.);std::fill(previous_.begin(),previous_.end(),1.);
    std::fill(fixed_previous_.begin(),fixed_previous_.end(),0.);std::fill(current_.begin(),current_.end(),0.);
    std::fill(input_.begin(),input_.end(),0.f);std::fill(spectrum_.begin(),spectrum_.end(),0.f);
}
bool PreparedPredictiveReferenceComb::set_hop_samples(std::size_t hop) noexcept {
    if(!hop_ || !hop || hop>262144)return false;
    hop_=hop;return true;
}
void PreparedPredictiveReferenceComb::threshold(std::span<double> values) noexcept {
    fill_thresholds(values,threshold_);
    for(std::size_t i=0;i<values.size();++i){values[i]=values[i]-threshold_[i];if(values[i]<0)values[i]=0;}
}
bool PreparedPredictiveReferenceComb::load() noexcept {
    if(!float_range(onset_))return false;
    std::fill(input_.begin(),input_.end(),0.f);
    for(std::size_t i=0;i<onset_.size();++i)input_[2*i]=float(onset_[i]);
    return true;
}
bool PreparedPredictiveReferenceComb::correlate() noexcept {
    if(!load() || !forward_.process(input_,spectrum_))return false;
    for(std::size_t i=0;i<1024;++i){
        const float real=spectrum_[2*i],imaginary=spectrum_[2*i+1];
        spectrum_[2*i]=real*real+imaginary*imaginary;spectrum_[2*i+1]=0.f;
    }
    return inverse_.process(spectrum_,input_) && finish_correlation();
}
bool PreparedPredictiveReferenceComb::finish_correlation() noexcept {
    double lag=512;
    for(std::size_t i=0;i<acf_.size();++i){
        // The generic inverse includes 1/N. Undo that exact power-of-two
        // Float32 scaling before the source's Float32 squared sum. Values lost
        // to inverse underflow also have zero source Float32 squared sum.
        const float real=input_[2*i]*1024.f,imaginary=input_[2*i+1]*1024.f;
        // The genuine compiled source rounds the Float32 sqrt result before
        // storing absValue in Double; widening the sqrt itself changes ACF.
        const double absolute=std::sqrt(real*real+imaginary*imaginary);
        acf_[i]=absolute/lag;acf_[i]=acf_[i]/1024.;lag=lag-1.;
    }
    return float_range(acf_);
}
void PreparedPredictiveReferenceComb::filter() noexcept {
    std::fill(comb_.begin(),comb_.end(),0.);
    for(std::ptrdiff_t period=2;period<=127;++period)for(std::ptrdiff_t harmonic=1;harmonic<=4;++harmonic)
        for(std::ptrdiff_t offset=1-harmonic;offset<=harmonic-1;++offset)
            comb_[std::size_t(period-1)]+=(acf_[std::size_t(harmonic*period+offset-1)]*rayleigh_[std::size_t(period-1)])/(2*harmonic-1);
}
void PreparedPredictiveReferenceComb::observe() noexcept {
    const double factor=60.*44100./512.;
    for(std::size_t i=0;i<observation_.size();++i){
        const auto first=std::size_t(std::round(factor/double(2*i+80))),second=std::size_t(std::round(factor/double(4*i+160)));
        observation_[i]=comb_[first-1]+comb_[second-1];
    }
}
bool PreparedPredictiveReferenceComb::transition() noexcept {
    for(std::size_t to=0;to<41;++to){
        const double maximum=transition_maximum(fixed_?fixed_previous_:previous_,transitions_,to);
        current_[to]=maximum*observation_[to];
    }
    const double total=std::accumulate(current_.begin(),current_.end(),0.);
    if(!std::isfinite(total))return false;
    if(total>0)for(auto& value:current_)value=value/total;
    return float_range(current_);
}
bool PreparedPredictiveReferenceComb::select(float& period) noexcept {
    const auto index=maximum_probability(current_);
    const double selected=std::round((60.*44100.)/(double(2*index+80)*double(hop_)));
    if(!float_range(selected) || selected<=0)return false;
    const double bpm=60./((double(hop_)/44100.)*selected);
    if(!float_range(bpm))return false;
    std::copy(current_.begin(),current_.end(),previous_.begin());bpm_=bpm;period=float(selected);return true;
}
bool PreparedPredictiveReferenceComb::process(std::span<const float> values,float& period) noexcept {
    if(!hop_ || values.size()!=512 || !std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}))return false;
    std::copy(values.begin(),values.end(),onset_.begin());threshold(onset_);
    if(!correlate())return false;
    filter();threshold(comb_);observe();
    return float_range(comb_) && float_range(observation_) && transition() && select(period);
}
std::size_t PreparedPredictiveReferenceComb::tempo_index(float value) const noexcept {
    if(!std::isfinite(value) || value<=0)return 41;
    double tempo=value;
    while(tempo>160)tempo=tempo/2;
    while(tempo<80)tempo=tempo*2;
    return std::size_t(std::round((tempo-80.)/2));
}
bool PreparedPredictiveReferenceComb::set_tempo(float value) noexcept {
    const auto index=tempo_index(value);if(!hop_ || index>=41)return false;
    std::fill(previous_.begin(),previous_.end(),0.);previous_[index]=1;return true;
}
bool PreparedPredictiveReferenceComb::fix_tempo(float value) noexcept {
    const auto index=tempo_index(value);if(!hop_ || index>=41)return false;
    std::fill(fixed_previous_.begin(),fixed_previous_.end(),0.);fixed_previous_[index]=1;fixed_=true;return true;
}
}
