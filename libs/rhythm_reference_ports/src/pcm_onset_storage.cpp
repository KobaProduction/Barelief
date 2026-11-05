// SPDX-License-Identifier: GPL-3.0-or-later
#include <rhythm_reference_ports/pcm_onset_sequence.h>
#include <dsp_kernels/prepared_storage.h>

namespace rhythm_reference_ports {
bool PreparedPcmOnsetSequence::storage_bytes(PcmOnsetSequenceConfig config,std::size_t& output,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(!config.sample_rate || config.sample_rate>INT32_MAX || !config.spectrum.hop_samples)return false;
    std::size_t bytes=0,nested=0;const auto bins=std::size_t(config.spectrum.window_samples)/2+1;
    OnsetMagnitudeConfig magnitude;magnitude.whitening.frame_period_s=float(config.spectrum.hop_samples)/float(config.sample_rate);
    OnsetDecisionConfig decision;using namespace dsp_kernels::prepared_storage;
    if(!PreparedHopSpectrum::storage_bytes(config.spectrum,{},bytes,factory) ||
       !PreparedOnsetDescriptor::storage_bytes(bins,config.method,nested) || !add_bytes(bytes,nested) ||
       !PreparedOnsetMagnitude::storage_bytes(bins,magnitude,nested) || !add_bytes(bytes,nested) ||
       !PreparedAdaptivePeakPicker::storage_bytes({},nested) || !add_bytes(bytes,nested) ||
       !onset_decision_parameters(config.method,config.spectrum.hop_samples,float(config.sample_rate),decision) ||
       !add_array(bytes,bins,2*sizeof(float)))return false;
    output=bytes;return true;
}
std::size_t PreparedPcmOnsetSequence::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+descriptor_.workspace_bytes()+magnitude_.workspace_bytes()+peaks_.workspace_bytes()+
        (values_.capacity()+phase_.capacity())*sizeof(float);
}
}
