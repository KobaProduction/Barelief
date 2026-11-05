// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <span>
#include <vector>

namespace rhythm_reference_ports {
enum class BufferedMaximumReset {SourceReset,ConfiguredStart};
// Source MaxFilter chunk/state semantics, distinct from a reflected centered
// max-filter. The source 'causal=false' only changes initial fill offset; it
// does not shift output. State persists across vectors, including band rows.
// Partial-buffer reseeding retains the source ternary-precedence expression.
// reset() follows source fill-index0, ConfiguredStart restores configure offset.
// The source can access an uninitialized buffer after noncausal reset; this
// port always seeds prepared storage before first access (explicit safe repair).
class PreparedBufferedMaximum {
public:
    bool prepare(std::size_t width=3,bool causal=true);
    static bool storage_bytes(std::size_t width,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return buffer_.capacity()*sizeof(float);}
    void reset(BufferedMaximumReset=BufferedMaximumReset::SourceReset) noexcept;
    bool process(std::span<const float>,std::span<float>) noexcept;
private:
    void seed(float) noexcept;
    std::size_t fill(std::span<const float>,std::span<float>) noexcept;
    std::size_t width_=0,index_=0;
    float current_maximum_=0;
    bool causal_=true,filled_=false,seeded_=false;
    std::vector<float> buffer_;
};
}
