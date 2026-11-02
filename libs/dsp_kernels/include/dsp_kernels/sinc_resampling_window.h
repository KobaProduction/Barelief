// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <dsp_kernels/resampling_window.h>
#include <span>
#include <vector>
namespace dsp_kernels {
struct SincCoefficientTable {
    std::span<const float> values;
    std::uint32_t increment=0;
};
// Fresh-state, end-of-input, constant-ratio mono sinc convolution from
// libsamplerate 0.2.2. All taps and source 12-bit coefficient indexing remain.
// Table storage is borrowed, immutable, and must outlive this prepared plan.
// Dot products/interpolation are Float32 by default. Explicit DesktopReference
// retains the source's Float32 coefficient difference, Double fraction/product/
// serial sums/scale, and single Float32 output store. Payload buffers stay F32.
class PreparedSincResamplingWindow {
public:
    bool prepare(ResamplingWindowConfig,SincCoefficientTable);
    bool process(std::span<const float> input,std::size_t input_stride,
        std::span<float> output,std::size_t output_stride,std::size_t& written) const noexcept;
    std::size_t generated_samples() const noexcept {return generated_;}
    std::size_t owned_bytes() const noexcept {return positions_.capacity()*sizeof(Position);}
    static bool storage_bytes(const ResamplingWindowConfig&,std::size_t&) noexcept;
private:
    struct Position {std::int64_t center=0,coefficient=0;};
    bool configure(ResamplingWindowConfig,SincCoefficientTable);
    bool plan_positions();
    bool generate_positions(double ratio,double scaled_increment,double step,double terminate);
    bool process_channel(std::span<const float>,std::span<float>) const noexcept;
    float interpolate(std::int64_t) const noexcept;
    float convolve(std::span<const float>,Position) const noexcept;
    float half_sum(std::span<const float>,std::int64_t coefficient,
        std::int64_t sample,int direction) const noexcept;
    double reference_interpolate(std::int64_t) const noexcept;
    double reference_half_sum(std::span<const float>,std::int64_t,std::int64_t,int) const noexcept;
    float reference_convolve(std::span<const float>,Position) const noexcept;
    ResamplingWindowConfig config_;
    SincCoefficientTable table_;
    std::vector<Position> positions_;
    std::int64_t increment_=0,maximum_=0;
    std::size_t generated_=0;
    float scale_=1;
    double reference_scale_=1;
};
}
