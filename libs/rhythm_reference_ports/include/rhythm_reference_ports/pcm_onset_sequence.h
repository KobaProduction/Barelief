// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/hop_spectrum.h>
#include <rhythm_reference_ports/onset_descriptor.h>
#include <rhythm_reference_ports/onset_magnitude.h>
#include <rhythm_reference_ports/adaptive_peak_picker.h>
#include <rhythm_reference_ports/onset_decision.h>
#include <cstdint>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct PcmOnsetSequenceConfig {
    HopSpectrumConfig spectrum;
    std::uint32_t sample_rate=44100;
    OnsetRecipe method=OnsetRecipe::FrequencyContent; // source onset "default"
};
struct PcmOnsetObservation {
    float descriptor=0;
    AdaptivePeakObservation peak;
    OnsetDecision onset;
    bool silent=false;
};
// Complete aubio onset.c recipe for a fixed PCM hop. Numerical stages remain
// independently usable. Controls do not reprepare descriptors or erase history.
// reset() matches aubio_onset_reset: CLOCKS ONLY. restart() explicitly clears
// all analytical state while preserving current controls.
// apply_default_parameters(profile) changes controls, NOT the descriptor method,
// just like source. Non-SpectralFlux profiles leave whitening floor/relax intact.
class PreparedPcmOnsetSequence {
public:
    bool prepare(PcmOnsetSequenceConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool storage_bytes(PcmOnsetSequenceConfig,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    void reset() noexcept;
    void restart() noexcept;
    bool process_hop(std::span<const float>,PcmOnsetObservation&) noexcept;
    bool apply_default_parameters(OnsetRecipe) noexcept;
    bool set_silence(float db) noexcept;
    bool set_threshold(float value) noexcept {return peaks_.set_threshold(value);}
    bool set_whitening(bool value) noexcept {return magnitude_.set_whitening(value);}
    bool set_compression(float value) noexcept {return magnitude_.set_compression(value);}
    bool set_delay_samples(std::uint64_t value) noexcept {return decision_.set_delay_samples(value);}
    bool set_minimum_interval_samples(std::uint64_t value) noexcept {return decision_.set_minimum_interval_samples(value);}
    bool set_delay_seconds(float) noexcept;
    bool set_delay_milliseconds(float value) noexcept {return set_delay_seconds(value/1000.f);}
    bool set_minimum_interval_seconds(float) noexcept;
    bool set_minimum_interval_milliseconds(float value) noexcept {return set_minimum_interval_seconds(value/1000.f);}
    float threshold() const noexcept {return peaks_.threshold();}
    float silence_db() const noexcept {return silence_db_;}
    bool whitening_enabled() const noexcept {return magnitude_.whitening_enabled();}
    float compression() const noexcept {return magnitude_.compression();}
    std::uint64_t delay_samples() const noexcept {return decision_.delay_samples();}
    std::uint64_t minimum_interval_samples() const noexcept {return decision_.minimum_interval_samples();}
    std::uint64_t last_onset_sample() const noexcept {return decision_.last_sample();}
    OnsetRecipe method() const noexcept {return config_.method;}
    float last_onset_milliseconds() const noexcept {return last_onset_seconds()*1000.f;}
    float delay_milliseconds() const noexcept {return delay_seconds()*1000.f;}
    float minimum_interval_milliseconds() const noexcept {return minimum_interval_seconds()*1000.f;}
    float last_onset_seconds() const noexcept {return float(last_onset_sample())/float(config_.sample_rate);}
    float delay_seconds() const noexcept {return float(delay_samples())/float(config_.sample_rate);}
    float minimum_interval_seconds() const noexcept {return float(minimum_interval_samples())/float(config_.sample_rate);}
    float descriptor() const noexcept {return descriptor_value_;}
    float thresholded_descriptor() const noexcept {return thresholded_value_;}
    std::span<const float> processed_magnitude() const noexcept {return values_;}
    std::span<const float> phase() const noexcept {return phase_;}
    std::span<const float> whitening_peaks() const noexcept {return magnitude_.whitening_peaks();}
    const PreparedHopSpectrum& spectrum_stage() const noexcept {return spectrum_;}
private:
    bool configure(PcmOnsetSequenceConfig,const dsp_kernels::FourierBackendFactory*);
    bool prepare_components(const dsp_kernels::FourierBackendFactory*);
    PcmOnsetSequenceConfig config_;
    PreparedHopSpectrum spectrum_;
    PreparedOnsetDescriptor descriptor_;
    PreparedOnsetMagnitude magnitude_;
    PreparedAdaptivePeakPicker peaks_;
    PreparedOnsetDecision decision_;
    std::vector<float> values_,phase_;
    float silence_db_=-70,descriptor_value_=0,thresholded_value_=0;
    bool prepared_=false;
};
}
