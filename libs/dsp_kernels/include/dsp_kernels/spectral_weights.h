#pragma once
#include <cmath>
#include <cstddef>
#include <span>

#include <cstdint>
#include <limits>

namespace dsp_kernels {

struct PositiveSpectrumPeak {
    std::size_t index=0;
    float magnitude=0;
    bool include(float value,std::size_t at) noexcept {
        if(!std::isfinite(value))return false;
        if(value>magnitude){magnitude=value;index=at;}
        return true;
    }
    bool frequency(double bin_hz,std::uint32_t first_bin,float& output) const noexcept {
        const auto hz=magnitude>0?(double(first_bin)+double(index))*bin_hz:0.;
        if(!std::isfinite(hz) || hz<0 || hz>std::numeric_limits<float>::max())return false;
        output=float(hz);return true;
    }
};

inline double bark_weight(double hz) noexcept { return 7. * std::asinh(hz / 650.); }
inline void prepare_bark_weights(std::span<float> output,double origin,double step) noexcept {
    for(std::size_t i=0;i<output.size();++i)output[i]=float(bark_weight(origin+double(i)*step));
}
inline double mel_frequency(double fraction, double nyquist) noexcept {
    return 700. * std::expm1(fraction * std::log1p(nyquist / 700.));
}
inline double triangular_weight(double hz, double left, double center, double right) noexcept {
    if (hz <= left || hz >= right || center <= left || right <= center) return 0;
    return hz < center ? (hz - left) / (center - left) : (right - hz) / (right - center);
}
inline bool apply_mel_weights(std::span<const float> input, std::span<const double> frequencies,
                              std::span<const double> boundaries, std::span<float> output) noexcept {
    if (input.size() != frequencies.size() || boundaries.size() != output.size() + 2) return false;
    for (std::size_t band = 0; band < output.size(); ++band) {
        double energy = 0;
        for (std::size_t bin = 0; bin < input.size(); ++bin)
            energy += input[bin] * triangular_weight(frequencies[bin], boundaries[band], boundaries[band+1], boundaries[band+2]);
        if (!std::isfinite(energy) || !std::isfinite(float(energy))) return false;
        output[band] = float(energy);
    }
    return true;
}
}
