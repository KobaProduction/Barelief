#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
enum class CombDirection {Forward,Backward};
// Explicit-channel Float32 comb bank, source madmom audio/comb_filters.pyx.
// Delays are positive sample counts. One alpha broadcasts, otherwise one per
// filter. Output layout [channel][filter]. History is prepared once; process()
// consumes exactly one multichannel sample. Forward stores x, backward y.
class PreparedCombFilterBank {
public:
    static bool storage_bytes(std::span<const std::size_t>,std::span<const float>,CombDirection,std::size_t channels,std::size_t&) noexcept;
    static bool storage_bytes(std::size_t filters,std::size_t maximum_delay,CombDirection,std::size_t channels,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return delays_.capacity()*sizeof(std::size_t)+(alpha_.capacity()+history_.capacity()+values_.capacity())*sizeof(float);}
    bool prepare(std::span<const std::size_t> delays,std::span<const float> alpha,
                 CombDirection=CombDirection::Backward,std::size_t channels=1);
    void reset() noexcept;
    bool process(std::span<const float> input,std::span<float> output) noexcept;
    std::size_t channels() const noexcept {return channels_;}
    std::size_t filters() const noexcept {return delays_.size();}
private:
    bool configure(std::span<const std::size_t>,std::span<const float>,CombDirection,std::size_t);
    void allocate(std::span<const float> alpha);
    float delayed(std::size_t channel,std::size_t filter) const noexcept;
    void advance(std::span<const float>) noexcept;
    std::size_t channels_=0,capacity_=0,cursor_=0;
    CombDirection direction_=CombDirection::Backward;
    std::vector<std::size_t> delays_;
    std::vector<float> alpha_,history_,values_;
};
}
