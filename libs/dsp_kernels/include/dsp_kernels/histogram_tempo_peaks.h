#pragma once
#include <dsp_kernels/curve_interpolation.h>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
struct HistogramTempo {float tempo=0,strength=0;};
// Source dominant_interval: optional shared 'same' convolution, FIRST argmax,
// explicit coordinates. Scratch >= max(bins,kernel); unequal axes are rejected.
bool dominant_histogram_coordinate(std::span<const float> bins,std::span<const float> coordinates,
    std::span<const float> kernel,std::span<float> scratch,float& coordinate) noexcept;
// Source tempo.py detect_tempo: optional quadratic interpolation at .01,
// strict wrapped local maxima, descending heights, normalized peak strengths.
// Coordinates are the explicit histogram axis. frame_rate >0 converts its
// interval coordinates to BPM; zero keeps the coordinates. No peaks returns
// one {NaN,0} candidate, as in the source. Tied heights use descending index.
class PreparedHistogramTempoPeaks {
public:
    static bool storage_bytes(std::size_t bins,float minimum_coordinate,float maximum_coordinate,bool interpolate,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return interpolation_.workspace_bytes()+(grid_.capacity()+bins_.capacity())*sizeof(float)+peaks_.capacity()*sizeof(std::size_t)+result_.capacity()*sizeof(HistogramTempo);}
    bool prepare(std::span<const float> coordinates,float frame_rate=0,bool interpolate=false);
    bool process(std::span<const float> histogram) noexcept;
    std::span<const HistogramTempo> candidates() const noexcept {return std::span(result_).first(count_);}
    std::span<const float> evaluated_histogram() const noexcept {return count_?std::span<const float>(bins_):std::span<const float>{};}
    std::span<const float> evaluated_coordinates() const noexcept {return grid_;}
private:
    bool configure(std::span<const float>,float,bool);
    bool interpolation_grid(std::span<const float>);
    bool load(std::span<const float>) noexcept;
    void find_peaks() noexcept;
    bool publish() noexcept;
    PreparedCurveInterpolation interpolation_;
    std::size_t input_count_=0,count_=0;
    float frame_rate_=0;
    bool interpolate_=false;
    std::vector<float> grid_,bins_;
    std::vector<std::size_t> peaks_;
    std::vector<HistogramTempo> result_;
};
}
