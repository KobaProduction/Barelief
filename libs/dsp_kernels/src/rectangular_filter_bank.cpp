// Adapted from madmom audio/filters.py RectangularFilterbank at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/rectangular_filter_bank.h>
#include <dsp_kernels/log_frequency_filter_bank.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <utility>

namespace dsp_kernels {
namespace {
bool ordered(std::span<const float> values,bool strict) noexcept {
    if(values.empty() || values.front()<0)return false;
    for(std::size_t i=0;i<values.size();++i){
        if(!std::isfinite(values[i]) || (i && (values[i]<values[i-1] || (strict && values[i]==values[i-1]))))return false;
    }
    return true;
}
std::vector<std::size_t> filter_corners(std::span<const float> grid,std::span<const float> borders,bool unique) {
    std::vector<std::size_t> result;result.reserve(borders.size());
    for(const float hz:borders){
        const auto bin=LogFrequencyFilterBank::nearest(grid,hz);
        if(!unique || result.empty() || result.back()!=bin)result.push_back(bin);
    }
    return result;
}
void filter_weights(std::span<const std::size_t> corners,std::size_t bins,bool normalize,
    std::span<float> weights,std::span<FilterBandSupport> supports) noexcept {
    for(std::size_t band=0;band+1<corners.size();++band){
        const auto count=corners[band+1]-corners[band];supports[band]={corners[band],count};
        const float gain=normalize && count?1.f/float(count):1.f;
        std::fill_n(weights.begin()+std::ptrdiff_t(band*bins+corners[band]),count,gain);
    }
}
bool within_peak(std::size_t actual,std::size_t bound,const std::vector<std::size_t>& corners,
    const std::vector<float>& weights,const std::vector<FilterBandSupport>& supports) noexcept {
    return prepared_storage::add_array(actual,corners.capacity(),sizeof(std::size_t)) && prepared_storage::add_array(actual,weights.capacity(),sizeof(float)) &&
       prepared_storage::add_array(actual,supports.capacity(),sizeof(FilterBandSupport)) && actual<=bound;
}
}
bool PreparedRectangularFilterBank::storage_bytes(std::size_t bins,std::size_t borders,std::size_t& out) noexcept {
    if(bins<2 || borders<2)return false;
    const auto bands=borders-1;std::size_t bytes=0,weights=0;
    if(!MelFilterBank::matrix_storage_bytes(bins,bands,bytes) || !prepared_storage::mul_bytes(bands,bins,weights) ||
       !prepared_storage::add_array(bytes,weights,sizeof(float)) || !prepared_storage::add_array(bytes,bands,sizeof(FilterBandSupport)) ||
       !prepared_storage::add_array(bytes,borders,sizeof(std::size_t)))return false;
    out=bytes;return true;
}
namespace {
bool valid_rectangular_axes(std::span<const float> grid,std::span<const float> borders) noexcept {
    return grid.size()>=2 && borders.size()>=2 && ordered(grid,true) && ordered(borders,false) &&
        borders.size()-1<=std::numeric_limits<std::size_t>::max()/grid.size();
}
}
bool PreparedRectangularFilterBank::configure(std::span<const float> grid,std::span<const float> borders,bool normalize,bool unique,std::size_t bound) {
    const auto corners=filter_corners(grid,borders,unique);
    const auto bands=borders.size()-1;
    std::vector<float> weights(bands*grid.size(),0.f);
    std::vector<FilterBandSupport> supports(bands);
    filter_weights(corners,grid.size(),normalize,weights,supports);
    bands_=bands;
    return projection_.prepare_matrix(grid.size(),bands,weights,{}, {false,false},supports) &&
        within_peak(workspace_bytes(),bound,corners,weights,supports);
}
bool PreparedRectangularFilterBank::prepare(std::span<const float> grid,std::span<const float> borders,bool normalize,bool unique) {
    std::size_t bound=0;if(!storage_bytes(grid.size(),borders.size(),bound))return false;
    if(!valid_rectangular_axes(grid,borders))return false;
    PreparedRectangularFilterBank next;
    if(!next.configure(grid,borders,normalize,unique,bound))return false;
    *this=std::move(next);return true;
}
}
