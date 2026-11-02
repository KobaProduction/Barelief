#pragma once
#include <dsp_kernels/complex_values.h>
#include <cstddef>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class SpectralDescriptor {Flux,ComplexDifference,PhaseDeviation,HighFrequencyContent,
    SquaredFlux,KullbackLeibler,ModifiedKullbackLeibler,NormalizedWeightedPhaseDeviation,SpectralEnergy,HighFrequencyDifference,L2Flux};
enum class SpectralAggregation {MethodDefault,Sum,Mean};
enum class ComplexDistanceMode {RotatedHypot,SquaredCosine,SquaredCosineRaw};
enum class SpectralDivergenceArithmetic {Float32,Float64RatioFloat32Log};
struct SpectralDescriptorConfig {
    SpectralDescriptor method=SpectralDescriptor::Flux;
    bool half_wave=true,weighted=false,squared=false;
    float magnitude_threshold=0;
    float epsilon=2.220446049250313e-16f,bin_offset=1;
    SpectralAggregation aggregation=SpectralAggregation::MethodDefault;
    std::size_t source_fft_size=0,warmup_frames=0;
    bool mirror_spectrum=false,strict_half_wave=false,first_prediction_zero=false,magnitude_gate=true;
    float frequency_step=1,frequency_exponent=1;
    bool compensated_sum=true;
    bool allow_signed_magnitude=false,stable_log1p=true,normalized_phase_mean=false;
    ComplexDistanceMode complex_distance=ComplexDistanceMode::RotatedHypot;
    bool update_magnitude_history=true,update_phase_history=true;
    // Explicit KL/MKL argument evaluation only. The second mode evaluates
    // 1+current/(previous+ratio_epsilon) in Double, stores one Float32 logf
    // argument, then retains Float32 logarithm/product/reduction/state.
    SpectralDivergenceArithmetic divergence_arithmetic=SpectralDivergenceArithmetic::Float32;
    double ratio_epsilon=0;
    bool operator==(const SpectralDescriptorConfig&) const=default;
};
// One complex [bin][Re,Im] frame, including exactly the bins supplied by the
// caller. No FFT, normalization, logarithm, peak selection or timing inference.
// Flux: sum positive (or absolute) magnitude differences. SquaredFlux sums
// squared differences; L2Flux takes their square root after aggregation
// (Sum gives the L2 norm, Mean gives RMS), optionally half-wave rectified.
// Complex: sum Euclidean distances from magnitude[n-1], phase=2*p[n-1]-p[n-2];
// half_wave discards bins whose magnitude decreased.
// Independent implementation of Duxbury et al., DAFx03, equations 8-11:
// https://www.dafx.de/paper-archive/2003/pdfs/dafx81.pdf
// Phase: mean absolute wrapped second phase difference, optionally magnitude
// weighted; magnitude_gate enables the explicit magnitude_threshold.
// Disable the gate for the original ungated phase-deviation operation. This is not aubio's histogram variant.
// HFC: sum ((k+bin_offset)*frequency_step)^frequency_exponent times
// magnitude[k], or magnitude[k]^2 when squared. Exponent2 gives the Jensen
// weighting; Hz-based variants receive an explicit frequency_step.
// HFD: weighted magnitude difference, positive when half_wave, otherwise absolute.
// History starts at zero. Reset on source discontinuity. Float32 arithmetic by
// default; the explicit divergence argument policy above is the only exception.
// fixed prepared storage; failed prepare/process leaves history/output intact.
class PreparedSpectralDescriptor {
public:
    bool prepare(std::size_t bins,const SpectralDescriptorConfig& config={});
    // Warm operation selection; retains both histories and allocated geometry.
    bool set_parameters(const SpectralDescriptorConfig&) noexcept;
    void reset() noexcept;
    bool process(std::span<const float> complex,float& output) noexcept;
    // Optional reference controls the magnitude-rise gate independently from
    // the n-1 complex predictor. It is never used as the phase/magnitude state.
    bool process_polar(std::span<const float> magnitude,std::span<const float> phase,float& output,
                       std::span<const float> reference={}) noexcept;
    // Values-only descriptors (flux/energy/HFC/HFD/KL/MKL) do not require
    // phase. A supplied reference is the explicit delayed values vector;
    // otherwise the preceding frame is used. Signed log features are accepted.
    bool process_values(std::span<const float> values,float& output,std::span<const float> reference={}) noexcept;
    bool copy_state_from(const PreparedSpectralDescriptor&) noexcept;
    static bool storage_bytes(std::size_t bins,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept {
        return (magnitude_.capacity()+phase_.capacity()+previous_magnitude_.capacity()+previous_phase_.capacity()+older_phase_.capacity())*sizeof(float);
    }
private:
    bool polar(std::span<const float>) noexcept;
    bool values_only() const noexcept;
    bool evaluate(float& output,std::span<const float> reference={}) noexcept;
    float contribution(std::size_t bin,float reference) const noexcept;
    float phase_deviation(std::size_t bin) const noexcept;
    float divergence(std::size_t bin,float reference) const noexcept;
    float complex_difference(std::size_t bin,float gate_reference) const noexcept;
    float high_frequency(std::size_t bin,float reference) const noexcept;
    float frequency_weight(std::size_t bin) const noexcept;
    float phase_value(std::size_t bin) const noexcept;
    float finish(float sum) const noexcept;
    float multiplicity(std::size_t bin) const noexcept;
    void commit() noexcept;
    SpectralDescriptorConfig config_;
    std::size_t processed_frames_=0;
    std::vector<float> magnitude_,phase_,previous_magnitude_,previous_phase_,older_phase_;
};
}
