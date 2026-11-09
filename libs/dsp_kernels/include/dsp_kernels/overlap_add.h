#pragma once
#include <cstddef>
#include <span>
#include <vector>

namespace dsp_kernels {
enum class OverlapNormalization {WindowSquared,None};
enum class OverlapSampleSelection {All,Positive};
// Explicit windowed overlap/add, optionally with sum-of-squared-window
// compensation (ISTFT default) or raw summation. Optional positive selection
// is applied after window multiplication, before summation.
class PreparedOverlapAdd {
public:
    static bool storage_bytes(std::size_t frames,std::size_t window_length,std::size_t hop,
        OverlapNormalization,OverlapSampleSelection,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return (window_.capacity()+sum_.capacity()+weight_.capacity())*sizeof(float);}
    bool prepare(std::size_t maximum_frames,std::span<const float> window,
                 std::size_t hop=1,bool center=true,
                 OverlapNormalization=OverlapNormalization::WindowSquared,
                 OverlapSampleSelection=OverlapSampleSelection::All);
    void reset() noexcept;
    bool add(std::size_t frame,std::span<const float> values) noexcept;
    bool finish(std::span<float> output) const noexcept;
private:
    void configure(std::size_t,std::span<const float>,std::size_t,bool,OverlapNormalization,OverlapSampleSelection);
    std::size_t maximum_frames_=0,hop_=1,offset_=0,used_=0;
    bool accumulate(std::size_t,float,float) noexcept;
    OverlapNormalization normalization_=OverlapNormalization::WindowSquared;
    OverlapSampleSelection selection_=OverlapSampleSelection::All;
    bool valid_=false;
    std::vector<float> window_,sum_,weight_;
};
}
