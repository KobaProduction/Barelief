#pragma once
// Internal measurement adapter, intentionally outside the installed API.
// Stage the original finite input OUTSIDE the core timer on every iteration.
// Never repeatedly FFT a previous output: that changes values and overflows.
#include "dsp_kernels/dsp.h"
#include "backend.h"
#include <algorithm>
namespace dsp_kernels::detail {
struct FftBenchmarkAccess {
    static void stage(FFT& plan, std::span<const float> input) noexcept {
        for(std::size_t i=0;i<input.size();++i) {
            plan.scratch_[2*i]=input[i]; plan.scratch_[2*i+1]=0;
        }
    }
    static bool core(FFT& plan) noexcept {
        if(!transform(plan.scratch_.data(),plan.samples(),plan.radix4_)) return false;
        // Match the real-input publication contract's exact real edge bins.
        plan.scratch_[1]=0;plan.scratch_[plan.samples()+1]=0;
        return true;
    }
    static void stage(PreparedRfft& plan, std::span<const float> input) noexcept {
        const auto n=plan.samples_;
        if(plan.mixed_radix_) {
            for(std::size_t i=0;i<n/4;++i) {
                plan.mixed_[2*i]=input[4*i];
                plan.mixed_[2*i+1]=input[4*i+1];
                plan.mixed_[n/2+2*i]=input[4*i+2];
                plan.mixed_[n/2+2*i+1]=input[4*i+3];
            }
        } else std::copy(input.begin(),input.end(),plan.scratch_.data());
    }
    static bool core(PreparedRfft& plan) noexcept { return plan.transform_prepared(); }
    static std::span<const float> output(const FFT& plan) noexcept {
        return {plan.scratch_.data(),plan.scratch_.size()};
    }
    static std::span<const float> output(const PreparedRfft& plan) noexcept {
        return {plan.scratch_.data(),plan.scratch_.size()};
    }
};
}
