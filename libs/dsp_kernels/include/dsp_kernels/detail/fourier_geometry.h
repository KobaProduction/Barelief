#pragma once
#include <cstddef>
#include <limits>
namespace dsp_kernels::detail {
inline std::size_t fourier_convolution_size(std::size_t samples) noexcept {
    if(!samples || samples>std::numeric_limits<std::size_t>::max()/4)return 0;
    if(!(samples&(samples-1)))return samples;
    std::size_t size=4;
    while(size<2*samples-1)size*=2;
    return size>=2*samples-1?size:0;
}
}
