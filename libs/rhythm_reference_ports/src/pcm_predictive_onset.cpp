// SPDX-License-Identifier: GPL-3.0-or-later
// BTrack OnsetDetectionFunction.cpp, 9d6127618a5679e9caa74c594b88f1d74f0e035f.
// Copyright (C) 2008-2014 Queen Mary University of London, Adam Stark.
// Float32 decomposition Copyright (C) 2026 Koba Production.
// GPL version 3 or later; no warranty. See ../COPYING.
#include <rhythm_reference_ports/pcm_predictive_onset.h>
#include <algorithm>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
namespace {
bool reference_config(const PcmPredictiveOnsetConfig& c) noexcept {
    return c.precision==dsp_kernels::NumericalPrecision::DesktopReference;
}
bool portable_config(const PcmPredictiveOnsetConfig& c) noexcept {
    return c.precision==dsp_kernels::NumericalPrecision::PortableFloat32;
}
bool reference_storage(PcmPredictiveOnsetConfig config,std::size_t& output,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    std::size_t bytes=0;
    if(!detail::PreparedPredictiveReferenceFrontend::storage_bytes(config.frame_samples,config.hop_samples,config.method,config.window,bytes,factory) ||
       !dsp_kernels::prepared_storage::add_array(bytes,config.frame_samples,4*sizeof(float)))return false;
    output=bytes;return true;
}
}
bool PreparedPcmPredictiveOnset::prepare(PcmPredictiveOnsetConfig config,const dsp_kernels::FourierBackendFactory* factory) {
    PreparedPcmPredictiveOnset next;std::size_t bytes=0;
    if(!storage_bytes(config,bytes,factory) || !next.configure(config,factory) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedPcmPredictiveOnset::storage_bytes(PcmPredictiveOnsetConfig config,std::size_t& output,
                                              const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(reference_config(config))return reference_storage(config,output,factory);
    if(!portable_config(config) || factory)return false;
    PredictiveOnsetParameters parameters;
    if(config.frame_samples<2 || config.frame_samples>INT32_MAX || !config.hop_samples || config.hop_samples>config.frame_samples ||
       unsigned(config.window)>unsigned(PredictiveWindowShape::ShiftedTaper) ||
       !predictive_onset_parameters(config.method,config.frame_samples,parameters))return false;
    std::size_t bytes=0,nested=0;
    if(!PreparedHopSpectrum::storage_bytes({config.frame_samples,config.hop_samples},{},bytes) ||
       !dsp_kernels::PreparedSpectralDescriptor::storage_bytes(config.frame_samples,nested) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,nested) ||
       !dsp_kernels::prepared_storage::add_array(bytes,config.frame_samples,5*sizeof(float)))return false;
    output=bytes;return true;
}
std::size_t PreparedPcmPredictiveOnset::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+descriptor_.workspace_bytes()+reference_.workspace_bytes()+
        (complex_.capacity()+magnitude_.capacity()+phase_.capacity())*sizeof(float);
}
bool PreparedPcmPredictiveOnset::configure_analysis(PcmPredictiveOnsetConfig c) {
    std::vector<float> coefficients(c.frame_samples);
    if(!predictive_window(coefficients,c.window))return false;
    if(!spectrum_.prepare({c.frame_samples,c.hop_samples},coefficients) ||
        !descriptor_.prepare(c.frame_samples))return false;
    complex_.resize(2*std::size_t(c.frame_samples));magnitude_.resize(c.frame_samples);phase_.resize(c.frame_samples);
    dsp_kernels::EnergyNoveltyConfig energy;energy.compensated_sum=false;
    if(!energy_.prepare(energy))return false;
    energy.positive_difference=true;return energy_difference_.prepare(energy);
}
bool PreparedPcmPredictiveOnset::configure(PcmPredictiveOnsetConfig c,const dsp_kernels::FourierBackendFactory* factory) {
    if(reference_config(c))return configure_reference(c,factory);
    PredictiveOnsetParameters parameters;
    if(c.frame_samples<2 || c.frame_samples>INT32_MAX || !c.hop_samples || c.hop_samples>c.frame_samples ||
        !predictive_onset_parameters(c.method,c.frame_samples,parameters))return false;
    if(!configure_analysis(c))return false;
    config_=c;prepared_=true;return set_method(c.method);
}
bool PreparedPcmPredictiveOnset::configure_reference(PcmPredictiveOnsetConfig c,const dsp_kernels::FourierBackendFactory* factory) {
    if(!reference_.prepare(c.frame_samples,c.hop_samples,c.method,c.window,factory))return false;
    complex_.resize(2*std::size_t(c.frame_samples));magnitude_.resize(c.frame_samples);phase_.resize(c.frame_samples);
    config_=c;prepared_=true;return true;
}
bool PreparedPcmPredictiveOnset::set_method(PredictiveOnsetMethod method) noexcept {
    if(config_.precision==dsp_kernels::NumericalPrecision::DesktopReference){
        if(!prepared_ || !reference_.set_method(method))return false;
        config_.method=method;return true;
    }
    PredictiveOnsetParameters parameters;
    if(!prepared_ || !predictive_onset_parameters(method,config_.frame_samples,parameters))return false;
    if(!parameters.uses_pcm_energy){
        // Explicit N-bin source order, rather than multiplying half-spectrum sums.
        parameters.spectral.mirror_spectrum=false;
        if(!descriptor_.set_parameters(parameters.spectral))return false;
    }
    config_.method=method;return true;
}
void PreparedPcmPredictiveOnset::restart() noexcept {
    spectrum_.restart();reference_.restart();descriptor_.reset();energy_.reset();energy_difference_.reset();
    std::fill(complex_.begin(),complex_.end(),0.f);
    std::fill(magnitude_.begin(),magnitude_.end(),0.f);std::fill(phase_.begin(),phase_.end(),0.f);
}
bool PreparedPcmPredictiveOnset::process_spectral(float& output) noexcept {
    if(!spectrum_.analyze_current() ||
        !dsp_kernels::expand_real_spectrum(spectrum_.complex_bins(),config_.frame_samples,complex_))return false;
    if(!dsp_kernels::cartesian_to_polar(complex_,magnitude_,phase_,dsp_kernels::ZeroMagnitudePhase::Atan2,
        dsp_kernels::ComplexMagnitudeNorm::SquaredNorm))return false;
    return descriptor_.process_polar(magnitude_,phase_,output);
}
bool PreparedPcmPredictiveOnset::process_hop(std::span<const float> pcm,float& output) noexcept {
    if(prepared_ && reference_config(config_))return reference_.process_hop(pcm,complex_,magnitude_,phase_,output);
    if(!prepared_ || !spectrum_.append_hop(pcm))return false;
    if(config_.method==PredictiveOnsetMethod::EnergyEnvelope)return energy_.process(spectrum_.raw_pcm_history(),output);
    if(config_.method==PredictiveOnsetMethod::EnergyDifference)return energy_difference_.process(spectrum_.raw_pcm_history(),output);
    return process_spectral(output);
}
std::span<const float> PreparedPcmPredictiveOnset::raw_pcm_history() const noexcept {
    return reference_config(config_)?reference_.raw_pcm_history():spectrum_.raw_pcm_history();
}
}
