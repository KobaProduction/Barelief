// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/hop_spectrum.h>
#include <rhythm_reference_ports/onset_descriptor.h>
#include <rhythm_reference_ports/adaptive_peak_picker.h>
#include <rhythm_reference_ports/context_beat_sequence.h>
#include <rhythm_reference_ports/onset_decision.h>
#include <cstdint>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct PcmContextTempoConfig {
    HopSpectrumConfig spectrum;
    std::uint32_t sample_rate=44100;
    OnsetRecipe method=OnsetRecipe::SpectralFlux; // source tempo "default"
    ContextBeatSequenceConfig sequence;
    float threshold=.3f,silence_db=-90;
};
struct PcmContextTempoObservation {
    float descriptor=0;
    AdaptivePeakObservation onset_peak;
    ContextBeatFrame beat;
};
// Complete aubio tempo.c PCM recipe. Unlike onset.c, NO whitening or compression.
// Preserves tracker-boundary-before-picker ordering, thresholded ODF history,
// source tactus (including exact zero), confidence, sample times and controls.
// Tatum polling is explicit: process_hop does not call the mutating source query.
// restart() is an explicit fresh-analysis extension; source tempo has no reset API.
class PreparedPcmContextTempo {
public:
    bool prepare(PcmContextTempoConfig={},const dsp_kernels::FourierBackendFactory* =nullptr);
    static bool storage_bytes(PcmContextTempoConfig,std::size_t&,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept;
    void restart() noexcept;
    bool process_hop(std::span<const float>,PcmContextTempoObservation&) noexcept;
    bool set_silence(float) noexcept;
    bool set_threshold(float value) noexcept {return peaks_.set_threshold(value);}
    bool set_delay_samples(std::int32_t value) noexcept {return sequence_.set_delay_samples(value);}
    bool set_delay_seconds(float) noexcept;
    bool set_delay_milliseconds(float value) noexcept {return set_delay_seconds(value/1000.f);}
    bool set_tatum_signature(unsigned value) noexcept {return prepared_ && sequence_.set_tatum_signature(value);}
    unsigned poll_tatum() noexcept {return sequence_.poll_tatum();}
    unsigned tatum_signature() const noexcept {return sequence_.tatum_signature();}
    float threshold() const noexcept {return peaks_.threshold();}
    float silence_db() const noexcept {return config_.silence_db;}
    std::int32_t delay_samples() const noexcept {return sequence_.delay_samples();}
    std::int64_t last_beat_sample() const noexcept {return sequence_.last_beat_sample();}
    std::int64_t last_tatum_sample() const noexcept {return sequence_.last_tatum_sample();}
    float delay_seconds() const noexcept {return float(delay_samples())/float(config_.sample_rate);}
    float delay_milliseconds() const noexcept {return delay_seconds()*1000.f;}
    float last_beat_milliseconds() const noexcept {return last_beat_seconds()*1000.f;}
    float last_tatum_sample_float() const noexcept {return float(last_tatum_sample());}
    float last_beat_seconds() const noexcept {return float(last_beat_sample())/float(config_.sample_rate);}
    float period_samples() const noexcept {return sequence_.period_samples();}
    float period_seconds() const noexcept {return period_samples()/float(config_.sample_rate);}
    float bpm() const noexcept {return sequence_.tracker().bpm();}
    float confidence() const noexcept {return sequence_.tracker().confidence();}
    std::span<const float> raw_magnitude() const noexcept {return values_;}
    std::span<const float> phase() const noexcept {return phase_;}
    std::span<const float> onset_window() const noexcept {return sequence_.onset_window();}
    const PreparedHopSpectrum& spectrum_stage() const noexcept {return spectrum_;}
    const PreparedContextBeatSequence& sequence_stage() const noexcept {return sequence_;}
private:
    bool configure(PcmContextTempoConfig,const dsp_kernels::FourierBackendFactory*);
    PcmContextTempoConfig config_;
    PreparedHopSpectrum spectrum_;
    PreparedOnsetDescriptor descriptor_;
    PreparedAdaptivePeakPicker peaks_;
    PreparedContextBeatSequence sequence_;
    std::vector<float> values_,phase_;
    bool prepared_=false;
};
}
