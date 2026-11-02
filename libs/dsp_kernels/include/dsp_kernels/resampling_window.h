#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <dsp_kernels/numerical_precision.h>
namespace dsp_kernels {
// EndpointLinear uses integer interval subdivision, including the final endpoint.
// Source geometry: MTG/essentia@b9fa6cb674ca43dfb94d28d293aeda441c6745db,
// src/algorithms/rhythm/tempotapdegara.cpp (compute, ODF expansion).
enum class ResamplingWindowMode { SincBestQuality,EndpointLinear };
// Finite independent windows; these counts are not PCM sample rates.
// Counts are exact for sinc and maximum capacities for endpoint expansion.
// Each channel is independent and stored in its own planar span.
struct ResamplingWindowConfig {
    std::size_t input_samples=0,output_samples=0,channels=1;
    ResamplingWindowMode mode=ResamplingWindowMode::SincBestQuality;
    std::size_t expansion_factor=1;
    // Source sinc uses Float32 coefficients/samples with Double interpolation
    // and serial accumulation. EndpointLinear supports PortableFloat32 only.
    NumericalPrecision precision=NumericalPrecision::PortableFloat32;
    bool operator==(const ResamplingWindowConfig&) const = default;
};
inline bool endpoint_expansion_size(std::size_t samples,std::size_t factor,std::size_t& output) noexcept {
    if(!factor || (samples && samples-1>(SIZE_MAX-1)/factor))return false;
    output=samples?(samples-1)*factor+1:0;return true;
}
inline bool valid_resampling_window(const ResamplingWindowConfig& c) noexcept {
    if(!c.channels || c.channels>2 || !c.input_samples || !c.output_samples)return false;
    if(unsigned(c.precision)>unsigned(NumericalPrecision::DesktopReference))return false;
    if(c.mode==ResamplingWindowMode::EndpointLinear){
        std::size_t count=0;
        return c.precision==NumericalPrecision::PortableFloat32 && c.input_samples<=PTRDIFF_MAX && c.output_samples<=PTRDIFF_MAX &&
            endpoint_expansion_size(c.input_samples,c.expansion_factor,count) && count==c.output_samples;
    }
    if(c.mode!=ResamplingWindowMode::SincBestQuality || c.expansion_factor!=1)return false;
    if(c.input_samples>INT32_MAX || c.output_samples>INT32_MAX)return false;
    return std::uint64_t(c.input_samples)<=256*std::uint64_t(c.output_samples) &&
        std::uint64_t(c.output_samples)<=256*std::uint64_t(c.input_samples);
}
}
