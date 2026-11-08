#pragma once
#include <cstddef>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class SubnormalStatePolicy {Preserve,FlushAll,FlushAfterFirst};
enum class RecursiveFilterForm {TransposedDirectII,DirectI};
struct RecursiveFilterOptions {
    RecursiveFilterForm form=RecursiveFilterForm::TransposedDirectII;
    float input_zero_threshold=0; // abs(input) strictly below this value becomes zero.
};
// Prepared Float32 difference equation. Direct I preserves separate += b*x and
// -= a*y operations, with independent input/output histories. Coefficients
// are explicit, normalized by a[0] during cold preparation, and zero-extended.
// Reconfiguration keeps delay state only when requested and the order matches.
// Scalar processing commits state/output atomically. A vector consumes its
// successful prefix on arithmetic overflow. Exact in-place buffers are allowed.
class PreparedRecursiveFilter {
public:
    bool prepare(std::span<const float> numerator,std::span<const float> denominator,
                 SubnormalStatePolicy=SubnormalStatePolicy::FlushAll,RecursiveFilterOptions={});
    bool reconfigure(std::span<const float> numerator,std::span<const float> denominator,
                     bool preserve_state=true);
    // Warm coefficient binding within prepared storage; no allocation. State is
    // retained only when requested and the active coefficient count is unchanged.
    bool set_coefficients(std::span<const float> numerator,std::span<const float> denominator,
                          bool preserve_state=true) noexcept;
    void reset() noexcept;
    // Bind an explicitly supplied delay state, atomically, without allocation.
    bool set_state(std::span<const float>) noexcept;
    bool process(float input,float& output) noexcept;
    bool process(std::span<const float> input,std::span<float> output) noexcept;
    std::size_t order() const noexcept {return active_size_;}
    // Direct I returns [input history][output history], each newest first and
    // order() entries long. Transposed II keeps its original order()-entry view.
    std::span<const float> state() const noexcept {return std::span(state_).first(state_count());}
    // Actual owned dynamic capacities; instance header is excluded.
    std::size_t storage_bytes() const noexcept;
    static bool storage_bytes(std::size_t numerator_count,std::size_t denominator_count,
                              RecursiveFilterOptions,std::size_t& bytes) noexcept;
private:
    bool coefficients(std::span<const float>,std::span<const float>);
    bool advance(float input,float output) noexcept;
    float next_state(std::size_t,float input,float output) const noexcept;
    float clean(float,std::size_t) const noexcept;
    bool copy_coefficients(std::span<const float>,std::span<const float>) noexcept;
    std::size_t state_count() const noexcept;
    bool direct(float input,float& output) noexcept;
    void advance_direct(float input,float output) noexcept;
    std::size_t numerator_size_=0,denominator_size_=0,active_size_=0;
    SubnormalStatePolicy cleanup_=SubnormalStatePolicy::FlushAll;
    RecursiveFilterOptions options_;
    std::vector<float> numerator_,denominator_,state_,pending_;
};
}
