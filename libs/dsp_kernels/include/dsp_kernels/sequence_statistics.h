#pragma once
#include <cstddef>
#include <span>

namespace dsp_kernels {
enum class Float32Summation {Plain,Compensated};
struct Float32Accumulator {
    float value=0,error=0;
    void add(float input,Float32Summation mode=Float32Summation::Compensated) noexcept {
        if(mode==Float32Summation::Plain){value+=input;return;}
        const float corrected=input-error,next=value+corrected;
        error=(next-value)-corrected;value=next;
    }
};
// Float32 arithmetic: plain sequential summation by default, explicit
// compensated summation where selected; no Float64 accumulator.
// Empty/nonfinite/overflow leaves output unchanged. Deviation uses n-1.
// Regression uses x=0..n-1 and reports (Syy-Sxy*Sxy/Sxx)/n, without
// clamping a negative rounding residual or materializing a fitted sequence.
// Stable log(sum(exp(x))), accepting -inf entries; all -inf/nonfinite fails.
bool log_sum_exp_float32(std::span<const float>,float& output) noexcept;
bool sum_float32(std::span<const float>,float& output,Float32Summation=Float32Summation::Plain,bool allow_nonfinite=false) noexcept;
bool arithmetic_mean_float32(std::span<const float>,float& output,Float32Summation=Float32Summation::Plain) noexcept;
// One pre-scaled Kahan mean contribution. Call with the final sample count on
// every step; sum/correction start at zero. Failure leaves both unchanged.
bool accumulate_scaled_mean_float32(float value,float count,float& sum,float& correction) noexcept;
// Population variance (correction=0) or sample variance (correction=1).
// Mean, centered squares and division keep sequential Float32 source order.
// count_divisor selects float(N-correction), otherwise float(N)-float(correction).
bool variance_float32(std::span<const float>,float& output,std::size_t correction=0,
                      Float32Summation=Float32Summation::Plain,bool count_divisor=false) noexcept;
bool sample_deviation_float32(std::span<const float>,float& output,
                             Float32Summation=Float32Summation::Plain,bool count_divisor=false) noexcept;
bool linear_residual_mean_square(std::span<const float>,float& output) noexcept;
}
