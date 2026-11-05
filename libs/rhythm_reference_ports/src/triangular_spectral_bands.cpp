// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of Essentia TriangularBands/SuperFlux defaults at
// b9fa6cb674ca43dfb94d28d293aeda441c6745db.
// Copyright (C) 2006-2021 Music Technology Group, Universitat Pompeu Fabra.
// Float32 decomposition Copyright (C) 2026 Koba Production. See ../AGPL-COPYING.
#include <rhythm_reference_ports/triangular_spectral_bands.h>
#include <dsp_kernels/log_compression.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace rhythm_reference_ports {
namespace {
float warped(float hz,TriangleWarp weighting) noexcept {
    if(weighting==TriangleWarp::NaturalLogMel)return 1127.01048f*std::log(hz/700.f+1.f);
    if(weighting==TriangleWarp::Log10Mel)return 2595.f*std::log10(hz/700.f+1.f);
    return hz;
}
bool valid(std::size_t bins,std::span<const float> edges,TriangularSpectralBandConfig config) noexcept {
    if(bins<2 || bins>std::size_t(std::numeric_limits<int>::max()) || edges.size()<3 || edges.size()-2>SIZE_MAX/bins)return false;
    if(!std::isfinite(config.sample_rate) || config.sample_rate<=0 || unsigned(config.weighting)>unsigned(TriangleWarp::Log10Mel) ||
       unsigned(config.normalization)>unsigned(TriangleNormalization::UnitMaximum))return false;
    float previous=-1;
    for(const float edge:edges){
        if(!std::isfinite(edge) || edge<=previous)return false;
        previous=edge;
    }
    return edges.front()>=0;
}
bool support(std::span<const float> edges,float step,std::size_t bins,dsp_kernels::FilterBandSupport& result) noexcept {
    const float first=std::ceil(edges[0]/step),last=std::floor(edges[2]/step);
    if(!std::isfinite(first) || !std::isfinite(last) || first<0 || last<first || last>=float(bins))return false;
    result={std::size_t(first),std::size_t(last-first)+1};return true;
}
bool triangle(std::span<const float> edges,float step,TriangularSpectralBandConfig config,
              std::size_t bins,std::span<float> weights,dsp_kernels::FilterBandSupport& bounds) noexcept {
    if(!support(edges,step,bins,bounds))return false;
    const float low=warped(edges[0],config.weighting),mid=warped(edges[1],config.weighting),high=warped(edges[2],config.weighting);
    const float rise=mid-low,fall=high-mid;
    if(!std::isfinite(rise) || !std::isfinite(fall) || rise<=0 || fall<=0)return false;
    float sum=0;
    for(std::size_t j=bounds.first;j<bounds.first+bounds.count;++j){
        const float hz=float(j)*step;
        const float value=hz<edges[1]?(warped(hz,config.weighting)-low)/rise:(high-warped(hz,config.weighting))/fall;
        if(!weights.empty())weights[j]=value;
        sum+=value;
    }
    if(!std::isfinite(sum) || sum==0)return false;
    if(config.normalization==TriangleNormalization::UnitTriangle)sum=(rise+fall)/2.f;
    if(!weights.empty() && config.normalization!=TriangleNormalization::UnitMaximum)
        for(std::size_t j=bounds.first;j<bounds.first+bounds.count;++j)weights[j]/=sum;
    return true;
}
}

bool warped_band_edges(std::span<float> output,float low,float high,TriangleWarp warp) noexcept {
    if(output.size()<3 || !std::isfinite(low) || !std::isfinite(high) || low<0 || high<=low ||
       unsigned(warp)>unsigned(TriangleWarp::Log10Mel))return false;
    float coordinate=warped(low,warp);
    const float step=(warped(high,warp)-coordinate)/float(output.size()-1);
    for(auto& value:output){
        if(warp==TriangleWarp::Linear)value=coordinate;
        else if(warp==TriangleWarp::NaturalLogMel)value=700.f*(std::exp(coordinate/1127.01048f)-1.f);
        else value=700.f*(std::pow(10.f,coordinate/2595.f)-1.f);
        if(!std::isfinite(value))return false;
        coordinate+=step;
    }
    return true;
}

bool PreparedTriangularSpectralBands::configure(std::size_t bins,std::span<const float> edges,TriangularSpectralBandConfig config) {
    if(!valid(bins,edges,config))return false;
    const float step=(config.sample_rate/2.f)/float(bins-1);
    if(!std::isfinite(step) || step<=0)return false;
    const auto bands=edges.size()-2;
    std::vector<float> weights(bands*bins,0.f);
    std::vector<dsp_kernels::FilterBandSupport> supports(bands);
    for(std::size_t i=0;i<bands;++i)
        if(!triangle(edges.subspan(i,3),step,config,bins,std::span(weights).subspan(i*bins,bins),supports[i]))return false;
    const std::vector<double> coordinates(edges.begin(),edges.end());
    dsp_kernels::FilterProjectionOptions options;options.compensated=false;options.square_input=config.square_magnitude;
    if(!projection_.prepare_weights(bins,weights,coordinates,options,supports))return false;
    logarithm_=config.logarithm;result_.resize(bands);return true;
}
bool PreparedTriangularSpectralBands::prepare(std::size_t bins,std::span<const float> edges,TriangularSpectralBandConfig config) {
    PreparedTriangularSpectralBands next;std::size_t bytes=0;
    if(!storage_bytes(bins,edges,config,bytes) || !next.configure(bins,edges,config) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedTriangularSpectralBands::storage_bytes(std::size_t bins,std::span<const float> edges,
    TriangularSpectralBandConfig config,std::size_t& output) noexcept {
    if(!valid(bins,edges,config))return false;
    const float step=(config.sample_rate/2.f)/float(bins-1);
    if(!std::isfinite(step) || step<=0)return false;
    dsp_kernels::FilterBandSupport bounds;const auto bands=edges.size()-2;
    // Reuse the same coefficient construction/zero-sum rejection, without
    // storing coefficients or allocating a preparation prototype.
    for(std::size_t i=0;i<bands;++i)if(!triangle(edges.subspan(i,3),step,config,bins,{},bounds))return false;
    std::size_t bytes=0;
    using namespace dsp_kernels::prepared_storage;
    if(!dsp_kernels::MelFilterBank::matrix_storage_bytes(bins,bands,bytes) ||
       !add_array(bytes,bands,sizeof(float)) || !add_array(bytes,bands*bins,sizeof(float)) ||
       !add_array(bytes,bands,sizeof(dsp_kernels::FilterBandSupport)) || !add_array(bytes,edges.size(),sizeof(double)))return false;
    output=bytes;return true;
}
bool PreparedTriangularSpectralBands::process(std::span<const float> input,std::span<float> output) noexcept {
    if(output.size()!=result_.size() || result_.empty() || !projection_.process(input,result_))return false;
    if(logarithm_ && !dsp_kernels::logarithmic_compression(result_,result_,1,1,dsp_kernels::LogarithmBase::Two,true))return false;
    if(!std::all_of(result_.begin(),result_.end(),[](float x){return std::isfinite(x);}))return false;
    std::copy(result_.begin(),result_.end(),output.begin());return true;
}
}
