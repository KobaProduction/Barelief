// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/tempogram_sequence.h>
#include <dsp_kernels/tempo_ratio_sequence.h>
#include <dsp_kernels/pcm_onset_envelope.h>
namespace dsp_kernels {
struct EnvelopeTempoRatiosConfig {
    TempogramSequenceConfig tempogram;
    TempoRatioSequenceConfig ratios;
};
struct EnvelopeTempoRatioChannelInput {
    std::span<const float> envelope;
    std::optional<std::span<const float>> bpm;
    std::span<const float> log_prior;
    FloatMatrixSource frequencies;
    FloatMatrixReductionPort reduction;
};
// Source envelope -> ACF -> tempo/f0_harmonics recipe. External arbitrary
// tempograms bypass this frontend and use PreparedTempoRatioSequence directly.
class PreparedEnvelopeTempoRatios {
public:
    static bool storage_bytes(std::size_t,EnvelopeTempoRatiosConfig,std::span<const float>,std::span<const float>,std::optional<std::span<const float>>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return tempogram_.workspace_bytes()+ratios_.workspace_bytes();}
    bool prepare(std::size_t maximum_frames,EnvelopeTempoRatiosConfig={},
        std::span<const float> coefficients={},std::span<const float> coordinates={},
        std::optional<std::span<const float>> factors=std::nullopt);
    bool process(std::span<const float>,FloatMatrixDestination,
        std::optional<std::span<const float>> bpm=std::nullopt,std::span<const float> log_prior={},
        FloatMatrixSource frequencies={},FloatMatrixReductionPort={}) noexcept;
    // Independent channels share prepared arithmetic/storage. All per-channel
    // views are borrowed for this call only; the sink consumes rows immediately.
    // A failed channel may follow an already written prefix of earlier channels.
    bool process_channels(std::span<const EnvelopeTempoRatioChannelInput>,FloatMatrixBatchDestination) noexcept;
    std::size_t output_frames(std::size_t count) const noexcept {return ratios_.output_frames(tempogram_.frames(count));}
    std::size_t factors() const noexcept {return ratios_.factors();}
private:
    struct Rows {PreparedTempogramSequence* processor;std::span<const float> envelope;};
    static bool read(const void*,std::size_t,std::span<float>) noexcept;
    PreparedTempogramSequence tempogram_;
    PreparedTempoRatioSequence ratios_;
    std::size_t maximum_frames_=0;
};
struct PcmTempoRatiosConfig {
    PcmOnsetStrengthConfig onset;
    EnvelopeTempoRatiosConfig ratios;
};
struct PcmTempoRatioChannelInput {
    std::span<const float> pcm;
    std::optional<std::span<const float>> bpm;
    std::span<const float> log_prior;
    FloatMatrixSource frequencies;
    FloatMatrixReductionPort reduction;
};
// PCM channels are processed independently through the existing scalar onset
// frontend and envelope-ratio recipe. Only the current scalar history is owned;
// no batch spectrogram/tempogram is materialized and no input view is retained.
class PreparedPcmTempoRatios {
public:
    static bool storage_bytes(std::size_t,PcmTempoRatiosConfig,std::span<const float>,std::span<const float>,std::span<const float>,std::optional<std::span<const float>>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return onset_.workspace_bytes()+ratios_.workspace_bytes();}
    bool prepare(std::size_t maximum_samples,PcmTempoRatiosConfig={},
        std::span<const float> spectral_window={},std::span<const float> temporal_window={},
        std::span<const float> coordinates={},std::optional<std::span<const float>> factors=std::nullopt);
    bool process(std::span<const float> pcm,FloatMatrixDestination,
        std::optional<std::span<const float>> bpm=std::nullopt,std::span<const float> log_prior={},
        FloatMatrixSource frequencies={},FloatMatrixReductionPort={}) noexcept;
    bool process_channels(std::span<const PcmTempoRatioChannelInput>,FloatMatrixBatchDestination) noexcept;
    std::size_t output_frames(std::size_t count) const noexcept {return ratios_.output_frames(onset_.frames(count));}
    std::size_t factors() const noexcept {return ratios_.factors();}
    std::span<const float> onset_envelope() const noexcept {return onset_.values();}
private:
    PreparedPcmOnsetEnvelope onset_;
    PreparedEnvelopeTempoRatios ratios_;
};
}
