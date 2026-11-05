// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <dsp_kernels/recursive_filter.h>
#include <array>
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
enum class ThresholdStatistic {Median,Mean};
using AdaptiveThresholdFunction=float (*)(std::span<float>) noexcept;
using AdaptivePeakFunction=bool (*)(std::span<const float>,std::size_t) noexcept;
struct AdaptivePeakConfig {
    std::size_t pre=1,post=5;
    float threshold=.1f;
    std::array<float,3> feedforward{.15998789f,.31997577f,.15998789f};
    // Source coefficients are preserved, including its documented historical
    // feedback-coefficient change. A corrected Butterworth is a separate variant.
    std::array<float,3> feedback{1,.23484048f,0};
    ThresholdStatistic statistic=ThresholdStatistic::Median;
    AdaptiveThresholdFunction threshold_function=nullptr;
    AdaptivePeakFunction peak_function=nullptr;
};
struct AdaptivePeakObservation {
    bool peak=false;
    float position=0,thresholded=0,mean=0,median=0;
};
class PreparedAdaptivePeakPicker {
public:
    bool prepare(AdaptivePeakConfig={});
    static bool storage_bytes(AdaptivePeakConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {
        return filter_.storage_bytes()+(history_.capacity()+filtered_.capacity()+scratch_.capacity())*sizeof(float);
    }
    void reset() noexcept;
    bool set_threshold(float) noexcept;
    float threshold() const noexcept {return config_.threshold;}
    void set_threshold_function(AdaptiveThresholdFunction function) noexcept {config_.threshold_function=function;}
    AdaptiveThresholdFunction threshold_function() const noexcept {return config_.threshold_function;}
    void set_peak_function(AdaptivePeakFunction function) noexcept {config_.peak_function=function;}
    AdaptivePeakFunction peak_function() const noexcept {return config_.peak_function;}
    bool process(float novelty,AdaptivePeakObservation&) noexcept;
private:
    bool prepare_filter(const AdaptivePeakConfig&);
    bool filter(std::span<float>) noexcept;
    bool smooth() noexcept;
    float statistic() noexcept;
    void publish(float average,float median,float thresholded,AdaptivePeakObservation&) noexcept;
    AdaptivePeakConfig config_;
    dsp_kernels::PreparedRecursiveFilter filter_;
    std::array<float,3> peak_{};
    std::vector<float> history_,filtered_,scratch_;
};
}
