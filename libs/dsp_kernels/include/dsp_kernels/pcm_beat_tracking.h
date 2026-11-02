// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/envelope_beat_tracking.h>
#include <dsp_kernels/envelope_local_pulse.h>
#include <dsp_kernels/pcm_onset_envelope.h>
namespace dsp_kernels {
struct PcmBeatTrackingConfig {
    PcmOnsetStrengthConfig onset=median_onset_parameters();
    EnvelopeBeatTrackingConfig beats;
    std::size_t maximum_channels=1;
};
struct PcmBeatTrackingInput {
    std::span<const float> pcm;
    std::optional<std::span<const float>> bpm;
    std::span<const float> log_prior;
};
// Full source PCM beat_track: median onset -> optional tempo -> shared DP.
// Batch mode materializes only its required scalar onset tensor and invokes
// the shared global-any batch rule. Borrowed BPM/prior views are cleared at exit.
class PreparedPcmBeatTracking {
public:
    static bool storage_bytes(std::size_t,PcmBeatTrackingConfig,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return onset_.workspace_bytes()+beats_.workspace_bytes()+batch_envelopes_.capacity()*sizeof(float)+batch_inputs_.capacity()*sizeof(BeatTrackingChannelInput);}
    bool prepare(std::size_t maximum_samples,PcmBeatTrackingConfig={},
        std::span<const float> spectral_window={});
    bool process(std::span<const float>,BeatTrackingSequenceResult&,
        std::optional<std::span<const float>> bpm=std::nullopt,std::span<const float> log_prior={}) noexcept;
    bool process_channels(std::span<const PcmBeatTrackingInput>,BeatTrackingDestination) noexcept;
    std::span<const float> onset_envelope() const noexcept {return onset_.values();}
private:
    PreparedPcmOnsetEnvelope onset_;
    PreparedEnvelopeBeatTracking beats_;
    std::size_t maximum_frames_=0;
    std::vector<float> batch_envelopes_;
    std::vector<BeatTrackingChannelInput> batch_inputs_;
};
struct PcmLocalPulseConfig {
    PcmOnsetStrengthConfig onset=median_onset_parameters();
    EnvelopeLocalPulseConfig pulse;
};
struct PcmLocalPulseInput {
    std::span<const float> pcm,log_prior;
    FloatMatrixSource log_prior_rows;
};
class PreparedPcmLocalPulse {
public:
    static bool storage_bytes(std::size_t,PcmLocalPulseConfig,std::span<const float>,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return onset_.workspace_bytes()+pulse_.workspace_bytes()+batch_pulse_.capacity()*sizeof(float);}
    bool prepare(std::size_t maximum_samples,PcmLocalPulseConfig={},
        std::span<const float> spectral_window={},std::span<const float> temporal_window={});
    bool process(std::span<const float> pcm,std::span<float> pulse,std::span<const float> log_prior={},
        FloatMatrixSource log_prior_rows={}) noexcept;
    bool process_channels(std::span<const std::span<const float>>,FloatMatrixBatchDestination,
        std::span<const float> log_prior={}) noexcept;
    bool process_channels(std::span<const PcmLocalPulseInput>,FloatMatrixBatchDestination) noexcept;
    std::size_t output_frames(std::size_t samples) const noexcept {return onset_.frames(samples);}
    std::span<const float> onset_envelope() const noexcept {return onset_.values();}
    std::span<const float> coordinates_bpm() const noexcept {return pulse_.coordinates_bpm();}
    std::size_t tempogram_frames(std::size_t samples) const noexcept {return pulse_.tempogram_frames(onset_.frames(samples));}
private:
    PreparedPcmOnsetEnvelope onset_;
    bool process_channel(std::span<const float>,std::size_t,FloatMatrixBatchDestination,
        std::span<const float>,FloatMatrixSource) noexcept;
    PreparedEnvelopeLocalPulse pulse_;
    std::vector<float> batch_pulse_;
};
}
