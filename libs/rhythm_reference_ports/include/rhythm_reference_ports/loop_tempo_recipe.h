// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/pulse_tempo_recipe.h>
#include <rhythm_reference_ports/loop_length_confidence.h>
#include <span>

namespace rhythm_reference_ports {
// Source LoopBpmEstimator: default Percival (44100Hz) -> rounded BPM ->
// default LoopBpmConfidence -> >= threshold decision. Its only original
// configuration parameter is confidence_threshold; the component APIs remain
// independently configurable. Whole-buffer input is analyzed afresh per call.
class PreparedLoopTempoRecipe {
public:
    // Checked owned dynamic cold-peak bound, excluding instance and allocator overhead.
    static bool storage_bytes(std::size_t maximum,float threshold,std::size_t& bytes,
                              dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32,const dsp_kernels::FourierBackendFactory* =nullptr) noexcept;
    std::size_t workspace_bytes() const noexcept {return tempo_.workspace_bytes();}
    const PreparedPulseTempoRecipe& tempo() const noexcept {return tempo_;}
    bool prepare(std::size_t maximum_pcm_samples,float confidence_threshold=.95f,
                 dsp_kernels::NumericalPrecision window_precision=dsp_kernels::NumericalPrecision::PortableFloat32,const dsp_kernels::FourierBackendFactory* =nullptr);
    bool process(std::span<const float> pcm) noexcept;
    void reset() noexcept;
    float bpm() const noexcept {return bpm_;}
    float raw_bpm() const noexcept {return tempo_.result().bpm;}
    const LoopLengthConfidenceResult& confidence() const noexcept {return confidence_result_;}
private:
    float threshold_=.95f,bpm_=0;
    PreparedPulseTempoRecipe tempo_;
    PreparedLoopLengthConfidence confidence_;
    LoopLengthConfidenceResult confidence_result_;
};
}
