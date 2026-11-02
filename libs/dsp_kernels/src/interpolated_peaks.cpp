#include <dsp_kernels/peaks.h>
namespace dsp_kernels {
bool interpolated_local_peaks(std::span<const float> input,float threshold,bool endpoints,
    std::span<float> positions,std::span<float> heights,std::span<float> offsets,std::size_t& count) noexcept {
    count=0;
    if(!std::isfinite(threshold) || positions.size()<input.size() || heights.size()<input.size() || offsets.size()!=input.size() ||
       !std::all_of(input.begin(),input.end(),[](float v){return std::isfinite(v);}))return false;
    std::fill(offsets.begin(),offsets.end(),0.f);
    for(std::size_t i=0;i<input.size();++i){
        if(input[i]<threshold || ((!i || i+1==input.size()) && !endpoints))continue;
        if((i && input[i]<=input[i-1]) || (i+1<input.size() && input[i]<input[i+1]))continue;
        const float offset=i && i+1<input.size()?quadratic_peak_offset(input[i-1],input[i],input[i+1]):0;
        offsets[i]=offset;positions[count]=float(i)+offset;heights[count]=input[i];++count;
    }
    return true;
}
}
