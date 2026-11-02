#pragma once
#include <cstddef>
#include <span>
namespace dsp_kernels {
enum class FrameSamplingTail {Normal,Extend,Complete};
struct FrameSamplingConfig {
    std::size_t length=2048;
    // Coordinate/clock precision only; analytical samples remain Float32.
    // Matches source int(index*hop), including fractional hops.
    double hop_samples=441;
    std::ptrdiff_t origin=0;
    FrameSamplingTail tail=FrameSamplingTail::Normal;
    bool fixed_frames=false;
    std::size_t frame_count=0;
    bool center=true; // Complete selects STFT frames inside the padded extent.
};
class PreparedFrameSampling {
public:
    bool prepare(std::size_t maximum_samples,FrameSamplingConfig={}) noexcept;
    static bool frame_count(std::size_t maximum_samples,FrameSamplingConfig,std::size_t&) noexcept;
    std::size_t frames(std::size_t samples) const noexcept;
    bool first_sample(std::size_t samples,std::size_t frame,std::ptrdiff_t&) const noexcept;
    const FrameSamplingConfig& config() const noexcept {return config_;}
private:
    bool configure(std::size_t,FrameSamplingConfig) noexcept;
    FrameSamplingConfig config_;
    std::size_t maximum_samples_=0;
};
}
