// SPDX-License-Identifier: ISC
#pragma once
#include <dsp_kernels/pcm_onset_strength.h>
#include <dsp_kernels/onset_detection_sequence.h>
namespace dsp_kernels {
struct PcmOnsetEventsConfig {
    PcmOnsetStrengthConfig strength;
    OnsetDetectionSequenceConfig detection;
};
// Complete PCM onset_detect recipe. The only retained history is the scalar
// envelope needed by offline peak selection/backtracking, not a spectrogram.
class PreparedPcmOnsetEvents {
public:
    bool prepare(std::size_t maximum_samples,PcmOnsetEventsConfig={},std::span<const float> window={});
    bool process(std::span<const float>,OnsetDetectionResult&,
        std::optional<std::span<const float>> energy=std::nullopt) noexcept;
    bool process_channels(std::span<const std::span<const float>>,OnsetDetectionDestination) noexcept;
    std::span<const float> onset_envelope() const noexcept {return std::span(envelope_).first(frames_);}
private:
    static bool write_envelope(void*,std::size_t,std::span<const float>) noexcept;
    PreparedPcmOnsetStrength strength_;
    PreparedOnsetDetectionSequence detection_;
    std::vector<float> envelope_;
    std::size_t frames_=0;
    bool sparse_=true;
};
}
