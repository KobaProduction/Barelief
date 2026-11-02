#include <dsp_kernels/rhythm.h>
#include <dsp_kernels/filter_projection.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels::rhythm {
namespace {
std::optional<float> raw_band_energy(std::span<const float> input,std::size_t first,std::size_t last) noexcept {
    const auto values=input.subspan(2*first,2*(last-first));
    float energy=0;
    if(!project_filter_weights(values,values,energy,{false,false}))return {};
    return energy;
}
std::optional<float> amplitude_band_energy(std::span<const float> input,std::size_t first,std::size_t last) noexcept {
    const auto values=input.subspan(first,last-first);
    if(std::any_of(values.begin(),values.end(),[](float value){return value<0;}))return {};
    float energy=0;
    if(!project_filter_weights(values,values,energy,{false,false}))return {};
    return energy;
}
bool project_bands(std::span<const float> input,std::span<float> output,
                   PackingStatistics& statistics,bool complex) noexcept {
    if(input.size()!=(complex?4098u:2048u) || (output.size()!=64 && output.size()!=96))return false;
    std::array<float,96> prepared{};PackingStatistics stats;
    const float scale=complex?2.f/2048.f:1.f;
    for(std::size_t band=0;band<output.size();++band){
        const auto first=2048*band/output.size(),last=2048*(band+1)/output.size();
        const auto energy=complex?raw_band_energy(input,first+1,last+1):amplitude_band_energy(input,first,last);
        if(!energy)return false;
        const float level=scale*std::sqrt(*energy);
        stats.clipped_bands+=unsigned(level>=1.f);stats.nonzero_bands+=unsigned(level>0);
        prepared[band]=std::min(1.f,level);
    }
    std::copy_n(prepared.begin(),output.size(),output.begin());statistics=stats;return true;
}
bool pack_bands(std::span<const float> input,std::span<std::uint8_t> output,
                PackingStatistics& statistics,bool complex) noexcept {
    std::array<float,96> prepared{};PackingStatistics stats;
    if(output.size()>prepared.size() || !project_bands(input,std::span{prepared}.first(output.size()),stats,complex))return false;
    stats.nonzero_bands=0;
    for(std::size_t band=0;band<output.size();++band){
        output[band]=std::uint8_t(std::lround(255.f*prepared[band]));
        stats.nonzero_bands+=unsigned(output[band]!=0);
    }
    statistics=stats;return true;
}
}

bool project_rfft_features(std::span<const float> input,std::span<float> output,PackingStatistics& statistics) noexcept {
    return project_bands(input,output,statistics,true);
}
bool project_peak_amplitude_features(std::span<const float> input,std::span<float> output,
                                     PackingStatistics& statistics) noexcept {
    return project_bands(input,output,statistics,false);
}
bool pack_rfft(std::span<const float> input,std::span<std::uint8_t> output,PackingStatistics& statistics) noexcept {
    return pack_bands(input,output,statistics,true);
}
bool pack_peak_amplitudes(std::span<const float> input,std::span<std::uint8_t> output,
                          PackingStatistics& statistics) noexcept {
    return pack_bands(input,output,statistics,false);
}
}
