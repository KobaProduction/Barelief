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
};
// Cold source Float32 triangular coefficient construction with explicit edges,
// normalization and ceil/floor support. Consumers receive weights and centers;
// projection, input squaring and logarithms are explicit common graph operations.
// NaturalLogMel preserves the source 1127.01048*ln(1+Hz/700) transformation.
class PreparedTriangularSpectralBands {
public:
    bool prepare(std::size_t bins,std::span<const float> edges_hz,TriangularSpectralBandConfig={});
    // Includes exact simultaneous configure temporaries and nested projection
    // payload. Excludes object headers, allocator overhead and caller stack.
    static bool storage_bytes(std::size_t bins,std::span<const float> edges_hz,TriangularSpectralBandConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return projection_.workspace_bytes();}
    std::size_t bands() const noexcept {return projection_.centers_hz().size();}
    std::span<const float> weights() const noexcept {return projection_.weights();}
    std::span<const double> centers_hz() const noexcept {return projection_.centers_hz();}
private:
    bool configure(std::size_t,std::span<const float>,TriangularSpectralBandConfig);
    bool prepare_projection(std::size_t,std::span<const float>,std::span<const float>,std::span<const dsp_kernels::FilterBandSupport>);
    dsp_kernels::MelFilterBank projection_;
};
// Uniform warped coordinates, source iterative Float32 increment and inverse.
bool warped_band_edges(std::span<float> edges_hz,float low_hz,float high_hz,TriangleWarp) noexcept;
std::span<const float> trajectory_band_edges() noexcept;
}
