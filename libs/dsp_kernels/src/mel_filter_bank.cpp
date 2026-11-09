// Adapted from librosa 1.0.0 filters.mel, core.convert Hz/Mel and
// feature.spectral.melspectrogram(S=...). ISC license and full notice:
// packaging/licenses/librosa.txt, shipped with Barelief and the portable SDK.
// Copyright (c) 2013--2026, librosa development team.
// https://github.com/librosa/librosa/blob/1.0.0/librosa/filters.py
#include <dsp_kernels/mel_filter_bank.h>
#include <dsp_kernels/filter_projection.h>
#include <dsp_kernels/discrete_triangle.h>
#include <dsp_kernels/frequency_bins.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dsp_kernels {
namespace {
double to_mel(double hz,bool htk) noexcept {
    if(htk)return 2595*std::log10(1+hz/700);
    return hz<1000?hz/(200./3):15+std::log(hz/1000)/(std::log(6.4)/27);
}
double from_mel(double mel,bool htk) noexcept {
    if(htk)return 700*(std::pow(10.,mel/2595)-1);
    return mel<15?(200./3)*mel:1000*std::exp((std::log(6.4)/27)*(mel-15));
}
bool valid_construction(const MelFilterBankConfig& c) noexcept {
    if(c.construction==MelConstruction::Continuous)return true;
    return c.construction==MelConstruction::NearestBinTriangles && c.weighting==MelWeighting::Frequency &&
        (c.normalization==MelNormalization::None || (c.normalization==MelNormalization::Lp && c.norm_p==1));
}
bool valid(const MelFilterBankConfig& c) noexcept {
    const auto high=c.fmax_hz?c.fmax_hz:c.sample_rate/2;
    return c.bands && std::isfinite(c.bin_hz) && c.bin_hz>0 && unsigned(c.scale)<=unsigned(MelScale::Slaney) && std::isfinite(c.sample_rate) && c.sample_rate>0 &&
        std::isfinite(c.fmin_hz) && c.fmin_hz>=0 && std::isfinite(high) && high>c.fmin_hz &&
        unsigned(c.normalization)<=unsigned(MelNormalization::TriangleArea) && unsigned(c.weighting)<=unsigned(MelWeighting::Scale) &&
        (c.normalization!=MelNormalization::Lp || (std::isfinite(c.norm_p) && c.norm_p>0)) && valid_construction(c);
}
bool valid_grid(std::size_t bins,const MelFilterBankConfig& c,std::span<const double> values) noexcept {
    if(!values.empty() && values.size()!=bins)return false;
    double previous=-1;
    for(std::size_t bin=0;bin<bins;++bin){
        const auto hz=values.empty()?(double(bin)+double(c.first_bin))*c.bin_hz:values[bin];
        if(!std::isfinite(hz) || hz<0 || hz<=previous || hz>c.sample_rate/2)return false;
        previous=hz;
    }
    return true;
}
bool valid_explicit_weights(std::span<const float> weights,std::span<const FilterBandSupport> supports,std::size_t bins) noexcept {
    if(!std::all_of(weights.begin(),weights.end(),[](float x){return std::isfinite(x);}))return false;
    for(const auto& support:supports)if(support.first>bins || support.count>bins-support.first)return false;
    return true;
}
bool append_bytes(std::size_t count,std::size_t width,std::size_t& bytes) noexcept {
    if(count>(SIZE_MAX-bytes)/width)return false;
    bytes+=count*width;return true;
}
}
bool MelFilterBank::storage_bytes(std::size_t bins,const MelFilterBankConfig& c,std::size_t& out) noexcept {
    if(!bins || !valid(c) || c.bands>SIZE_MAX-2 || c.bands>SIZE_MAX/bins)return false;
    std::size_t bytes=sizeof(MelFilterBank);
    if(!append_bytes(c.bands*bins,sizeof(float),bytes) ||
       !append_bytes(c.bands+2,sizeof(double),bytes) || !append_bytes(c.bands,sizeof(float),bytes) || !append_bytes(c.bands,sizeof(FilterBandSupport),bytes) || !append_bytes(c.bands,sizeof(double),bytes))return false;
    out=bytes;return true;
}
bool MelFilterBank::matrix_storage_bytes(std::size_t bins,std::size_t bands,std::size_t& out) noexcept {
    MelFilterBankConfig config;config.bands=bands;std::size_t bytes=0;
    if(!storage_bytes(bins,config,bytes))return false;
    out=bytes-sizeof(MelFilterBank);return true;
}
namespace {
bool count_mel_corners(FrequencyGridView grid,const MelFilterBankConfig& c,std::size_t& count) noexcept {
    const auto low=to_mel(c.fmin_hz,c.scale==MelScale::Htk),high=to_mel(c.fmax_hz?c.fmax_hz:c.sample_rate/2,c.scale==MelScale::Htk);
    const auto step=(high-low)/double(c.bands+1);
    std::size_t previous=SIZE_MAX;
    for(std::size_t i=0;i<c.bands+2;++i){
        const auto hz=from_mel(i==c.bands+1?high:low+double(i)*step,c.scale==MelScale::Htk);
        const auto bin=nearest_frequency_bin(grid,hz);
        if(bin==SIZE_MAX)return false;
        if(!c.unique_filters || previous!=bin){++count;previous=bin;}
    }
    return true;
}
}
bool MelFilterBank::band_count(FrequencyGridView grid,const MelFilterBankConfig& c,std::size_t& out) noexcept {
    if(!grid.size || !grid.at || !valid(c) || c.bands>SIZE_MAX-2)return false;
    if(c.construction==MelConstruction::Continuous){out=c.bands;return true;}
    std::size_t count=0;
    if(!count_mel_corners(grid,c,count))return false;
    if(count<3)return false;
    out=count-2;return true;
}
void MelFilterBank::build_boundaries() {
    const auto low=to_mel(config_.fmin_hz,config_.scale==MelScale::Htk),high=to_mel(config_.fmax_hz?config_.fmax_hz:config_.sample_rate/2,config_.scale==MelScale::Htk);
    const auto step=(high-low)/double(config_.bands+1);
    for(std::size_t i=0;i<boundaries_.size();++i)boundaries_[i]=from_mel(i+1==boundaries_.size()?high:low+double(i)*step,config_.scale==MelScale::Htk);
}
double MelFilterBank::coordinate(double hz) const noexcept {
    return config_.weighting==MelWeighting::Scale?to_mel(hz,config_.scale==MelScale::Htk):hz;
}
void MelFilterBank::build_filter(std::size_t band,std::span<const double> frequencies) {
    const auto low=coordinate(boundaries_[band]),mid=coordinate(boundaries_[band+1]),high=coordinate(boundaries_[band+2]);
    for(std::size_t bin=0;bin<bins_;++bin){
        const auto hz=frequencies.empty()?(double(bin)+double(config_.first_bin))*config_.bin_hz:frequencies[bin];
        const auto x=coordinate(hz);
        weights_[band*bins_+bin]=float(std::max(0.,std::min((x-low)/(mid-low),(high-x)/(high-mid))));
    }
    normalize_filter(band);
}
bool MelFilterBank::valid_boundaries() const noexcept {
    double previous=-1;
    for(const auto hz:boundaries_){
        if(!std::isfinite(hz) || hz<=previous)return false;
        previous=hz;
    }
    return true;
}
double MelFilterBank::filter_norm(std::span<const float> row) const noexcept {
    if(config_.normalization==MelNormalization::Maximum)return *std::max_element(row.begin(),row.end());
    if(config_.normalization==MelNormalization::Minimum)return *std::min_element(row.begin(),row.end());
    double length=0;
    for(const auto value:row)length+=config_.normalization==MelNormalization::L0?double(value>0):std::pow(double(value),config_.norm_p);
    return config_.normalization==MelNormalization::L0?length:std::pow(length,1/config_.norm_p);
}
void MelFilterBank::normalize_filter(std::size_t band) {
    auto row=std::span(weights_).subspan(band*bins_,bins_);double scale=1;
    if(config_.normalization==MelNormalization::None)return;
    if(config_.normalization==MelNormalization::Slaney)scale=2/(boundaries_[band+2]-boundaries_[band]);
    else if(config_.normalization==MelNormalization::TriangleArea)scale=2/(coordinate(boundaries_[band+2])-coordinate(boundaries_[band]));
    else{const auto length=filter_norm(row);if(length>=std::numeric_limits<float>::min())scale=1/length;}
    for(auto& value:row)value=float(double(value)*scale);
}
void MelFilterBank::allocate_storage(std::size_t bins,const MelFilterBankConfig& config) {
    config_=config;bins_=bins;
    boundaries_.resize(config_.bands+2);centers_.resize(config_.bands);
    weights_.resize(config_.bands*bins_);
    staging_.resize(config_.bands);supports_.assign(config_.bands,{0,bins});
}
bool MelFilterBank::build_filters(std::span<const double> frequencies) {
    build_boundaries();if(!valid_boundaries())return false;
    std::copy_n(boundaries_.begin()+1,config_.bands,centers_.begin());
    for(std::size_t band=0;band<config_.bands;++band)build_filter(band,frequencies);
    return std::all_of(weights_.begin(),weights_.end(),[](float v){return std::isfinite(v);});
}
// Discrete triangle construction also follows madmom audio/filters.py at
// 27f032e8947204902c675e5e341a3faf5dc86dae; BSD notice: packaging/licenses/madmom.txt.
std::vector<std::size_t> MelFilterBank::binned_corners(std::span<const double> frequencies) const {
    std::vector<std::size_t> corners;
    corners.reserve(boundaries_.size());
    for(const auto hz:boundaries_){
        const auto bin=nearest_frequency_bin(frequencies,hz);
        if(!config_.unique_filters || corners.empty() || corners.back()!=bin)corners.push_back(bin);
    }
    return corners;
}
bool MelFilterBank::build_binned_filters(std::span<const double> frequencies) {
    build_boundaries();if(!valid_boundaries())return false;
    std::vector<double> generated;
    if(frequencies.empty()){
        generated.resize(bins_);
        for(std::size_t i=0;i<bins_;++i)generated[i]=(double(i)+double(config_.first_bin))*config_.bin_hz;
        frequencies=generated;
    }
    const auto corners=binned_corners(frequencies);
    return corners.size()>=3 && bind_binned_filters(corners,frequencies);
}
bool MelFilterBank::bind_binned_filters(std::span<const std::size_t> corners,std::span<const double> frequencies) {
    const auto count=corners.size()-2;
    weights_.resize(count*bins_);centers_.resize(count);supports_.resize(count);staging_.resize(count);
    const bool normalize=config_.normalization==MelNormalization::Lp;
    for(std::size_t band=0;band<count;++band){
        DiscreteTriangleGeometry geometry;
        if(!discrete_triangle_filter(corners[band],corners[band+1],corners[band+2],normalize,
            std::span(weights_).subspan(band*bins_,bins_),geometry))return false;
        centers_[band]=frequencies[geometry.center];supports_[band]=geometry.nonzero;
    }
    return true;
}
bool MelFilterBank::prepare(std::size_t bins,const MelFilterBankConfig& config,std::span<const double> frequencies) {
    std::size_t bytes=0;if(!storage_bytes(bins,config,bytes) || !valid_grid(bins,config,frequencies))return false;
    MelFilterBank next;next.allocate_storage(bins,config);
    const bool built=config.construction==MelConstruction::NearestBinTriangles?
        next.build_binned_filters(frequencies):next.build_filters(frequencies);
    if(!built || next.workspace_bytes()>bytes-sizeof(MelFilterBank))return false;
    *this=std::move(next);return true;
}
void MelFilterBank::assign_matrix(std::span<const float> weights,std::span<const double> centers,FilterProjectionOptions options,std::span<const FilterBandSupport> supports) {
    std::copy(weights.begin(),weights.end(),weights_.begin());
    if(!supports.empty())std::copy(supports.begin(),supports.end(),supports_.begin());
    centers_.assign(centers.begin(),centers.end());
    projection_=options;
}
bool MelFilterBank::prepare_matrix(std::size_t bins,std::size_t bands,std::span<const float> weights,
                                   std::span<const double> centers,FilterProjectionOptions options,
                                   std::span<const FilterBandSupport> supports) {
    MelFilterBankConfig config;config.bands=bands;std::size_t bytes=0;
    if(!valid_filter_projection_options(options) || !storage_bytes(bins,config,bytes) || weights.size()!=bands*bins ||
       (!supports.empty() && supports.size()!=bands) || (!centers.empty() && centers.size()!=bands))return false;
    if(!valid_explicit_weights(weights,supports,bins) ||
       !std::all_of(centers.begin(),centers.end(),[](double hz){return std::isfinite(hz) && hz>=0;}))return false;
    MelFilterBank next;next.allocate_storage(bins,config);
    next.assign_matrix(weights,centers,options,supports);
    if(next.workspace_bytes()>bytes-sizeof(MelFilterBank))return false;
    *this=std::move(next);return true;
}
bool MelFilterBank::prepare_weights(std::size_t bins,std::span<const float> weights,std::span<const double> boundaries,
                                    FilterProjectionOptions options,std::span<const FilterBandSupport> supports) {
    if(boundaries.size()<3)return false;
    MelFilterBank next;
    if(!next.prepare_matrix(bins,boundaries.size()-2,weights,boundaries.subspan(1,boundaries.size()-2),options,supports))return false;
    next.boundaries_.assign(boundaries.begin(),boundaries.end());
    if(boundaries.front()<0 || !next.valid_boundaries())return false;
    *this=std::move(next);return true;
}
bool MelFilterBank::prepare(std::size_t bins,std::size_t sample_rate,std::size_t bands,std::span<const double> frequencies) {
    MelFilterBankConfig config;config.sample_rate=double(sample_rate);config.bands=bands;
    config.bin_hz=bins?double(sample_rate)/double(bins)/2:0;config.first_bin=0;
    return prepare(bins,config,frequencies);
}
bool MelFilterBank::project_band(std::span<const float> input,std::size_t band,float& output) const noexcept {
    const auto support=supports_[band];
    return project_filter_weights(input.subspan(support.first,support.count),
        std::span(weights_).subspan(band*bins_+support.first,support.count),output,projection_);
}
bool MelFilterBank::process(std::span<const float> input,std::span<float> output) const noexcept {
    if(!bins_ || input.size()!=bins_ || output.size()!=staging_.size())return false;
    if(!std::all_of(input.begin(),input.end(),[](float v){return std::isfinite(v);}))return false;
    for(std::size_t band=0;band<staging_.size();++band)
        if(!project_band(input,band,staging_[band]))return false;
    std::copy(staging_.begin(),staging_.end(),output.begin());return true;
}
}
