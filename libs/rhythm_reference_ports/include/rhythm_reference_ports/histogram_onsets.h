// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct HistogramConfig {std::size_t bins=10;float minimum=0,maximum=3.14159265358979323846f;};
class PreparedHistogram {
public:
    bool prepare(HistogramConfig={});
    static bool storage_bytes(HistogramConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return (counts_.capacity()+centers_.capacity())*sizeof(float);}
    // Input is intentionally rescaled IN PLACE, as in the source histogram.
    // Result is sum(weighted-bin-counts)/number-of-bins, not a sample average.
    bool process(std::span<float>,float& output,bool dynamic=true,bool ignore_zero=true,bool weighted=true) noexcept;
    std::span<const float> counts() const noexcept {return counts_;}
private:
    bool adapt(std::span<const float>,bool dynamic) noexcept;
    void accumulate(std::span<float>,bool ignore_zero) noexcept;
    bool result(bool weighted,float&) noexcept;
    HistogramConfig config_;
    float minimum_=0,maximum_=0,step_=0,scale_=0;
    std::vector<float> counts_,centers_;
};
enum class HistogramOnsetMethod {Phase,WeightedPhase,SpectralDifference};
struct HistogramOnsetConfig {
    HistogramOnsetMethod method=HistogramOnsetMethod::Phase;
    HistogramConfig histogram;
    float magnitude_threshold=.1f;
};
// Source histogram family from Aubio, distinct from mean phase deviation.
// WeightedPhase preserves the source's two histogram/rescaling passes.
class PreparedHistogramOnset {
public:
    bool prepare(std::size_t complex_bins,HistogramOnsetConfig={});
    static bool storage_bytes(std::size_t bins,HistogramOnsetConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {
        return histogram_.workspace_bytes()+(magnitude_.capacity()+phase_.capacity()+previous_magnitude_.capacity()+
            previous_phase_.capacity()+older_phase_.capacity()+values_.capacity())*sizeof(float);
    }
    // Views are valid after successful process only, until the next process.
    std::span<const float> histogram_counts() const noexcept {return histogram_.counts();}
    std::span<const float> rescaled_histogram_values() const noexcept {return values_;}
    void reset() noexcept;
    bool process(std::span<const float> complex,float& output) noexcept;
    bool process_polar(std::span<const float> magnitude,std::span<const float> phase,float& output) noexcept;
    // SpectralDifference consumes magnitude/old magnitude only. No invented
    // phase row, phase validation or Cartesian conversion is needed.
    bool process_magnitude(std::span<const float> magnitude,float& output) noexcept;
private:
    bool polar(std::span<const float>) noexcept;
    bool weighted_phase(float&) noexcept;
    bool evaluate(float& output) noexcept;
    void phase_difference() noexcept;
    void magnitude_difference() noexcept;
    void commit() noexcept;
    HistogramOnsetConfig config_;
    PreparedHistogram histogram_;
    std::vector<float> magnitude_,phase_,previous_magnitude_,previous_phase_,older_phase_,values_;
};
}
