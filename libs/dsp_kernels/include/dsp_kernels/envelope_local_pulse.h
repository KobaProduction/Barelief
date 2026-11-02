// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/local_pulse.h>
#include <dsp_kernels/tempogram_sequence.h>
namespace dsp_kernels {
struct EnvelopeLocalPulseConfig {
    float sample_rate=44100;
    std::size_t hop_samples=512;
    LocalPulseConfig pulse;
};
struct LocalPulseChannelInput {
    std::span<const float> envelope,log_prior;
    FloatMatrixSource log_prior_rows;
};
// Envelope -> complex Fourier tempogram -> source phase selection -> shared
// inverse/overlap-add -> positive/max normalization. No magnitude-only proxy.
class PreparedEnvelopeLocalPulse {
public:
    static bool storage_bytes(std::size_t,EnvelopeLocalPulseConfig,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return fourier_.workspace_bytes()+pulse_.workspace_bytes()+batch_pulse_.capacity()*sizeof(float);}
    bool prepare(std::size_t maximum_frames,EnvelopeLocalPulseConfig={},std::span<const float> coefficients={});
    bool process(std::span<const float>,std::span<float> pulse,std::span<const float> log_prior={},
        FloatMatrixSource log_prior_rows={}) noexcept;
    bool process_channels(std::span<const std::span<const float>>,FloatMatrixBatchDestination,
        std::span<const float> log_prior={}) noexcept;
    bool process_channels(std::span<const LocalPulseChannelInput>,FloatMatrixBatchDestination) noexcept;
    std::span<const float> coordinates_bpm() const noexcept {return fourier_.coordinates_bpm();}
    std::size_t tempogram_frames(std::size_t count) const noexcept {return fourier_.frames(count);}
private:
    bool process_channel(std::span<const float>,std::size_t,FloatMatrixBatchDestination,
        std::span<const float>,FloatMatrixSource) noexcept;
    struct Rows {PreparedTempogramSequence* processor;std::span<const float> envelope;};
    static bool read(const void*,std::size_t,std::span<float>) noexcept;
    PreparedTempogramSequence fourier_;
    PreparedLocalPulse pulse_;
    std::vector<float> batch_pulse_;
    std::size_t maximum_frames_=0;
};
}
