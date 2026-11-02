// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/tempogram_sequence.h>
#include <dsp_kernels/tempo_estimator.h>
namespace dsp_kernels {
struct EnvelopeTempoConfig {
    float sample_rate=44100;
    std::size_t hop_samples=512;
    double autocorrelation_seconds=8; // Cold clock conversion only.
    TempoEstimatorConfig estimator;
    NumericalPrecision precision=NumericalPrecision::PortableFloat32;
};
// Source time_to_frames(ac_size): truncate seconds*Fs to sample coordinates,
// then floor-divide by hop. This helper does not calculate analytical samples.
bool tempo_window_frames(double seconds,float sample_rate,std::size_t hop,std::size_t&) noexcept;
// Complete envelope -> default ACF tempogram -> existing tempo estimator.
// A supplied tempogram remains a direct PreparedTempoEstimator input: it
// overrides ACF geometry instead of being reinterpreted as an envelope.
class PreparedEnvelopeTempo {
public:
    static bool storage_bytes(std::size_t,EnvelopeTempoConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return tempogram_.workspace_bytes()+estimator_.workspace_bytes()+batch_bpm_.capacity()*sizeof(float);}
    bool prepare(std::size_t maximum_frames,EnvelopeTempoConfig={});
    bool process(std::span<const float> envelope,std::span<float> bpm,
        std::span<const float> log_prior={},FloatMatrixReductionPort={}) noexcept;
    bool process_channels(std::span<const std::span<const float>>,FloatMatrixBatchDestination,
        std::span<const float> log_prior={},FloatMatrixReductionPort={}) noexcept;
    std::size_t output_frames(std::size_t envelope_frames,FloatMatrixReductionPort={}) const noexcept;
    std::span<const float> coordinates_bpm() const noexcept {return tempogram_.coordinates_bpm();}
private:
    struct Rows {PreparedTempogramSequence* processor;std::span<const float> envelope;};
    static bool read(const void*,std::size_t,std::span<float>) noexcept;
    PreparedTempogramSequence tempogram_;
    PreparedTempoEstimator estimator_;
    EnvelopeTempoConfig config_;
    std::size_t maximum_frames_=0;
    std::vector<float> batch_bpm_;
};
}
