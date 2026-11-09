#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
// Source madmom detect_beats, without Python recursion. Evaluates every phase
// in [0,interval), refines each predicted beat with a symmetric Hamming window,
// then keeps the phase with the greatest summed activation. Nonnegative
// activations, explicit interval; smoothing and tempo estimation are separate.
// Interval may vary between calls. look_aside in [0,1], including source default.
// All Hamming extents are generated in prepare through the common window
// operation. Storage is R*(R+1) Float32 values, R=max(1,floor(max_interval*look_aside)).
class PreparedIntervalBeatSearch {
public:
    static bool storage_bytes(std::size_t frames,std::size_t interval,float look_aside,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return (window_.capacity()+scores_.capacity())*sizeof(float)+positions_.capacity()*sizeof(std::size_t);}
    bool prepare(std::size_t maximum_frames,std::size_t maximum_interval,float look_aside=.2f);
    bool process(std::span<const float> activations,std::size_t interval) noexcept;
    std::span<const std::size_t> positions() const noexcept {return std::span(positions_).first(count_);}
    std::span<const float> phase_scores() const noexcept {return std::span(scores_).first(result_interval_);}
private:
    void allocate_results(std::size_t,std::size_t,float);
    bool retain_position(std::size_t,bool) noexcept;
    bool select_phase(std::span<const float>,std::size_t) noexcept;
    bool prepare_windows(std::size_t maximum_radius);
    std::size_t refine(std::span<const float>,std::size_t) const noexcept;
    bool score_phases(std::span<const float>,std::size_t) noexcept;
    float weighted(std::span<const float>,std::ptrdiff_t,std::size_t) const noexcept;
    bool score_position(std::span<const float>,std::size_t&,bool,float&) noexcept;
    bool sequence(std::span<const float>,std::size_t,std::size_t,bool,float&) noexcept;
    std::size_t maximum_frames_=0,maximum_interval_=0,radius_=0,count_=0,result_interval_=0;
    float look_aside_=0;
    std::vector<float> window_,scores_;
    std::vector<std::size_t> positions_;
};
}
