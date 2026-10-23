#include <dsp_kernels/rhythm.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels::rhythm {
namespace {
std::optional<double> raw_band_energy(std::span<const float> input,std::size_t first,std::size_t last) noexcept {
    double energy=0;
    for(std::size_t k=first;k<last;++k){
        const double real=input[2*k],imaginary=input[2*k+1];
        if(!std::isfinite(real) || !std::isfinite(imaginary))return {};
        energy+=real*real+imaginary*imaginary;
    }
    return std::isfinite(energy)?std::optional{energy}:std::nullopt;
}
std::optional<double> amplitude_band_energy(std::span<const float> input,std::size_t first,std::size_t last) noexcept {
    double energy=0;
    for(std::size_t k=first;k<last;++k){
        const double amplitude=input[k];
        if(!std::isfinite(amplitude) || amplitude<0)return {};
        energy+=amplitude*amplitude;
    }
    return std::isfinite(energy)?std::optional{energy}:std::nullopt;
}
std::uint8_t quantize_level(double level,PackingStatistics& stats) noexcept {
    const auto value=std::uint8_t(std::lround(255*std::min(1.,level)));
    stats.clipped_bands+=unsigned(level>=1);
    stats.nonzero_bands+=unsigned(value!=0);
    return value;
}
}

bool pack_rfft(std::span<const float> input,std::span<std::uint8_t> output,PackingStatistics& statistics) noexcept {
    if(input.size()!=4098 || (output.size()!=64 && output.size()!=96))return false;
    std::array<std::uint8_t,96> prepared{};PackingStatistics stats;
    for(std::size_t band=0;band<output.size();++band){
        const auto energy=raw_band_energy(input,1+2048*band/output.size(),1+2048*(band+1)/output.size());
        if(!energy)return false;
        prepared[band]=quantize_level(2*std::sqrt(*energy)/2048,stats);
    }
    std::copy_n(prepared.begin(),output.size(),output.begin());statistics=stats;return true;
}

bool pack_peak_amplitudes(std::span<const float> input,std::span<std::uint8_t> output,
                          PackingStatistics& statistics) noexcept {
    // Input is the normalized k=1..2048 peak-amplitude spectrum from AmplitudePhase.
    if(input.size()!=2048 || (output.size()!=64 && output.size()!=96))return false;
    std::array<std::uint8_t,96> prepared{};PackingStatistics stats;
    for(std::size_t band=0;band<output.size();++band){
        const auto energy=amplitude_band_energy(input,2048*band/output.size(),2048*(band+1)/output.size());
        if(!energy)return false;
        prepared[band]=quantize_level(std::sqrt(*energy),stats);
    }
    std::copy_n(prepared.begin(),output.size(),output.begin());statistics=stats;return true;
}
}
