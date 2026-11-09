// Adapted from librosa 1.0.0 feature/rhythm.py: tempo, tempogram_ratio,
// metrogram. ISC, librosa development team. Interpolation modes are separate
// PreparedCurveInterpolation operations; linear convenience overloads remain.
// Full notice: packaging/licenses/librosa.txt.
// Source: librosa/librosa@3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/tempo_estimator.h>
#include <dsp_kernels/order_statistics.h>
#include <dsp_kernels/sequence_statistics.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace dsp_kernels {
namespace {
bool valid(const TempoEstimatorConfig& c) noexcept {
    return std::isfinite(c.frame_rate) && c.frame_rate>0 && std::isfinite(c.frame_rate*60.f) &&
        std::isfinite(c.start_bpm) && c.start_bpm>0 && std::isfinite(c.std_bpm) && c.std_bpm>0 &&
        (!c.limit_max_tempo || (std::isfinite(c.max_tempo) && c.max_tempo>0)) &&
        c.aggregate>=TempoAggregation::None && c.aggregate<=TempoAggregation::Median;
}
float harmonic_value(float value,float fill) noexcept {
    const float maximum=std::numeric_limits<float>::max();
    return std::isnan(value)?fill:std::clamp(value,-maximum,maximum);
}
bool valid_prior(std::span<const float> values) noexcept {
    return std::all_of(values.begin(),values.end(),[](float x){return !std::isnan(x) && x!=std::numeric_limits<float>::infinity();});
}
}
void PreparedTempoEstimator::allocate(std::size_t frames,std::size_t lags,TempoEstimatorConfig config) {
    config_=config;maximum_frames_=frames;lags_=lags;
    row_.resize(lags);aggregate_.resize(lags);correction_.resize(lags);
    column_.resize(frames);result_.resize(frames);
}
bool PreparedTempoEstimator::prepare(std::size_t frames,std::size_t lags,TempoEstimatorConfig config) {
    std::size_t required=0;if(!storage_bytes(frames,lags,config,required))return false;
    if(!frames || !lags || !valid(config))return false;
    PreparedTempoEstimator next;
    next.allocate(frames,lags,config);
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedTempoEstimator::read(FloatMatrixSource source,std::size_t frame) noexcept {
    return source.read && source.read(source.context,frame,row_) &&
        std::all_of(row_.begin(),row_.end(),[](float value){return std::isfinite(value);});
}
bool PreparedTempoEstimator::mean(FloatMatrixSource source,std::size_t frames) noexcept {
    std::fill(aggregate_.begin(),aggregate_.end(),0.f);std::fill(correction_.begin(),correction_.end(),0.f);
    for(std::size_t frame=0;frame<frames;++frame){
        if(!read(source,frame))return false;
        for(std::size_t lag=0;lag<lags_;++lag)
            if(!accumulate_scaled_mean_float32(row_[lag],float(frames),aggregate_[lag],correction_[lag]))return false;
    }
    return true;
}
bool PreparedTempoEstimator::median(FloatMatrixSource source,std::size_t frames) noexcept {
    for(std::size_t lag=0;lag<lags_;++lag){
        for(std::size_t frame=0;frame<frames;++frame){if(!read(source,frame))return false;column_[frame]=row_[lag];}
        aggregate_[lag]=median_in_place(std::span(column_).first(frames),EvenMedian::HalfSum);
        if(!std::isfinite(aggregate_[lag]))return false;
    }
    return true;
}
bool PreparedTempoEstimator::aggregate(FloatMatrixSource source,std::size_t frames) noexcept {
    return config_.aggregate==TempoAggregation::Median?median(source,frames):mean(source,frames);
}
std::size_t PreparedTempoEstimator::tempo_cutoff() const noexcept {
    if(config_.limit_max_tempo)
        for(std::size_t lag=1;lag<lags_;++lag)
            if(60.f*config_.frame_rate/float(lag)<config_.max_tempo)return lag;
    // Original np.argmax(False,...,False) returns zero, disabling the prefix mask.
    return 0;
}
bool PreparedTempoEstimator::select(std::span<const float> prior,float& bpm) const noexcept {
    const auto cutoff=tempo_cutoff();
    float best=-std::numeric_limits<float>::infinity();std::size_t selected=0;
    for(std::size_t lag=0;lag<lags_;++lag){
        const float tempo=lag?60.f*config_.frame_rate/float(lag):std::numeric_limits<float>::infinity();
        const float distance=(std::log2(tempo)-std::log2(config_.start_bpm))/config_.std_bpm;
        const float preference=lag<cutoff?-std::numeric_limits<float>::infinity():prior.empty()?-.5f*distance*distance:prior[lag];
        const float score=std::log1p(1e6f*row_[lag])+preference;
        if(std::isnan(score))return false;
        if(score>best){best=score;selected=lag;}
    }
    bpm=selected?60.f*config_.frame_rate/float(selected):std::numeric_limits<float>::infinity();return true;
}
bool PreparedTempoEstimator::reduce(FloatMatrixSource source,std::size_t frames,FloatMatrixReductionPort reduction) noexcept {
    if(!reduction.reduce)return aggregate(source,frames);
    return reduction.reduce(reduction.context,source,frames,lags_,aggregate_) &&
        std::all_of(aggregate_.begin(),aggregate_.end(),[](float x){return std::isfinite(x);});
}
bool PreparedTempoEstimator::select_aggregate(FloatMatrixSource source,std::size_t frames,std::span<const float> prior,FloatMatrixReductionPort reduction) noexcept {
    if(!reduce(source,frames,reduction))return false;
    std::copy(aggregate_.begin(),aggregate_.end(),row_.begin());
    return select(prior,result_[0]);
}
bool PreparedTempoEstimator::select_frames(FloatMatrixSource source,std::size_t frames,std::span<const float> prior) noexcept {
    for(std::size_t i=0;i<frames;++i)if(!read(source,i) || !select(prior,result_[i]))return false;
    return true;
}
bool PreparedTempoEstimator::process(FloatMatrixSource source,std::size_t frames,std::span<float> output,std::span<const float> prior,FloatMatrixReductionPort reduction) noexcept {
    if(!maximum_frames_ || !frames || frames>maximum_frames_ || (!prior.empty() && prior.size()!=lags_))return false;
    const bool aggregated=config_.aggregate!=TempoAggregation::None || reduction.reduce;
    const auto count=aggregated?1:frames;
    if(output.size()!=count || !valid_prior(prior))return false;
    const bool selected=aggregated?select_aggregate(source,frames,prior,reduction):select_frames(source,frames,prior);
    if(!selected)return false;
    std::copy_n(result_.begin(),count,output.begin());return true;
}
namespace {
bool tempo_curve_brackets(std::span<const float> grid,std::size_t previous,std::size_t i,float query) noexcept {
    return previous<grid.size() && query>std::min(grid[i],grid[previous]) && query<std::max(grid[i],grid[previous]);
}
bool interpolate_tempo_segment(std::span<const float> grid,std::span<const float> values,std::size_t previous,std::size_t i,float query,float& result) noexcept {
    const float t=(query-grid[previous])/(grid[i]-grid[previous]);
    result=values[previous]+t*(values[i]-values[previous]);
    return std::isfinite(result);
}
bool sample_tempo_curve(std::span<const float> grid,std::span<const float> values,float query,float& result) noexcept {
    std::size_t previous=grid.size();
    for(std::size_t i=0;i<grid.size();++i){
        if(!std::isfinite(grid[i]))continue;
        if(!std::isfinite(values[i]))return false;
        if(query==grid[i]){result=values[i];return true;}
        if(tempo_curve_brackets(grid,previous,i,query))return interpolate_tempo_segment(grid,values,previous,i,query,result);
        previous=i;
    }
    return true;
}
bool metrical_row(std::span<const float> grid,std::span<const float> values,float factor,std::span<float> output) noexcept {
    for(std::size_t j=0;j<values.size();++j){
        if(!std::isfinite(grid[j])){output[j]=0;continue;}
        float sample=0;
        if(!interpolate_tempo_curve(grid,values,grid[j]*factor,sample))return false;
        output[j]=sample*values[j];
    }
    return true;
}
bool metrical_row(const PreparedCurveInterpolation& curve,float factor,std::span<float> output) noexcept {
    const auto grid=curve.coordinates(),values=curve.values();
    for(std::size_t j=0;j<values.size();++j){
        float sample=0;
        if(!curve.sample(grid[j]*factor,sample))return false;
        const float value=sample*values[j];
        if(!std::isfinite(value))return false;
        output[j]=value;
    }
    return true;
}
}
bool interpolate_tempo_curve(std::span<const float> grid,std::span<const float> values,float query,float& result,float fill) noexcept {
    if(grid.size()!=values.size() || std::isnan(query) || !std::isfinite(fill))return false;
    result=fill;
    return sample_tempo_curve(grid,values,query,result);
}
bool tempogram_ratios(std::span<const float> grid,std::span<const float> values,float bpm,std::span<const float> factors,std::span<float> output,float fill) noexcept {
    if(!std::isfinite(fill) || grid.size()!=values.size() || output.size()!=factors.size())return false;
    for(std::size_t i=0;i<factors.size();++i){
        const float query=bpm*factors[i];float value=fill;
        if(!std::isnan(query) && !interpolate_tempo_curve(grid,values,query,value,fill))return false;
        output[i]=harmonic_value(value,fill);
    }
    return true;
}
bool metrical_tempogram(std::span<const float> grid,std::span<const float> values,std::span<const float> factors,std::span<float> output) noexcept {
    if(grid.size()!=values.size() || (values.size() && factors.size()>SIZE_MAX/values.size()) || output.size()!=values.size()*factors.size())return false;
    for(std::size_t i=0;i<factors.size();++i)
        if(!metrical_row(grid,values,factors[i],output.subspan(i*values.size(),values.size())))return false;
    return true;
}
bool tempogram_ratios(const PreparedCurveInterpolation& curve,float bpm,std::span<const float> factors,std::span<float> output,float fill) noexcept {
    if(!std::isfinite(fill) || output.size()!=factors.size() || curve.values().empty())return false;
    for(std::size_t i=0;i<factors.size();++i){
        float value=0;if(!curve.sample(bpm*factors[i],value))return false;
        output[i]=harmonic_value(value,fill);
    }
    return true;
}
bool metrical_tempogram(const PreparedCurveInterpolation& curve,std::span<const float> grid,
    std::span<const float> values,std::span<const float> factors,std::span<float> output) noexcept {
    if(grid.size()!=values.size() || values.empty() || factors.size()>SIZE_MAX/values.size() || output.size()!=factors.size()*values.size())return false;
    for(std::size_t factor=0;factor<factors.size();++factor)for(std::size_t bin=0;bin<values.size();++bin){
        float value=0;if(!curve.sample(factors[factor]*grid[bin],value))return false;
        output[factor*values.size()+bin]=value*values[bin];
    }
    return true;
}
bool metrical_tempogram(const PreparedCurveInterpolation& curve,std::span<const float> factors,std::span<float> output) noexcept {
    const auto values=curve.values();
    if(values.empty() || factors.size()>SIZE_MAX/values.size() || output.size()!=values.size()*factors.size())return false;
    for(std::size_t i=0;i<factors.size();++i)
        if(!metrical_row(curve,factors[i],output.subspan(i*values.size(),values.size())))return false;
    return true;
}

}


namespace dsp_kernels {
bool PreparedTempoEstimator::storage_bytes(std::size_t frames,std::size_t lags,TempoEstimatorConfig c,std::size_t& out) noexcept {
    std::size_t bytes=0;
    if(!frames || !lags || !valid(c) || !prepared_storage::add_array(bytes,lags,3*sizeof(float)) ||
       !prepared_storage::add_array(bytes,frames,2*sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedTempoEstimator::workspace_bytes() const noexcept {
    return (row_.capacity()+aggregate_.capacity()+correction_.capacity()+column_.capacity()+result_.capacity())*sizeof(float);
}
}
