#pragma once
#include <cstddef>
#include <span>

namespace dsp_kernels {
enum class ZeroMagnitudePhase {Zero,Atan2};
enum class ComplexMagnitudeNorm {Hypot,SquaredNorm,ScaledRatioFma};
struct ComplexPolarOptions {
    ZeroMagnitudePhase zero_phase=ZeroMagnitudePhase::Zero;
    ComplexMagnitudeNorm magnitude_norm=ComplexMagnitudeNorm::Hypot;
    bool real_axis=false;
};
// Raw Float32 values, without window, sidedness, FFT or physical calibration.
// Either output may be null. Real-axis mode ignores Im for magnitude/phase,
// publishing abs(Re) and sign(Re)?pi:0 as required by explicit real endpoints.
// ScaledRatioFma uses Float32 max/min ratio, fused ratio^2+1, sqrt and
// multiplication by max. Existing Hypot/SquaredNorm values and defaults stay.
// The bounded fused step uses exact binary64 product/residual and midpoint
// correction to retain binary32 RNE even with an inaccurate platform fmaf.
bool complex_polar_value(float real,float imaginary,float* magnitude,float* phase,
                         ComplexPolarOptions={}) noexcept;
bool magnitude_power(float magnitude,float exponent,float& output) noexcept;
bool magnitude_power(std::span<const float> magnitude,float exponent,std::span<float> output) noexcept;
// Interleaved [bin][Re,Im]; supplied output planes are disjoint from input and
// each other. Failure may leave a successfully converted output prefix.
// real_fft_size selects real DC/even-Nyquist endpoints and validates full half
// spectrum geometry. Zero/Atan2 preserves signed-zero source differences.
bool cartesian_to_polar(std::span<const float> complex,std::span<float> magnitude,
    std::span<float> phase,ZeroMagnitudePhase=ZeroMagnitudePhase::Zero,
    ComplexMagnitudeNorm=ComplexMagnitudeNorm::Hypot,std::size_t real_fft_size=0) noexcept;
bool expand_real_spectrum(std::span<const float> half,std::size_t fft_size,std::span<float> full) noexcept;
}
