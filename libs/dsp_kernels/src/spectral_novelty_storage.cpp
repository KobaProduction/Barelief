#include <dsp_kernels/spectral_novelty.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dsp_kernels {
namespace {
bool difference_storage(std::size_t frames,std::size_t features,std::size_t width,std::size_t lag,std::size_t& bytes) noexcept {
    OnsetStrengthConfig c;c.center=false;c.aggregate=OnsetAggregate::Sum;c.max_size=std::max(std::size_t{1},width);c.lag=lag;
    if(!PreparedOnsetStrength::storage_bytes(features,frames,c,bytes))return false;
    bytes-=sizeof(PreparedOnsetStrength);return true;
}
bool novelty_geometry(std::size_t frames,std::size_t features,std::size_t bins,SpectralNoveltyConfig c) noexcept {
    if(!frames || !features || !bins || !c.lag || unsigned(c.method)>unsigned(SpectralNoveltyMethod::AlignedDifference) ||
       !std::isfinite(c.epsilon) || c.epsilon<=0)return false;
    const bool shaped=spectral_novelty_uses_phase(c.method) && c.method!=SpectralNoveltyMethod::PhaseMaskedDifference && c.method!=SpectralNoveltyMethod::PhaseDeviation;
    return !shaped || features==bins;
}
bool novelty_nested(std::size_t frames,std::size_t features,std::size_t bins,SpectralNoveltyConfig c,std::size_t supports,std::size_t& bytes) noexcept {
    if(c.method==SpectralNoveltyMethod::PhaseMaskedDifference){
        ComplexFluxConfig mask;mask.lag=c.lag;mask.frequency_maximum=c.frequency_maximum;
        mask.temporal_maximum=c.temporal_maximum;mask.temporal_origin=c.temporal_origin;mask.compensated_sum=false;
        return PreparedComplexFlux::storage_bytes(frames,features,bins,supports,mask,bytes);
    }
    if(c.method==SpectralNoveltyMethod::AlignedDifference)return PreparedSpectralAlignment::storage_bytes(features,c.alignment_radius,bytes);
    if(c.method==SpectralNoveltyMethod::SquaredDifference || c.method==SpectralNoveltyMethod::PositiveDifference || c.method==SpectralNoveltyMethod::MaximumDifference)
        return difference_storage(frames,features,c.method==SpectralNoveltyMethod::MaximumDifference?c.frequency_maximum:1,1,bytes);
    return PreparedSpectralDescriptor::storage_bytes(c.method==SpectralNoveltyMethod::PhaseDeviation?bins:features,bytes);
}
}
bool PreparedComplexFlux::storage_bytes(std::size_t frames,std::size_t bands,std::size_t bins,
    std::size_t support_count,ComplexFluxConfig c,std::size_t& output) noexcept {
    if(!frames || !bands || !bins || frames>std::size_t(PTRDIFF_MAX)/2 || !c.lag ||
       !std::isfinite(c.phase_step) || (support_count && support_count!=bands))return false;
    if(c.temporal_maximum && (c.temporal_maximum>std::size_t(PTRDIFF_MAX)/2 ||
       c.temporal_origin<-std::ptrdiff_t(c.temporal_maximum/2) || c.temporal_origin>std::ptrdiff_t((c.temporal_maximum-1)/2)))return false;
    std::size_t bytes=0;
    if(!difference_storage(frames,bands,c.frequency_maximum,c.lag,bytes) ||
       !prepared_storage::add_array(bytes,bands,3*sizeof(float)) ||
       !prepared_storage::add_array(bytes,bins,3*sizeof(float)) ||
       !prepared_storage::add_array(bytes,support_count,sizeof(FilterBandSupport)))return false;
    output=bytes;return true;
}
std::size_t PreparedComplexFlux::workspace_bytes() const noexcept {
    return difference_.workspace_bytes()+supports_.capacity()*sizeof(FilterBandSupport)+
        (current_.capacity()+previous_.capacity()+difference_values_.capacity()+phase_.capacity()+delay_.capacity()+maximum_.capacity())*sizeof(float);
}
bool PreparedSpectralAlignment::storage_bytes(std::size_t bins,std::size_t radius,std::size_t& output) noexcept {
    if(!radius || bins>std::size_t(PTRDIFF_MAX)/2 || radius>=(bins+1)/2)return false;
    std::size_t bytes=0;
    if(!prepared_storage::add_array(bytes,2*radius+1,sizeof(float)) || !prepared_storage::add_array(bytes,bins,sizeof(float)))return false;
    output=bytes;return true;
}
bool PreparedSpectralNovelty::storage_bytes(std::size_t frames,std::size_t features,std::size_t bins,
    SpectralNoveltyConfig c,std::size_t supports,std::size_t& output) noexcept {
    if(!novelty_geometry(frames,features,bins,c))return false;
    std::size_t bytes=0;
    if(!novelty_nested(frames,features,bins,c,supports,bytes) ||
       !prepared_storage::add_array(bytes,features,4*sizeof(float)) || !prepared_storage::add_array(bytes,bins,2*sizeof(float)) ||
       !prepared_storage::add_array(bytes,c.method==SpectralNoveltyMethod::AlignedDifference?features:1,sizeof(float)))return false;
    output=bytes;return true;
}
std::size_t PreparedSpectralNovelty::workspace_bytes() const noexcept {
    return descriptor_.workspace_bytes()+difference_.workspace_bytes()+phase_mask_.workspace_bytes()+alignment_.workspace_bytes()+
        (current_.capacity()+previous_.capacity()+differences_.capacity()+phase_.capacity()+unit_magnitude_.capacity()+ones_.capacity()+result_.capacity())*sizeof(float);
}
}
