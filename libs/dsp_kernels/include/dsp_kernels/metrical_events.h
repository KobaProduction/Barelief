#pragma once
#include <cstdint>
#include <span>

namespace dsp_kernels {
// Source beat-number change events, common to DBNDownBeat(correct=false) and
// PatternTracking. No first/last extrapolation. Explicit state positions/path;
// output buffers each hold at least path.size() entries, count is actual size.
bool metrical_change_events(std::span<const std::uint32_t> path,std::span<const float> positions,
                             std::span<std::uint32_t> frame_indices,std::span<std::uint32_t> beat_numbers,
                             std::size_t& count) noexcept;
}
