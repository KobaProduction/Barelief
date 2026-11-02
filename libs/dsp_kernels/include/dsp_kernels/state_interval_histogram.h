#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace dsp_kernels {
// Source madmom DBNTempoHistogramProcessor's interval counting. State path and
// state->interval table are explicit inputs, independent of decoder ownership.
// Dense bins correspond to every integer interval [minimum,maximum], including
// holes in a logarithmically sampled state space. Counts retain integer accuracy;
// conversion to Float32 for peak extraction is an explicit caller operation.
class PreparedStateIntervalHistogram {
public:
    static bool storage_bytes(std::uint32_t low,std::uint32_t high,std::size_t history,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return counts_.capacity()*sizeof(std::uint64_t)+history_.capacity()*sizeof(std::uint32_t);}
    bool prepare(std::uint32_t minimum_interval,std::uint32_t maximum_interval,
                 std::size_t history_frames);
    void reset() noexcept;
    bool offline(std::span<const std::uint32_t> path,std::span<const std::uint32_t> state_intervals) noexcept;
    bool online(std::span<const std::uint32_t> path,std::span<const std::uint32_t> state_intervals,
                bool reset_state=true) noexcept;
    std::span<const std::uint64_t> counts() const noexcept {return ready_?std::span<const std::uint64_t>(counts_):std::span<const std::uint64_t>{};}
    std::uint32_t minimum_interval() const noexcept {return minimum_;}
private:
    bool valid(std::span<const std::uint32_t>,std::span<const std::uint32_t>) const noexcept;
    void append(std::uint32_t interval) noexcept;
    std::uint32_t minimum_=0,maximum_=0;
    std::size_t cursor_=0,filled_=0;
    bool ready_=false,streaming_=false;
    std::vector<std::uint64_t> counts_;
    std::vector<std::uint32_t> history_;
};
}
