#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <cstddef>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class EmptySubdivision {SourceNaN,Zero};
struct BeatFeatureSynchronizationConfig {
    std::size_t subdivisions=4;
    float frame_rate=100;
    EmptySubdivision empty=EmptySubdivision::SourceNaN;
};
// Source SyncronizeFeaturesProcessor, explicit [frame][feature] input rows and
// beat TIMES (a source 2D beat annotation's first column is an adapter concern).
// Trim beats beyond feature duration. First start is 20ms before beat0; subsequent
// starts are preceding ends, with half-subdivision offset capped at 50ms.
// Output rows flatten [beat interval][subdivision], each containing [feature].
// Unlike its docstring, original mean(empty) produces NaN, not zero. Default
// retains NaN plus explicit empty_subdivisions diagnostics; Zero is opt-in.
// Synchronous writer may receive a prefix on failure; no full feature copy.
class PreparedBeatFeatureSynchronization {
public:
    static bool storage_bytes(std::size_t features,BeatFeatureSynchronizationConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_features,BeatFeatureSynchronizationConfig={});
    bool process(FloatMatrixSource,std::size_t frames,std::size_t features,
                 std::span<const float> beat_seconds,FloatMatrixDestination) noexcept;
    std::size_t retained_beats() const noexcept {return retained_beats_;}
    std::size_t empty_subdivisions() const noexcept {return empty_count_;}
private:
    void accumulate_row(std::span<const float>,std::size_t) noexcept;
    bool interval_end(std::span<const float>,std::size_t,std::size_t,std::size_t&) const noexcept;
    bool boundaries(std::size_t,std::span<const float>,std::size_t&) noexcept;
    bool interval(FloatMatrixSource,std::size_t start,std::size_t end,std::size_t features) noexcept;
    bool write(FloatMatrixDestination,std::size_t interval,std::size_t features) noexcept;
    std::size_t maximum_features_=0,retained_beats_=0,empty_count_=0;
    BeatFeatureSynchronizationConfig config_;
    std::vector<float> sums_,row_;
    std::vector<std::size_t> counts_;
};
}
