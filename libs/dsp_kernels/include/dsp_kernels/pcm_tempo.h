// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/pcm_onset_envelope.h>
#include <dsp_kernels/envelope_tempo.h>
namespace dsp_kernels {
struct PcmTempogramConfig {
    PcmOnsetStrengthConfig onset;
    TempogramSequenceConfig tempogram;
};
class PreparedPcmTempogram {
public:
    static bool storage_bytes(std::size_t,PcmTempogramConfig,std::span<const float>,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return onset_.workspace_bytes()+tempogram_.workspace_bytes()+row_.capacity()*sizeof(float);}
    bool prepare(std::size_t maximum_samples,PcmTempogramConfig={},
        std::span<const float> spectral_window={},std::span<const float> temporal_window={});
    bool process(std::span<const float> pcm,FloatMatrixDestination) noexcept;
    bool process_channels(std::span<const std::span<const float>>,FloatMatrixBatchDestination) noexcept;
    std::span<const float> onset_envelope() const noexcept {return onset_.values();}
    std::span<const float> coordinates_bpm() const noexcept {return tempogram_.coordinates_bpm();}
    std::size_t columns() const noexcept {return tempogram_.columns();}
private:
    bool process_channel(std::span<const float>,std::size_t,FloatMatrixBatchDestination) noexcept;
    PreparedPcmOnsetEnvelope onset_;
    PreparedTempogramSequence tempogram_;
    std::vector<float> row_;
};
struct PcmTempoConfig {
    PcmOnsetStrengthConfig onset;
    EnvelopeTempoConfig tempo;
};
class PreparedPcmTempo {
public:
    static bool storage_bytes(std::size_t,PcmTempoConfig,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return onset_.workspace_bytes()+tempo_.workspace_bytes()+batch_bpm_.capacity()*sizeof(float);}
    bool prepare(std::size_t maximum_samples,PcmTempoConfig={},std::span<const float> spectral_window={});
    bool process(std::span<const float> pcm,std::span<float> bpm,
        std::span<const float> log_prior={},FloatMatrixReductionPort={}) noexcept;
    bool process_channels(std::span<const std::span<const float>>,FloatMatrixBatchDestination,
        std::span<const float> log_prior={},FloatMatrixReductionPort={}) noexcept;
    std::size_t output_frames(std::size_t pcm_samples,FloatMatrixReductionPort reduction={}) const noexcept {
        return tempo_.output_frames(onset_.frames(pcm_samples),reduction);
    }
    std::span<const float> onset_envelope() const noexcept {return onset_.values();}
private:
    PreparedPcmOnsetEnvelope onset_;
    PreparedEnvelopeTempo tempo_;
    std::vector<float> batch_bpm_;
};
}
