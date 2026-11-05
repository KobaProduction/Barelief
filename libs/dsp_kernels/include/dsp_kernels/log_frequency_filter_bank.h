#pragma once
#include <dsp_kernels/filter_projection.h>
#include <dsp_kernels/frequency_bins.h>
#include <dsp_kernels/numerical_precision.h>
#include <span>
#include <vector>
namespace dsp_kernels {
struct LogFrequencyFilterConfig {
    float bands_per_octave=24,fmin=30,fmax=17000,reference=440;
    bool unique_filters=true,normalize=true;
    NumericalPrecision precision=NumericalPrecision::PortableFloat32;
};
// Source port of madmom LogarithmicFilterbank: explicit input frequencies,
// reference-aligned octave grid, nearest-bin corners and overlapping discrete
// triangles. Does not apply a logarithm or infer magnitude/power units.
class LogFrequencyFilterBank {
public:
    bool prepare(std::span<const float> frequencies,LogFrequencyFilterConfig={});
    bool process(std::span<const float>,std::span<float>) const noexcept;
    std::size_t bands() const noexcept {return centers_.size();}
    std::span<const float> centers_hz() const noexcept {return centers_;}
    std::span<const float> weights() const noexcept {return weights_;}
    std::span<const FilterBandSupport> supports() const noexcept {return supports_;}
    // Shared source nearest-frequency rule, ties choose upper; grid is ordered.
    static std::size_t nearest(std::span<const float> grid,double frequency_hz) noexcept;
    static bool band_count(FrequencyGridView,const LogFrequencyFilterConfig&,std::size_t&) noexcept;
private:
    bool configure(std::span<const float>,LogFrequencyFilterConfig);
    bool corners(std::span<const float>,const LogFrequencyFilterConfig&);
    bool triangle(std::span<const float>,std::size_t,const LogFrequencyFilterConfig&);
    std::size_t bins_=0;
    std::vector<std::size_t> corners_;
    std::vector<float> centers_,weights_;
    std::vector<FilterBandSupport> supports_;
    mutable std::vector<float> result_;
};
}
