// Adapted from madmom features/downbeats.py at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/metrical_events.h>
#include <cmath>
#include <limits>

namespace dsp_kernels {
namespace {
bool metrical_number(std::uint32_t state,std::span<const float> positions,std::uint32_t& number) noexcept {
    if(state>=positions.size())return false;
    const float position=positions[state];
    if(!std::isfinite(position) || position<0 || position>=float(UINT32_MAX))return false;
    number=std::uint32_t(position)+1;
    return true;
}
}
bool metrical_change_events(std::span<const std::uint32_t> path,std::span<const float> positions,
                             std::span<std::uint32_t> frames,std::span<std::uint32_t> numbers,std::size_t& count) noexcept {
    count=0;
    if(path.size()>UINT32_MAX || frames.size()<path.size() || numbers.size()<path.size())return false;
    std::uint32_t previous=0;
    for(std::size_t frame=0;frame<path.size();++frame){
        std::uint32_t number=0;
        if(!metrical_number(path[frame],positions,number))return false;
        if(frame && number!=previous){frames[count]=std::uint32_t(frame);numbers[count++]=number;}
        previous=number;
    }
    return true;
}
}
