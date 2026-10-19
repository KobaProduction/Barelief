#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace dsp_kernels {
// Integer phase accumulator shared by live and offline window scheduling.
// The caller owns the remainder, allowing resets/state transfer without allocation.
inline std::size_t next_sample_hop(std::size_t base, std::uint64_t fraction,
                                   std::uint64_t denominator, std::uint64_t& remainder) noexcept {
    if (!denominator || fraction >= denominator || remainder >= denominator) return 0;
    if (remainder < denominator - fraction) { remainder += fraction; return base; }
    remainder -= denominator - fraction;
    return base == std::numeric_limits<std::size_t>::max() ? 0 : base + 1;
}
struct SampleCadence {
    std::size_t base = 0;
    std::uint64_t fraction = 0, denominator = 1, remainder = 0;
    void clear() noexcept {base=0;fraction=0;denominator=1;remainder=0;}
    bool prepare(std::uint32_t rate, std::uint64_t numerator, std::uint64_t divisor) noexcept {
        clear();
        if (!rate || !numerator || !divisor || numerator > UINT64_MAX / rate) return false;
        const auto product = numerator * rate;
        if (!product || product / divisor > SIZE_MAX) return false;
        base = std::size_t(product / divisor); fraction = product % divisor;
        if(base==SIZE_MAX && fraction){base=0;fraction=0;return false;}
        denominator = divisor; remainder = 0;
        return base > 0;
    }
    std::size_t next() noexcept { return next_sample_hop(base, fraction, denominator, remainder); }
};
}
