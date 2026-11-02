#pragma once
#include <cstddef>
#include <span>
namespace dsp_kernels {
enum class EnergyNoveltyValue {SumSquares,RootMeanSquare};
struct EnergyNoveltyConfig {
    EnergyNoveltyValue value=EnergyNoveltyValue::SumSquares;
    bool positive_difference=false,compensated_sum=true;
};
// Explicit PCM-frame energy/RMS and positive temporal difference. A spectrum
// is not silently treated as PCM; sample/window selection belongs to the caller.
class PreparedEnergyNovelty {
public:
    bool prepare(EnergyNoveltyConfig={}) noexcept;
    void reset() noexcept {previous_=0;}
    bool process(std::span<const float> pcm,float& output) noexcept;
private:
    EnergyNoveltyConfig config_;
    float previous_=0;
    bool prepared_=false;
};
}
