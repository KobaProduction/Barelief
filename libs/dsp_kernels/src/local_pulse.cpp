// Adapted from librosa 1.0.0 beat.py: plp, ISC.
// Copyright (c) 2013--2026, librosa development team.
// Full notice: packaging/licenses/librosa.txt.
// Source: librosa/librosa@3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/local_pulse.h>
#include <dsp_kernels/tempogram.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_kernels {
namespace {
bool valid(const LocalPulseConfig& c) noexcept {
    return std::isfinite(c.frame_rate) && c.frame_rate>0 && std::isfinite(c.frame_rate*60.f) &&
        (!c.limit_min || !std::isnan(c.min_bpm)) &&
        (!c.limit_max || !std::isnan(c.max_bpm)) &&
        (!c.limit_min || !c.limit_max || c.max_bpm>c.min_bpm);
}
bool finite_prior(std::span<const float> prior) noexcept {
    return std::all_of(prior.begin(),prior.end(),[](float x){return !std::isnan(x) && x!=std::numeric_limits<float>::infinity();});
}
}
bool PreparedLocalPulse::prepare(std::size_t maximum,LocalPulseConfig config,std::span<const float> coefficients) {
    std::size_t required=0;if(!storage_bytes(maximum,config,coefficients,required))return false;
    if(!maximum || !valid(config))return false;
    PreparedLocalPulse next;if(!next.window_.prepare(config.window,coefficients) || !next.inverse_.prepare(config.window.length))return false;
    if(!next.overlap_.prepare(next.window_.frames(maximum),next.window_.coefficients(),config.window.hop,config.window.center))return false;
    next.config_=config;next.maximum_samples_=maximum;next.bins_=config.window.length/2+1;
    next.spectrum_.resize(2*next.bins_);next.score_.resize(next.bins_);next.real_.resize(config.window.length);
    next.imaginary_.resize(config.window.length);next.transformed_.resize(2*config.window.length);
    next.frame_.resize(config.window.length);next.pulse_.resize(maximum);next.prior_row_.resize(next.bins_);
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedLocalPulse::load(FloatMatrixSource source,std::size_t index,std::span<const float> prior) noexcept {
    if(!source.read || !source.read(source.context,index,spectrum_))return false;
    if(!std::all_of(spectrum_.begin(),spectrum_.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t i=0;i<bins_;++i){
        const float bpm=60.f*config_.frame_rate*float(i)/float(config_.window.length);
        if((config_.limit_min && bpm<config_.min_bpm) || (config_.limit_max && bpm>config_.max_bpm)){spectrum_[2*i]=0;spectrum_[2*i+1]=0;}
    }
    return select(prior);
}
bool PreparedLocalPulse::select(std::span<const float> prior) noexcept {
    for(std::size_t i=0;i<bins_;++i){
        score_[i]=std::log1p(1e6f*std::hypot(spectrum_[2*i],spectrum_[2*i+1]))+(prior.empty()?0:prior[i]);
        if(std::isnan(score_[i]) || score_[i]==std::numeric_limits<float>::infinity())return false;
    }
    const float peak=*std::max_element(score_.begin(),score_.end());
    for(std::size_t i=0;i<bins_;++i)if(score_[i]<peak){spectrum_[2*i]=0;spectrum_[2*i+1]=0;}
    const float scale=divisor();
    for(auto& value:spectrum_)value/=scale;
    return true;
}
float PreparedLocalPulse::divisor() const noexcept {
    // Original np.max on complex bins is lexicographic, NOT max(abs(bin)).
    float re=spectrum_[0],im=spectrum_[1];
    for(std::size_t i=1;i<bins_;++i)if(spectrum_[2*i]>re || (spectrum_[2*i]==re && spectrum_[2*i+1]>im)){re=spectrum_[2*i];im=spectrum_[2*i+1];}
    return std::sqrt(std::numeric_limits<float>::min())+std::hypot(re,im);
}
bool PreparedLocalPulse::reconstruct(std::size_t index) noexcept {
    std::fill(real_.begin(),real_.end(),0.f);std::fill(imaginary_.begin(),imaginary_.end(),0.f);
    for(std::size_t i=0;i<bins_;++i){
        real_[i]=spectrum_[2*i];imaginary_[i]=spectrum_[2*i+1];
        const auto mirror=real_.size()-i;
        if(i && mirror!=i){real_[mirror]=real_[i];imaginary_[mirror]=-imaginary_[i];}
    }
    imaginary_[0]=0;if(real_.size()%2==0)imaginary_[real_.size()/2]=0;
    if(!inverse_.process(real_,imaginary_,transformed_,true))return false;
    for(std::size_t i=0;i<frame_.size();++i)frame_[i]=transformed_[2*i];
    return overlap_.add(index,frame_);
}
bool PreparedLocalPulse::process(FloatMatrixSource source,std::size_t frames,std::span<float> output,std::span<const float> prior,FloatMatrixSource prior_rows) noexcept {
    if(!maximum_samples_ || output.size()>maximum_samples_ || frames!=window_.frames(output.size()))return false;
    if((!prior.empty() && prior.size()!=bins_) || !finite_prior(prior) || (prior_rows.read && !prior.empty()))return false;
    overlap_.reset();
    for(std::size_t i=0;i<frames;++i){
        if(prior_rows.read){
            if(!prior_rows.read(prior_rows.context,i,prior_row_) || !finite_prior(prior_row_))return false;
            prior=prior_row_;
        }
        if(!load(source,i,prior) || !reconstruct(i))return false;
    }
    return finish(output);
}
bool PreparedLocalPulse::finish(std::span<float> output) noexcept {
    auto values=std::span(pulse_).first(output.size());if(!overlap_.finish(values))return false;
    for(auto& value:values)value=std::max(0.f,value);
    if(!normalize_vector(values,VectorNormalization::Maximum))return false;
    std::copy(values.begin(),values.end(),output.begin());return true;
}
}


namespace dsp_kernels {
bool PreparedLocalPulse::storage_bytes(std::size_t maximum,LocalPulseConfig c,std::span<const float> coeff,std::size_t& out) noexcept {
    if(!maximum || !valid(c))return false;
    std::size_t bytes=0,nested=0,frames=0;
    if(!PreparedTemporalWindow::storage_bytes(c.window,coeff,bytes) || !PreparedTemporalWindow::frame_count(c.window,maximum,frames) ||
       !PreparedFourierTransform::storage_bytes(c.window.length,nested) || !prepared_storage::add_bytes(bytes,nested) ||
       !PreparedOverlapAdd::storage_bytes(frames,c.window.length,c.window.hop,OverlapNormalization::WindowSquared,OverlapSampleSelection::All,nested) ||
       !prepared_storage::add_bytes(bytes,nested))return false;
    if(!prepared_storage::add_array(bytes,c.window.length,5*sizeof(float)) ||
       !prepared_storage::add_array(bytes,c.window.length/2+1,4*sizeof(float)) || !prepared_storage::add_array(bytes,maximum,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedLocalPulse::workspace_bytes() const noexcept {
    return window_.workspace_bytes()+inverse_.workspace_bytes()+overlap_.workspace_bytes()+
        (spectrum_.capacity()+score_.capacity()+real_.capacity()+imaginary_.capacity()+transformed_.capacity()+frame_.capacity()+pulse_.capacity()+prior_row_.capacity())*sizeof(float);
}
}
