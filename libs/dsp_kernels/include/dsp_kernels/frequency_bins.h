#pragma once
#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
namespace dsp_kernels {
// Borrowed cold coordinate accessor for allocation-free bank geometry queries.
struct FrequencyGridView {
    const void* context=nullptr;
    std::size_t size=0;
    double (*at)(const void*,std::size_t) noexcept=nullptr;
};
inline std::size_t nearest_frequency_bin(FrequencyGridView grid,double hz) noexcept {
    if(!grid.size || !grid.at || !std::isfinite(hz))return SIZE_MAX;
    if(grid.size==1)return 0;
    std::size_t low=0,high=grid.size;
    while(low<high){const auto middle=low+(high-low)/2;if(grid.at(grid.context,middle)<hz)low=middle+1;else high=middle;}
    auto right=std::clamp(low,std::size_t{1},grid.size-1);
    if(hz-grid.at(grid.context,right-1)<grid.at(grid.context,right)-hz)--right;
    return right;
}
// Ordered explicit frequency grid. Source nearest rule clips to endpoints;
// exact midpoint ties select the upper bin. Cold coordinate precision only.
template<std::floating_point T>
std::size_t nearest_frequency_bin(std::span<const T> grid,double hz) noexcept {
    if(grid.empty() || !std::isfinite(hz))return SIZE_MAX;
    if(grid.size()==1)return 0;
    const auto at=std::lower_bound(grid.begin(),grid.end(),hz);
    auto right=std::clamp(std::size_t(at-grid.begin()),std::size_t{1},grid.size()-1);
    if(hz-double(grid[right-1])<double(grid[right])-hz)--right;
    return right;
}
}
