// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/context_beat_tracker.h>
#include <rhythm_reference_ports/adaptive_peak_picker.h>
#include <dsp_kernels/correlation.h>
#include <dsp_kernels/recursive_filter.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace aubio_test {
struct PeakInputs {
    std::array<float,7> history{},smoothed{},backward{};
    dsp_kernels::PreparedRecursiveFilter filter;
    bool prepare() {
        dsp_kernels::RecursiveFilterOptions options;options.form=dsp_kernels::RecursiveFilterForm::DirectI;
        options.input_zero_threshold=std::nextafter(2e-42f,std::numeric_limits<float>::infinity());
        return filter.prepare(rhythm_reference_ports::adaptive_peak_numerator,rhythm_reference_ports::adaptive_peak_denominator,
            dsp_kernels::SubnormalStatePolicy::Preserve,options);
    }
    bool pass(std::span<float> row) noexcept {filter.reset();return filter.process(row,row);}
    bool push(float value) noexcept {
        std::move(history.begin()+1,history.end(),history.begin());history.back()=value;
        smoothed=history;if(!pass(smoothed))return false;
        std::reverse_copy(smoothed.begin(),smoothed.end(),backward.begin());if(!pass(backward))return false;
        std::reverse_copy(backward.begin(),backward.end(),smoothed.begin());return true;
    }
};
struct ContextInputs {
    std::array<float,513> history{};
    std::array<float,512> weights{},reversed{},acf{};
    bool prepare() noexcept {return rhythm_reference_ports::context_detection_weights({},weights);}
    bool analyze(std::span<const float> input) noexcept {
        if(input.size()!=weights.size())return false;
        for(std::size_t i=0;i<input.size();++i)reversed[input.size()-1-i]=input[i]*weights[i];
        return dsp_kernels::linear_autocorrelation(input,acf,dsp_kernels::CorrelationNormalization::Unbiased);
    }
    bool push(float value) noexcept {
        std::move(history.begin()+1,history.end(),history.begin());history.back()=value;
        return analyze(std::span<const float>(history).first(512));
    }
};
}
