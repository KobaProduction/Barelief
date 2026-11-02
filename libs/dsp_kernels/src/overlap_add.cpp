// The sum-of-squared-window compensation follows librosa 1.0.0 core.istft.
// ISC, Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/overlap_add.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace dsp_kernels {
bool PreparedOverlapAdd::prepare(std::size_t frames,std::span<const float> window,std::size_t hop,bool center,
                                 OverlapNormalization normalization,OverlapSampleSelection selection) {
    std::size_t required=0;if(!storage_bytes(frames,window.size(),hop,normalization,selection,required))return false;
    if(!frames || window.empty() || !hop || frames-1>(SIZE_MAX-std::max(window.size(),hop))/hop)return false;
    if(normalization!=OverlapNormalization::WindowSquared && normalization!=OverlapNormalization::None)return false;
    if(selection!=OverlapSampleSelection::All && selection!=OverlapSampleSelection::Positive)return false;
    if(!std::all_of(window.begin(),window.end(),[](float value){return std::isfinite(value);}))return false;
    PreparedOverlapAdd next;next.maximum_frames_=frames;next.hop_=hop;next.offset_=center?window.size()/2:0;
    next.window_.assign(window.begin(),window.end());next.sum_.resize((frames-1)*hop+std::max(window.size(),hop));
    next.normalization_=normalization;next.selection_=selection;
    next.weight_.resize(next.sum_.size());next.reset();if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
void PreparedOverlapAdd::reset() noexcept {
    std::fill(sum_.begin(),sum_.end(),0.f);std::fill(weight_.begin(),weight_.end(),0.f);used_=0;valid_=maximum_frames_>0;
}
bool PreparedOverlapAdd::add(std::size_t frame,std::span<const float> values) noexcept {
    if(!valid_ || frame>=maximum_frames_ || values.size()!=window_.size())return false;
    if(!std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}))return false;
    const auto start=frame*hop_;
    for(std::size_t i=0;i<values.size();++i)if(!accumulate(start+i,values[i],window_[i]))return false;
    used_=std::max(used_,start+values.size());return true;
}
bool PreparedOverlapAdd::accumulate(std::size_t index,float value,float window) noexcept {
    const float sample=value*window;
    if(!std::isfinite(sample)){valid_=false;return false;}
    if(selection_==OverlapSampleSelection::Positive && sample<=0)return true;
    sum_[index]+=sample;
    if(normalization_==OverlapNormalization::WindowSquared)weight_[index]+=window*window;
    valid_=std::isfinite(sum_[index]) && std::isfinite(weight_[index]);return valid_;
}
bool PreparedOverlapAdd::finish(std::span<float> output) const noexcept {
    if(!valid_ || offset_>sum_.size() || output.size()>sum_.size()-offset_)return false;
    for(std::size_t i=0;i<output.size();++i){
        const auto at=i+offset_;const float scale=weight_[at];
        output[i]=at>=used_?0:(normalization_==OverlapNormalization::WindowSquared && scale>std::numeric_limits<float>::min()?sum_[at]/scale:sum_[at]);
    }
    return true;
}
}


namespace dsp_kernels {
bool PreparedOverlapAdd::storage_bytes(std::size_t frames,std::size_t window,std::size_t hop,
    OverlapNormalization normalization,OverlapSampleSelection selection,std::size_t& out) noexcept {
    if(!frames || !window || !hop || frames-1>(SIZE_MAX-std::max(window,hop))/hop ||
       unsigned(normalization)>unsigned(OverlapNormalization::None) || unsigned(selection)>unsigned(OverlapSampleSelection::Positive))return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,window,sizeof(float)) ||
       !prepared_storage::add_array(bytes,(frames-1)*hop+std::max(window,hop),2*sizeof(float)))return false;
    out=bytes;return true;
}
}
