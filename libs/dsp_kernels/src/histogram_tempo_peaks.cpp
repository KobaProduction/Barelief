// Adapted from madmom features/tempo.py detect_tempo at revision
// 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU, and OFAI.
// BSD-style license and disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/histogram_tempo_peaks.h>
#include <dsp_kernels/convolution.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
namespace dsp_kernels {
namespace {
template<typename Scalar> Scalar peak_height_sum(std::span<const Scalar> bins,std::span<const std::size_t> peaks) noexcept {
    Scalar sum=0;for(const auto index:peaks)sum+=bins[index];return sum;
}
bool ordered(std::span<const float> values,float rate) noexcept {
    if(values.empty() || !std::isfinite(rate) || rate<0)return false;
    for(std::size_t i=0;i<values.size();++i)
        if(!std::isfinite(values[i]) || (rate>0 && values[i]<=0) || (i && values[i]<=values[i-1]))return false;
    return true;
}
}
bool dominant_histogram_coordinate(std::span<const float> bins,std::span<const float> coordinates,
    std::span<const float> kernel,std::span<float> scratch,float& coordinate) noexcept {
    const auto count=same_convolution_size(bins.size(),kernel.size());
    if(!count || coordinates.size()!=count || scratch.size()<count ||
        !std::all_of(coordinates.begin(),coordinates.end(),[](float x){return std::isfinite(x);}))return false;
    auto smoothed=scratch.first(count);
    if(!convolve_same(bins,kernel,smoothed))return false;
    const auto index=std::size_t(std::max_element(smoothed.begin(),smoothed.end())-smoothed.begin());
    coordinate=coordinates[index];return true;
}
bool PreparedHistogramTempoPeaks::prepare(std::span<const float> axis,float rate,bool interpolate,NumericalPrecision precision) {
    std::size_t bound=0;if(!(!axis.empty() && storage_bytes(axis.size(),axis.front(),axis.back(),interpolate,precision,bound)))return false;
    PreparedHistogramTempoPeaks next;if(!next.configure(axis,rate,interpolate,precision))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedHistogramTempoPeaks::configure(std::span<const float> axis,float rate,bool interpolate,NumericalPrecision precision) {
    if(!ordered(axis,rate))return false;
    input_count_=axis.size();frame_rate_=rate;interpolate_=interpolate;
    if(interpolate){if(!interpolation_grid(axis))return false;}
    else grid_.assign(axis.begin(),axis.end());
    bins_.resize(grid_.size());peaks_.resize(grid_.size());result_.resize(grid_.size());
    if(precision==NumericalPrecision::DesktopReference){reference_bins_.resize(grid_.size());reference_result_.resize(grid_.size());}
    return true;
}
bool PreparedHistogramTempoPeaks::interpolation_grid(std::span<const float> axis) {
    CurveInterpolationConfig config;config.kind=CurveInterpolation::Quadratic;
    if(!interpolation_.prepare(axis,config))return false;
    const float count=std::ceil((axis.back()-axis.front())*100.f);
    if(!std::isfinite(count) || count<1 || count>=float(SIZE_MAX))return false;
    grid_.resize(std::size_t(count));
    for(std::size_t i=0;i<grid_.size();++i)grid_[i]=axis.front()+float(i)*.01f;
    return ordered(grid_,frame_rate_) && grid_.back()<axis.back();
}
bool PreparedHistogramTempoPeaks::load(std::span<const float> histogram) noexcept {
    if(histogram.size()!=input_count_ || !std::all_of(histogram.begin(),histogram.end(),[](float x){return std::isfinite(x);}))return false;
    if(!interpolate_){std::copy(histogram.begin(),histogram.end(),bins_.begin());return true;}
    if(!interpolation_.set_values(histogram))return false;
    for(std::size_t i=0;i<grid_.size();++i)
        if(!interpolation_.sample(grid_[i],bins_[i]) || !std::isfinite(bins_[i]))return false;
    return true;
}
template<typename Scalar> void PreparedHistogramTempoPeaks::find_peaks(std::span<const Scalar> bins) noexcept {
    for(std::size_t i=0;i<bins.size();++i){
        const auto previous=i?i-1:bins.size()-1,next=i+1==bins.size()?0:i+1;
        if(bins[i]>bins[previous] && bins[i]>bins[next])peaks_[count_++]=i;
    }
    std::sort(peaks_.begin(),peaks_.begin()+std::ptrdiff_t(count_),[bins](auto a,auto b){
        return bins[a]>bins[b] || (bins[a]==bins[b] && a>b);
    });
}
template<typename Scalar,typename Candidate> bool PreparedHistogramTempoPeaks::publish_candidate(std::span<const Scalar> bins,std::size_t index,Scalar sum,Candidate& result) noexcept {
    const Scalar tempo=frame_rate_>0?Scalar(60)*Scalar(frame_rate_)/Scalar(grid_[index]):Scalar(grid_[index]);
    if(!std::isfinite(tempo))return false;
    result={tempo,count_==1?Scalar(1):bins[index]/sum};return true;
}
template<typename Scalar,typename Candidate> bool PreparedHistogramTempoPeaks::publish_values(std::span<const Scalar> bins,std::span<Candidate> result) noexcept {
    if(!count_){result[0]={std::numeric_limits<Scalar>::quiet_NaN(),0};count_=1;return true;}
    const Scalar sum=peak_height_sum<Scalar>(bins,std::span<const std::size_t>(peaks_).first(count_));
    if(count_>1 && (!std::isfinite(sum) || sum==0))return false;
    for(std::size_t i=0;i<count_;++i)
        if(!publish_candidate<Scalar,Candidate>(bins,peaks_[i],sum,result[i]))return false;
    return true;
}
bool PreparedHistogramTempoPeaks::publish() noexcept {
    return publish_values<float,HistogramTempo>(bins_,result_);
}
bool PreparedHistogramTempoPeaks::process(std::span<const float> histogram) noexcept {
    count_=0;if(!reference_bins_.empty() || !input_count_ || !load(histogram))return false;
    find_peaks<float>(bins_);if(publish())return true;
    count_=0;return false;
}
bool PreparedHistogramTempoPeaks::publish_reference() noexcept {
    const double limit=double(std::numeric_limits<float>::max());
    for(std::size_t i=0;i<count_;++i){
        const auto value=reference_result_[i];
        if(std::abs(value.tempo)>limit || !std::isfinite(value.strength) || std::abs(value.strength)>limit)return false;
        result_[i]={float(value.tempo),float(value.strength)};
    }
    return true;
}

bool PreparedHistogramTempoPeaks::process(std::span<const double> histogram) noexcept {
    count_=0;
    if(reference_bins_.empty() || histogram.size()!=input_count_ ||
       !std::all_of(histogram.begin(),histogram.end(),[](double x){return std::isfinite(x) && std::abs(x)<=double(std::numeric_limits<float>::max());}))return false;
    std::copy(histogram.begin(),histogram.end(),reference_bins_.begin());
    find_peaks<double>(reference_bins_);
    if(!publish_values<double,HistogramTempoReference>(reference_bins_,reference_result_)){count_=0;return false;}
    if(!publish_reference()){count_=0;return false;}
    std::transform(histogram.begin(),histogram.end(),bins_.begin(),[](double x){return float(x);});
    return true;
}

}


namespace dsp_kernels {
bool PreparedHistogramTempoPeaks::storage_bytes(std::size_t bins,float low,float high,bool interpolate,std::size_t& out) noexcept {
    if(!bins || !std::isfinite(low) || !std::isfinite(high) || high<low)return false;
    std::size_t bytes=0,grid=bins;
    if(interpolate){
        const auto count=std::ceil((high-low)*100.f);
        CurveInterpolationConfig c;c.kind=CurveInterpolation::Quadratic;
        if(!std::isfinite(count) || count<1 || count>=float(SIZE_MAX) || !PreparedCurveInterpolation::storage_bytes(bins,c,bytes))return false;
        grid=std::size_t(count);
    }
    if(!prepared_storage::add_array(bytes,grid,2*sizeof(float)+sizeof(std::size_t)+sizeof(HistogramTempo)))return false;
    out=bytes;return true;
}
bool PreparedHistogramTempoPeaks::storage_bytes(std::size_t bins,float low,float high,bool interpolate,NumericalPrecision precision,std::size_t& out) noexcept {
    if(unsigned(precision)>unsigned(NumericalPrecision::DesktopReference) || (precision==NumericalPrecision::DesktopReference && interpolate))return false;
    std::size_t bytes=0;
    if(!storage_bytes(bins,low,high,interpolate,bytes))return false;
    if(precision==NumericalPrecision::DesktopReference && !prepared_storage::add_array(bytes,bins,sizeof(double)+sizeof(HistogramTempoReference)))return false;
    out=bytes;return true;
}

}
