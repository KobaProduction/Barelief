// Adapted from librosa 1.0.0, librosa/onset.py: onset_strength(_multi).
// https://github.com/librosa/librosa/blob/1.0.0/librosa/onset.py
// ISC License
// Copyright (c) 2013--2026, librosa development team.
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#include <dsp_kernels/onset_strength.h>
#include <dsp_kernels/order_statistics.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>

namespace dsp_kernels {
namespace {
bool valid(const OnsetStrengthConfig& c) noexcept {
    return c.lag && c.max_size && c.n_fft && c.hop_length &&
        c.lag<=SIZE_MAX-c.n_fft/2/c.hop_length &&
        (c.aggregate==OnsetAggregate::Mean || c.aggregate==OnsetAggregate::Median || c.aggregate==OnsetAggregate::Sum);
}
std::size_t reflect(std::size_t position,std::size_t left,std::size_t count) noexcept {
    const auto period=2*count;
    const auto folded=(position+period-left)%period;
    return folded<count?folded:period-1-folded;
}
bool add_bytes(std::size_t count,std::size_t width,std::size_t& total) noexcept {
    if(count>(SIZE_MAX-total)/width)return false;
    total+=count*width;return true;
}
void sliding_maximum(std::span<const float> row,std::span<float> result,std::span<std::size_t> queue,std::size_t width) noexcept {
    const auto left=width/2;std::size_t head=0,tail=0;
    for(std::size_t position=0;position<row.size()+width-1;++position){
        const auto value=row[reflect(position,left,row.size())];
        while(head<tail && queue[head]+width<=position)++head;
        while(head<tail && row[reflect(queue[tail-1],left,row.size())]<=value)--tail;
        queue[tail++]=position;
        if(position+1>=width)result[position+1-width]=row[reflect(queue[head],left,row.size())];
    }
}
struct ContiguousRows {
    std::span<const float> values;std::size_t features=0;
    static bool read(const void* context,std::size_t time,std::span<float> row) noexcept {
        const auto& source=*static_cast<const ContiguousRows*>(context);
        std::copy_n(source.values.begin()+std::ptrdiff_t(time*source.features),row.size(),row.begin());return true;
    }
};
bool finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float value){return std::isfinite(value);});
}
bool partial_overlap(std::span<const float> input,std::span<float> output) noexcept {
    if(input.empty() || output.empty() || input.data()==output.data())return false;
    const std::less<const float*> less;
    return less(input.data(),output.data()+output.size()) && less(output.data(),input.data()+input.size());
}
bool detrend_storage(std::size_t features,std::size_t& bytes) noexcept {
    std::size_t filter=0;
    return PreparedRecursiveFilter::storage_bytes(2,2,{RecursiveFilterForm::DirectI,0},filter) &&
        add_bytes(features,sizeof(PreparedRecursiveFilter)+filter,bytes) && add_bytes(1,filter,bytes);
}
}
namespace {
bool aggregate_sum(std::span<const float> values,OnsetAggregate mode,bool allow_nonfinite,float& result) noexcept {
    const auto summation=allow_nonfinite?Float32Summation::Plain:Float32Summation::Compensated;
    if(!sum_float32(values,result,summation,allow_nonfinite))return false;
    if(mode==OnsetAggregate::Mean)result/=float(values.size());
    return true;
}
}
bool aggregate_onset_values(std::span<float> values,OnsetAggregate mode,float& output,bool allow_nonfinite) noexcept {
    if(values.empty() || (!allow_nonfinite && !finite(values)) || unsigned(mode)>unsigned(OnsetAggregate::Sum))return false;
    if(allow_nonfinite && std::any_of(values.begin(),values.end(),[](float x){return std::isnan(x);})){
        output=std::numeric_limits<float>::quiet_NaN();return true;
    }
    float result=0;
    if(mode==OnsetAggregate::Median){
        result=median_in_place(values,EvenMedian::Mean,allow_nonfinite);
    }else if(!aggregate_sum(values,mode,allow_nonfinite,result))return false;
    if(!allow_nonfinite && !std::isfinite(result))return false;
    output=result;return true;
}
bool PreparedOnsetStrength::storage_bytes(std::size_t features,std::size_t frames,const OnsetStrengthConfig& c,std::size_t& out) noexcept {
    if(!features || features>SIZE_MAX/4 || !frames || !valid(c))return false;
    std::size_t bytes=sizeof(PreparedOnsetStrength);
    if(!add_bytes(features,4*sizeof(float)+3*sizeof(std::size_t),bytes) ||
       !add_bytes(std::max(frames,c.lag),sizeof(float),bytes))return false;
    if(c.detrend && !detrend_storage(features,bytes))return false;
    out=bytes;return true;
}
void PreparedOnsetStrength::allocate_rows(std::size_t features) {
    reference_.resize(features);positive_.resize(features);queue_.resize(3*features);
    current_.resize(features);previous_.resize(features);
}
bool PreparedOnsetStrength::prepare(std::size_t features,std::size_t frames,const OnsetStrengthConfig& config) {
    std::size_t bytes=0;if(!storage_bytes(features,frames,config,bytes) || frames>SIZE_MAX/features)return false;
    PreparedOnsetStrength next;next.features_=features;next.maximum_frames_=frames;next.config_=config;
    next.allocate_rows(features);
    if(!next.prepare_detrend())return false;
    next.result_.resize(std::max(frames,config.lag));
    if(next.workspace_bytes()>bytes-sizeof(PreparedOnsetStrength))return false;
    *this=std::move(next);return true;
}
std::size_t PreparedOnsetStrength::workspace_bytes() const noexcept {
    std::size_t bytes=(reference_.capacity()+positive_.capacity()+current_.capacity()+previous_.capacity()+result_.capacity())*sizeof(float)+
        queue_.capacity()*sizeof(std::size_t)+feature_detrend_.capacity()*sizeof(PreparedRecursiveFilter)+scalar_detrend_.storage_bytes();
    for(const auto& filter:feature_detrend_)bytes+=filter.storage_bytes();
    return bytes;
}
bool PreparedOnsetStrength::prepare_detrend() {
    if(!config_.detrend)return true;
    constexpr std::array b{1.f,-1.f},a{1.f,-.99f};
    const RecursiveFilterOptions options{RecursiveFilterForm::DirectI,0};
    if(!scalar_detrend_.prepare(b,a,SubnormalStatePolicy::Preserve,options))return false;
    feature_detrend_.resize(features_);
    for(auto& filter:feature_detrend_)if(!filter.prepare(b,a,SubnormalStatePolicy::Preserve,options))return false;
    return true;
}
std::size_t PreparedOnsetStrength::output_frames(std::size_t frames) const noexcept {
    return !features_ || frames>maximum_frames_?0:config_.center?frames:std::max(frames,config_.lag);
}
void PreparedOnsetStrength::filter(std::span<const float> row) noexcept {
    if(config_.max_size>=2*features_){std::fill(reference_.begin(),reference_.end(),*std::max_element(row.begin(),row.end()));return;}
    sliding_maximum(row,reference_,queue_,config_.max_size);
}
bool PreparedOnsetStrength::positive_differences(std::span<const float> current,std::span<const float> previous,std::span<float> output,bool apply_maximum) noexcept {
    return differences(current,previous,output,{apply_maximum,true,false});
}
bool spectral_difference_values(std::span<const float> current,std::span<const float> reference,std::span<float> output,bool positive,bool zero_infinite) noexcept {
    if(current.size()!=reference.size() || output.size()!=current.size() || !finite(current) || !finite(reference))return false;
    if(partial_overlap(current,output) || partial_overlap(reference,output))return false;
    for(std::size_t bin=0;bin<current.size();++bin){
        float value=current[bin]-reference[bin];
        if(positive)value=std::max(0.f,value);
        if(zero_infinite && std::isinf(value))value=0;
        output[bin]=value;
    }
    return finite(output);
}
bool PreparedOnsetStrength::differences(std::span<const float> current,std::span<const float> previous,std::span<float> output,SpectralDifferenceOptions options) noexcept {
    if(!features_ || current.size()!=features_ || previous.size()!=features_ || output.size()!=features_)return false;
    if(!finite(current) || !finite(previous))return false;
    if(options.maximum)filter(previous);else std::copy(previous.begin(),previous.end(),reference_.begin());
    return spectral_difference_values(current,reference_,output,options.positive,options.zero_infinite);
}
float PreparedOnsetStrength::aggregate(std::span<const float> current,std::span<const float> previous,bool apply_maximum) noexcept {
    if(!positive_differences(current,previous,positive_,apply_maximum))return std::numeric_limits<float>::quiet_NaN();
    float result=0;
    return aggregate_onset_values(positive_,config_.aggregate,result)?result:std::numeric_limits<float>::quiet_NaN();
}
bool PreparedOnsetStrength::read_pair(OnsetSpectrumRows rows,OnsetSpectrumRows reference,std::size_t source) noexcept {
    return rows.read(rows.context,source,current_) && reference.read(reference.context,source-config_.lag,previous_) && finite(current_) && finite(previous_);
}
bool PreparedOnsetStrength::source_frame(std::size_t time,std::size_t frames,std::size_t& source) const noexcept {
    const auto shift=config_.center?config_.n_fft/2/config_.hop_length:0;
    source=time>=shift?time-shift:0;return time>=shift && source>=config_.lag && source<frames;
}
bool PreparedOnsetStrength::calculate(OnsetSpectrumRows rows,OnsetSpectrumRows reference,std::size_t frames,std::size_t count,bool apply_maximum) noexcept {
    scalar_detrend_.reset();
    for(std::size_t time=0;time<count;++time){
        std::size_t source=0;const bool active=source_frame(time,frames,source);
        if(active && !read_pair(rows,reference,source))return false;
        const float value=active?aggregate(current_,previous_,apply_maximum):0;
        float filtered=value;
        if(config_.detrend && !scalar_detrend_.process(value,filtered))return false;
        if(!std::isfinite(filtered))return false;
        result_[time]=filtered;
    }
    return true;
}
bool PreparedOnsetStrength::detrend_features() noexcept {
    if(!config_.detrend)return true;
    for(std::size_t bin=0;bin<features_;++bin){
        if(!feature_detrend_[bin].process(positive_[bin],positive_[bin]))return false;
    }
    return true;
}
bool PreparedOnsetStrength::calculate_features(OnsetSpectrumRows rows,OnsetSpectrumRows reference,std::size_t frames,FloatMatrixDestination output,bool apply_maximum) noexcept {
    for(auto& filter:feature_detrend_)filter.reset();
    for(std::size_t time=0;time<output_frames(frames);++time){
        std::size_t source=0;const bool active=source_frame(time,frames,source);
        if(active){
            if(!read_pair(rows,reference,source) || !positive_differences(current_,previous_,positive_,apply_maximum))return false;
        }else std::fill(positive_.begin(),positive_.end(),0.f);
        if(!detrend_features() || !output.write(output.context,time,positive_))return false;
    }
    return true;
}
bool PreparedOnsetStrength::process(std::span<const float> spectrum,std::size_t frames,std::span<float> output) noexcept {
    if(!features_ || frames>maximum_frames_ || spectrum.size()!=frames*features_ || output.size()!=output_frames(frames))return false;
    if(!finite(spectrum))return false;
    const ContiguousRows rows{spectrum,features_};return process({&rows,ContiguousRows::read},frames,output);
}
bool PreparedOnsetStrength::process(std::span<const float> spectrum,std::span<const float> reference,std::size_t frames,std::span<float> output) noexcept {
    if(!features_ || frames>maximum_frames_ || spectrum.size()!=frames*features_ || reference.size()!=spectrum.size())return false;
    if(!finite(spectrum) || !finite(reference))return false;
    const ContiguousRows rows{spectrum,features_},references{reference,features_};
    return process({&rows,ContiguousRows::read},{&references,ContiguousRows::read},frames,output);
}
bool PreparedOnsetStrength::process(OnsetSpectrumRows rows,std::size_t frames,std::span<float> output) noexcept {
    if(!features_ || !rows.read || frames>maximum_frames_ || output.size()!=output_frames(frames))return false;
    if(!calculate(rows,rows,frames,output.size(),true))return false;
    std::copy_n(result_.begin(),output.size(),output.begin());return true;
}
bool PreparedOnsetStrength::process(OnsetSpectrumRows rows,OnsetSpectrumRows reference,std::size_t frames,std::span<float> output) noexcept {
    if(!features_ || !rows.read || !reference.read || frames>maximum_frames_ || output.size()!=output_frames(frames))return false;
    if(!calculate(rows,reference,frames,output.size(),false))return false;
    std::copy_n(result_.begin(),output.size(),output.begin());return true;
}
bool PreparedOnsetStrength::process_features(OnsetSpectrumRows rows,std::size_t frames,FloatMatrixDestination output) noexcept {
    if(!features_ || !rows.read || !output.write || frames>maximum_frames_)return false;
    return calculate_features(rows,rows,frames,output,true);
}
bool PreparedOnsetStrength::process_features(OnsetSpectrumRows rows,OnsetSpectrumRows reference,std::size_t frames,FloatMatrixDestination output) noexcept {
    if(!features_ || !rows.read || !reference.read || !output.write || frames>maximum_frames_)return false;
    return calculate_features(rows,reference,frames,output,false);
}
}
