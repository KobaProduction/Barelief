#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class PeakSelection {Greedy,MaximumCount,MaximumValue};
struct OnsetPeakConfig {
    std::size_t pre_max=1,post_max=1,pre_average=1,post_average=1,wait=0;
    float delta=.07f;
    bool normalize=false;
    PeakSelection selection=PeakSelection::Greedy;
    bool suppress_empty=false;
};
// Source onset_detect defaults for an explicit envelope frame rate.
bool onset_detection_parameters(float frame_rate,OnsetPeakConfig& output) noexcept;
// Port of librosa peak_pick: local maximum, local mean+delta, and wait. Optional
// whole-window 0..1 scaling is onset_detect's normalization, not raw ODF scaling.
// MaximumCount/Value are the original offline DP peak selection modes.
class PreparedOnsetPeakPicker {
public:
    static bool storage_bytes(std::size_t,OnsetPeakConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,OnsetPeakConfig={});
    bool process(std::span<const float>,std::span<std::uint8_t> mask) noexcept;
    std::span<const float> processed_envelope() const noexcept {return std::span(values_).first(result_frames_);}
private:
    bool load(std::span<const float>) noexcept;
    bool candidate(std::size_t index,std::size_t count) const noexcept;
    float average(std::size_t first,std::size_t last) const noexcept;
    std::size_t next(std::size_t index,std::size_t count) const noexcept;
    void greedy(std::size_t count) noexcept;
    void optimal(std::size_t count) noexcept;
    bool select(std::size_t index,std::size_t following) noexcept;
    void inherit_selection(std::size_t index) noexcept;
    OnsetPeakConfig config_;
    std::size_t maximum_frames_=0,result_frames_=0;
    std::vector<float> values_,prefix_,scores_;
    std::vector<std::size_t> links_,counts_;
    std::vector<std::uint8_t> taken_,mask_;
};
// Sparse-event source variant, including repeated backtracked event indices.
// Events may be unsorted and repeated; input/output event spans are disjoint.
class PreparedOnsetBacktracker {
public:
    static bool storage_bytes(std::size_t,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return minima_.capacity()*sizeof(std::size_t);}
    bool prepare(std::size_t maximum_energy_frames);
    bool process(std::span<const std::size_t> events,std::span<const float> energy,
                 std::span<std::size_t> output) noexcept;
private:
    std::size_t maximum_frames_=0;
    std::vector<std::size_t> minima_;
};
// Backtrack each marked onset to the nearest preceding local energy minimum.
// This dense-mask variant coalesces events mapping to the same minimum.
// Uses onset_backtrack's asymmetric <=left / <right rule and the frame0 anchor.
// Input/output masks must be disjoint; output has energy.size() elements.
bool backtrack_onset_peaks(std::span<const std::uint8_t> input,std::span<const float> energy,
                           std::span<std::uint8_t> output) noexcept;
}
