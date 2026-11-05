// SPDX-License-Identifier: GPL-3.0-or-later
#include <rhythm_reference_ports/pcm_context_tempo.h>
#include <dsp_kernels/prepared_storage.h>
#include <cmath>

namespace rhythm_reference_ports {
bool PreparedPcmContextTempo::storage_bytes(PcmContextTempoConfig config,std::size_t& output,const dsp_kernels::FourierBackendFactory* factory) noexcept {
    if(!config.sample_rate || config.sample_rate>INT32_MAX || !std::isfinite(config.silence_db) || !config.spectrum.hop_samples)return false;
    auto sequence=config.sequence;sequence.hop_samples=config.spectrum.hop_samples;
    sequence.tracker.frame_rate=float(config.sample_rate)/float(config.spectrum.hop_samples);
    AdaptivePeakConfig peak;peak.threshold=config.threshold;
    std::size_t bytes=0,nested=0;const auto bins=std::size_t(config.spectrum.window_samples)/2+1;
    using namespace dsp_kernels::prepared_storage;
    if(!PreparedHopSpectrum::storage_bytes(config.spectrum,{},bytes,factory) ||
       !PreparedOnsetDescriptor::storage_bytes(bins,config.method,nested) || !add_bytes(bytes,nested) ||
       !PreparedAdaptivePeakPicker::storage_bytes(peak,nested) || !add_bytes(bytes,nested) ||
       !PreparedContextBeatSequence::storage_bytes(sequence,nested) || !add_bytes(bytes,nested) ||
       !add_array(bytes,bins,2*sizeof(float)))return false;
    output=bytes;return true;
}
std::size_t PreparedPcmContextTempo::workspace_bytes() const noexcept {
    return spectrum_.workspace_bytes()+descriptor_.workspace_bytes()+peaks_.workspace_bytes()+sequence_.workspace_bytes()+
        (values_.capacity()+phase_.capacity())*sizeof(float);
}
}
