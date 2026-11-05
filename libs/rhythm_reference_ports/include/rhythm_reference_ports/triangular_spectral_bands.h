// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/mel_filter_bank.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
enum class TriangleWarp {Linear,NaturalLogMel,Log10Mel};
enum class TriangleNormalization {UnitSum,UnitTriangle,UnitMaximum};
struct TriangularSpectralBandConfig {
    float sample_rate=44100;
    TriangleWarp weighting=TriangleWarp::Linear;
    TriangleNormalization normalization=TriangleNormalization::UnitSum;
    bool square_magnitude=true,logarithm=true;
};
// Source Float32 filter construction with explicit edges and ceil/floor bin
// support. Its "slaneyMel" option actually uses 1127.01048*ln(1+Hz/700),
// NOT Slaney's piecewise scale; NaturalLogMel names the real transformation.
// Uses the EXISTING MelFilterBank's generic explicit-weight projection for
// source plain accumulation and optional magnitude squaring. log2(1+bands)
// uses the common logarithmic operation. Input is raw linear magnitude.
class PreparedTriangularSpectralBands {
public:
    bool prepare(std::size_t bins,std::span<const float> edges_hz,TriangularSpectralBandConfig={});
    // Includes exact simultaneous configure temporaries and nested projection
    // payload. Excludes object headers, allocator overhead and caller stack.
    static bool storage_bytes(std::size_t bins,std::span<const float> edges_hz,TriangularSpectralBandConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return projection_.workspace_bytes()+result_.capacity()*sizeof(float);}
    bool process(std::span<const float> magnitude,std::span<float> output) noexcept;
    std::size_t bands() const noexcept {return result_.size();}
    std::span<const float> weights() const noexcept {return projection_.weights();}
    std::span<const double> centers_hz() const noexcept {return projection_.centers_hz();}
private:
    bool configure(std::size_t,std::span<const float>,TriangularSpectralBandConfig);
    dsp_kernels::MelFilterBank projection_;
    bool logarithm_=true;
    std::vector<float> result_;
};
// Uniform warped coordinates, source iterative Float32 increment and inverse.
bool warped_band_edges(std::span<float> edges_hz,float low_hz,float high_hz,TriangleWarp) noexcept;
std::span<const float> trajectory_band_edges() noexcept;
}
