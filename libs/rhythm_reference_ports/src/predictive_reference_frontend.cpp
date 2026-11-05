// SPDX-License-Identifier: GPL-3.0-or-later
// OnsetDetectionFunction PCM/window/ten ODF methods at
// adamstark/BTrack@9d6127618a5679e9caa74c594b88f1d74f0e035f.
// Copyright (C) 2008-2014 Queen Mary University of London, Adam Stark.
// GPL version 3 or later; no warranty. See ../COPYING.
#include <rhythm_reference_ports/detail/predictive_reference_frontend.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
namespace rhythm_reference_ports::detail {
namespace {
constexpr double source_pi=3.14159265358979;
bool phase_method(PredictiveOnsetMethod method) noexcept {
    return method==PredictiveOnsetMethod::PhaseDeviation || method==PredictiveOnsetMethod::ComplexDifference ||
        method==PredictiveOnsetMethod::PositiveComplexDifference;
}
bool symmetric_method(PredictiveOnsetMethod method) noexcept {
    return method==PredictiveOnsetMethod::SpectralDifference || method==PredictiveOnsetMethod::PositiveSpectralDifference;
}
bool polar_value(std::span<const float> complex,std::size_t i,PredictiveOnsetMethod method,double& magnitude,double& phase) noexcept {
    const double real=complex[2*i],imaginary=complex[2*i+1];
    if(phase_method(method))phase=std::atan2(imaginary,real);
    magnitude=std::sqrt(std::pow(real,2)+std::pow(imaginary,2));
    return std::isfinite(magnitude) && magnitude<=std::numeric_limits<float>::max() && std::isfinite(phase);
}

double coefficient(PredictiveWindowShape shape,std::size_t index,std::size_t count) noexcept {
    const double period=double(count-1),position=double(std::ptrdiff_t(index)-std::ptrdiff_t(count/2)+1);
    const double phase=double(index)/period;
    switch(shape){
    case PredictiveWindowShape::Rectangle:return 1.;
    case PredictiveWindowShape::Hann:return .5*(1-std::cos(2*source_pi*phase));
    case PredictiveWindowShape::Hamming:return .54-(.46*std::cos(2.*source_pi*phase));
    case PredictiveWindowShape::Blackman:return .42-(.5*std::cos(2*source_pi*phase))+(.08*std::cos(4.*source_pi*phase));
    case PredictiveWindowShape::ShiftedTaper:
        return std::abs(position)<=.5*(period/2)?1.:.5*(1+std::cos(source_pi*(((2*position)/(.5*period))-1)));
    }
    return 0.;
}
}
bool PreparedPredictiveReferenceFrontend::window_coefficients(std::span<double> output,PredictiveWindowShape shape) noexcept {
    if(output.size()<2 || output.size()>std::size_t(INT32_MAX) || unsigned(shape)>unsigned(PredictiveWindowShape::ShiftedTaper))return false;
    for(std::size_t i=0;i<output.size();++i)output[i]=coefficient(shape,i,output.size());
    return true;
}
bool PreparedPredictiveReferenceFrontend::complex_storage_bytes(std::size_t frame,std::size_t& output) noexcept {
    output=0;
    return frame>=2 && frame<=INT32_MAX && frame%2==0 &&
        dsp_kernels::prepared_storage::add_array(output,frame,5*sizeof(double));
}
bool PreparedPredictiveReferenceFrontend::prepare_complex(std::size_t frame) {
    std::size_t bytes=0;if(!complex_storage_bytes(frame,bytes))return false;
    PreparedPredictiveReferenceFrontend next;
    next.magnitude_.resize(frame);next.phase_.resize(frame);next.previous_magnitude_.resize(frame);
    next.previous_phase_.resize(frame);next.older_phase_.resize(frame);
    if(next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPredictiveReferenceFrontend::process_complex(std::span<const float> complex,float& output) noexcept {
    if(magnitude_.empty() || complex.size()!=2*magnitude_.size() ||
        !std::all_of(complex.begin(),complex.end(),[](float x){return std::isfinite(x);}))return false;
    if(!polar(complex) || !evaluate(output))return false;
    commit_history();return true;
}
bool PreparedPredictiveReferenceFrontend::storage_bytes(std::uint32_t frame,std::uint32_t hop,PredictiveOnsetMethod method,
                                                        PredictiveWindowShape window,std::size_t& output,
                                                        const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(!factory || frame<2 || frame>INT32_MAX || frame%2 || !hop || hop>frame ||
       unsigned(method)>unsigned(PredictiveOnsetMethod::PositiveFrequencyDifference) ||
       unsigned(window)>unsigned(PredictiveWindowShape::ShiftedTaper))return false;
    dsp_kernels::FourierBackendResources resources;std::size_t bytes=0;
    if(!dsp_kernels::PreparedFourierBackend::query({frame,dsp_kernels::FourierBackendTransform::ComplexForward},*factory,resources) ||
       !ScalarHistory::storage_bytes(frame,bytes) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,resources.cold_peak_bytes) ||
       !dsp_kernels::prepared_storage::add_array(bytes,frame,2*sizeof(float)+6*sizeof(double)))return false;
    output=bytes;return true;
}
bool PreparedPredictiveReferenceFrontend::prepare(std::uint32_t frame,std::uint32_t hop,PredictiveOnsetMethod method,PredictiveWindowShape window,
                                                  const dsp_kernels::FourierBackendFactory* factory) {
    std::size_t bytes=0;
    if(!storage_bytes(frame,hop,method,window,bytes,factory))return false;
    PreparedPredictiveReferenceFrontend next;
    if(!next.configure(frame,hop,method,window,*factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPredictiveReferenceFrontend::configure(std::uint32_t frame,std::uint32_t hop,PredictiveOnsetMethod method,PredictiveWindowShape shape,
                                                    const dsp_kernels::FourierBackendFactory& factory) {
    if(!history_.prepare(frame) || !fft_.prepare({frame,dsp_kernels::FourierBackendTransform::ComplexForward},factory))return false;
    window_.resize(frame);input_.resize(2*std::size_t(frame));magnitude_.resize(frame);phase_.resize(frame);
    previous_magnitude_.resize(frame);previous_phase_.resize(frame);older_phase_.resize(frame);
    if(!window_coefficients(window_,shape))return false;
    hop_=hop;method_=method;return true;
}
std::size_t PreparedPredictiveReferenceFrontend::workspace_bytes() const noexcept {
    return fft_.workspace_bytes()+history_.workspace_bytes()+input_.capacity()*sizeof(float)+
        (window_.capacity()+magnitude_.capacity()+phase_.capacity()+previous_magnitude_.capacity()+
         previous_phase_.capacity()+older_phase_.capacity())*sizeof(double);
}
void PreparedPredictiveReferenceFrontend::restart() noexcept {
    history_.clear();std::fill(input_.begin(),input_.end(),0.f);
    std::fill(magnitude_.begin(),magnitude_.end(),0.);std::fill(phase_.begin(),phase_.end(),0.);
    std::fill(previous_magnitude_.begin(),previous_magnitude_.end(),0.);
    std::fill(previous_phase_.begin(),previous_phase_.end(),0.);std::fill(older_phase_.begin(),older_phase_.end(),0.);value_=0;previous_energy_=0;
}
bool PreparedPredictiveReferenceFrontend::set_method(PredictiveOnsetMethod method) noexcept {
    if(!hop_ || unsigned(method)>unsigned(PredictiveOnsetMethod::PositiveFrequencyDifference))return false;
    method_=method;return true;
}
void PreparedPredictiveReferenceFrontend::window_frame() noexcept {
    const auto pcm=history_.values();const auto half=pcm.size()/2;
    for(std::size_t i=0;i<pcm.size();++i){
        const auto source=(i+half)%pcm.size();
        input_[2*i]=float(double(pcm[source])*window_[source]);input_[2*i+1]=0.f;
    }
}
bool PreparedPredictiveReferenceFrontend::polar(std::span<const float> complex) noexcept {
    const auto count=symmetric_method(method_)?magnitude_.size()/2+1:magnitude_.size();
    for(std::size_t i=0;i<count;++i)
        if(!polar_value(complex,i,method_,magnitude_[i],phase_[i]))return false;
    if(symmetric_method(method_))for(std::size_t i=count;i<magnitude_.size();++i)magnitude_[i]=magnitude_[magnitude_.size()-i];
    return true;
}
double PreparedPredictiveReferenceFrontend::distance(std::size_t i) const noexcept {
    const double current=magnitude_[i],previous=previous_magnitude_[i];
    if(method_==PredictiveOnsetMethod::PositiveComplexDifference && current-previous<=0)return 0;
    // This branch deliberately uses the source's raw, unwrapped deviation.
    const double deviation=phase_[i]-(2*previous_phase_[i])+older_phase_[i];
    return std::sqrt(std::pow(current,2)+std::pow(previous,2)-2*current*previous*std::cos(deviation));
}
double PreparedPredictiveReferenceFrontend::phase_deviation(std::size_t i) const noexcept {
    if(magnitude_[i]<=.1)return 0;
    double value=phase_[i]-(2*previous_phase_[i])+older_phase_[i];
    while(value<=-source_pi)value=value+(2*source_pi);
    while(value>source_pi)value=value-(2*source_pi);
    return value<0?value*-1:value;
}
double PreparedPredictiveReferenceFrontend::contribution(std::size_t i) const noexcept {
    const double change=magnitude_[i]-previous_magnitude_[i],weight=double(i+1);
    switch(method_){
    case PredictiveOnsetMethod::SpectralDifference:return change<0?change*-1:change;
    case PredictiveOnsetMethod::PositiveSpectralDifference:return change>0?change:0;
    case PredictiveOnsetMethod::PhaseDeviation:return phase_deviation(i);
    case PredictiveOnsetMethod::ComplexDifference:
    case PredictiveOnsetMethod::PositiveComplexDifference:return distance(i);
    case PredictiveOnsetMethod::FrequencyContent:return magnitude_[i]*weight;
    case PredictiveOnsetMethod::FrequencyDifference:return (change<0?-change:change)*weight;
    case PredictiveOnsetMethod::PositiveFrequencyDifference:return change>0?change*weight:0;
    default:return 0;
    }
}
bool PreparedPredictiveReferenceFrontend::publish(double sum,float& output) noexcept {
    if(!std::isfinite(sum) || std::abs(sum)>std::numeric_limits<float>::max())return false;
    value_=sum;output=float(sum);return true;
}
bool PreparedPredictiveReferenceFrontend::energy(float& output) noexcept {
    double sum=0;
    for(float sample:history_.values())sum=sum+(double(sample)*double(sample));
    if(method_==PredictiveOnsetMethod::EnergyEnvelope)return publish(sum,output);
    const double difference=sum-previous_energy_;
    if(!publish(difference>0?difference:0,output))return false;
    previous_energy_=sum;return true;
}
bool PreparedPredictiveReferenceFrontend::evaluate(float& output) noexcept {
    double sum=0;
    for(std::size_t i=0;i<magnitude_.size();++i)sum=sum+contribution(i);
    return publish(sum,output);
}
void PreparedPredictiveReferenceFrontend::commit(std::span<float> magnitude,std::span<float> phase) noexcept {
    for(std::size_t i=0;i<magnitude_.size();++i){
        magnitude[i]=float(magnitude_[i]);phase[i]=float(phase_[i]);
    }
    commit_history();
}
void PreparedPredictiveReferenceFrontend::commit_history() noexcept {
    for(std::size_t i=0;i<magnitude_.size();++i){
        if(phase_method(method_)){older_phase_[i]=previous_phase_[i];previous_phase_[i]=phase_[i];}
        if(method_!=PredictiveOnsetMethod::PhaseDeviation)previous_magnitude_[i]=magnitude_[i];
    }
}
bool PreparedPredictiveReferenceFrontend::process_hop(std::span<const float> pcm,std::span<float> complex,
                                                     std::span<float> magnitude,std::span<float> phase,float& output) noexcept {
    if(!hop_ || pcm.size()!=hop_ || complex.size()!=input_.size() || magnitude.size()!=magnitude_.size() || phase.size()!=phase_.size() ||
       !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))return false;
    for(float x:pcm)history_.push(x);
    if(method_==PredictiveOnsetMethod::EnergyEnvelope || method_==PredictiveOnsetMethod::EnergyDifference)return energy(output);
    window_frame();
    if(!fft_.process(input_,complex) || !polar(complex) || !evaluate(output))return false;
    commit(magnitude,phase);return true;
}
}
