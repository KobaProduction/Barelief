// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>
#include <dsp_kernels/recursive_filter.h>
namespace rhythm_reference_ports {
// Source MovingAverage/IIR's transposed FIR form, with equal coefficients.
// State is explicit and reusable between calls; reset is a separate operation.
// Subnormal state entries are flushed as in the original IIR. No running-sum
// substitution or hidden padding/alignment. Scalar processing commits atomically;
// vector processing consumes successful preceding samples if a later one fails.
class PreparedCausalAverage {
public:
    bool prepare(std::size_t length);
    // Dynamic bound includes the source numerator temporary during cold prepare.
    static bool storage_bytes(std::size_t length,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return filter_.storage_bytes()+coefficients_.capacity()*sizeof(float);}
    // Select a length <= the prepared maximum and reset the FIR, without allocation.
    bool set_length(std::size_t length) noexcept;
    void reset() noexcept;
    bool process(float input,float& output) noexcept;
    bool process(std::span<const float> input,std::span<float> output) noexcept;
    std::size_t length() const noexcept {return filter_.order();}
private:
    dsp_kernels::PreparedRecursiveFilter filter_;
    std::vector<float> coefficients_;
};
}
