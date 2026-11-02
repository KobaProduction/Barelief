#include "spectral_sequence.h"
#include <algorithm>
#include <cmath>

namespace dsp_graph_adapters::detail {
using namespace dsp_kernels;
using namespace signal_contracts;
namespace {
bool signed_setting(const domain::ResolvedProcessingNode& n,std::string_view key,std::ptrdiff_t& output) noexcept {
    float value=0;if(!number(n,key,0,value) || value!=std::trunc(value) || value<-65536 || value>65536)return false;
    output=std::ptrdiff_t(value);return true;
}
bool novelty_settings(const domain::ResolvedProcessingNode& n,SpectralNoveltyConfig& c) noexcept {
    std::size_t method=2;
    if(!integer(n,"method",2,method) || method>11 || !integer(n,"lag",1,c.lag) ||
       !integer(n,"frequency_maximum",3,c.frequency_maximum) || !integer(n,"temporal_maximum",3,c.temporal_maximum) ||
       !signed_setting(n,"temporal_origin",c.temporal_origin) || !integer(n,"alignment_radius",1,c.alignment_radius))return false;
    c.method=SpectralNoveltyMethod(method);
    return number(n,"epsilon",2.220446049250313e-16f,c.epsilon) && flag(n,"supplied_differences",false,c.supplied_differences) &&
        flag(n,"positive_alignment",false,c.positive_alignment);
}
bool frame_settings(const domain::ResolvedProcessingNode& n,SpectralFrameRecipeConfig& c) noexcept {
    std::size_t window=0,tail=0,padding=0;float hop=441;
    if(!number(n,"sample_rate",44100,c.sample_rate) || !integer(n,"frame_length",2048,c.framing.length) ||
       !number(n,"hop_samples",441,hop) || !integer(n,"fft_size",0,c.fft_size) || !integer(n,"window_samples",0,c.window_samples) ||
       !integer(n,"window_function",0,window) || window>6 || !integer(n,"tail",0,tail) || tail>2 ||
       !integer(n,"padding",0,padding) || padding>4 || !signed_setting(n,"origin",c.framing.origin))return false;
    c.framing.hop_samples=hop;c.window=WindowFunction(window);c.framing.tail=FrameSamplingTail(tail);c.padding=TemporalPadding(padding);
    return flag(n,"periodic",false,c.periodic) && flag(n,"center",true,c.framing.center) &&
        flag(n,"fixed_frames",false,c.framing.fixed_frames) && integer(n,"frame_count",0,c.framing.frame_count) &&
        flag(n,"include_nyquist",false,c.include_nyquist) && flag(n,"circular_shift",false,c.circular_shift);
}
bool value_settings(const domain::ResolvedProcessingNode& n,SpectralFrameRecipeConfig& c) noexcept {
    std::size_t norm=0,zero=1,filter=0,base=0;
    if(!number(n,"power",1,c.power) || !integer(n,"magnitude_norm",0,norm) || norm>1 || !integer(n,"zero_phase",1,zero) || zero>1 ||
       !integer(n,"filtering",0,filter) || filter>3 || !integer(n,"logarithm_base",0,base) || base>2)return false;
    c.magnitude_norm=ComplexMagnitudeNorm(norm);c.zero_phase=ZeroMagnitudePhase(zero);c.filtering=SpectralFilterMode(filter);c.logarithm_base=LogarithmBase(base);
    return flag(n,"physical_filter_axis",false,c.physical_filter_axis) && flag(n,"logarithm",false,c.logarithm) &&
        number(n,"logarithm_multiply",1,c.logarithm_multiply) && number(n,"logarithm_add",1,c.logarithm_add);
}
bool filter_settings(const domain::ResolvedProcessingNode& n,SpectralFrameRecipeConfig& c) noexcept {
    auto& log=c.logarithmic_filters;
    if(!number(n,"bands_per_octave",12,log.bands_per_octave) || !number(n,"minimum_hz",30,log.fmin) ||
       !number(n,"maximum_hz",17000,log.fmax) || !number(n,"reference_hz",440,log.reference) ||
       !flag(n,"unique_filters",true,log.unique_filters) || !flag(n,"normalize_filters",true,log.normalize))return false;
    auto& mel=c.mel_filters;std::size_t scale=0,norm=3,weight=0,construction=1;float p=1;
    if(!integer(n,"mel_bands",12,mel.bands) || !integer(n,"mel_scale",0,scale) || scale>1 ||
       !integer(n,"mel_normalization",3,norm) || norm>6 || !integer(n,"mel_weighting",0,weight) || weight>1 ||
       !integer(n,"mel_construction",1,construction) || construction>1 || !number(n,"mel_norm_p",1,p))return false;
    mel.fmin_hz=log.fmin;mel.fmax_hz=log.fmax;mel.unique_filters=log.unique_filters;mel.norm_p=p;
    mel.scale=MelScale(scale);mel.normalization=MelNormalization(norm);mel.weighting=MelWeighting(weight);mel.construction=MelConstruction(construction);return true;
}
bool peak_settings(const domain::ResolvedProcessingNode& n,ActivationPeakConfig& c) noexcept {
    c.smoothing_crop=SameConvolutionCrop::Input;
    return number(n,"threshold",.5f,c.threshold) && integer(n,"pre_average",0,c.pre_average) &&
        integer(n,"post_average",0,c.post_average) && integer(n,"pre_maximum",1,c.pre_maximum) && integer(n,"post_maximum",1,c.post_maximum);
}
bool pcm_geometry(const domain::ResolvedProcessingNode& n,SpectralSequencePlan& p) noexcept {
    std::size_t one=0;
    if(!input_geometry(n,0,Rank::Vector,p.samples,one) || !frame_settings(n,p.pcm.spectrum) ||
       !value_settings(n,p.pcm.spectrum) || !filter_settings(n,p.pcm.spectrum) ||
       !flag(n,"automatic_difference_lag",true,p.pcm.automatic_difference_lag) || !number(n,"difference_overlap",.5f,p.pcm.difference_overlap))return false;
    if(!integer(n,"explicit_bands",1,p.filters.bands))return false;
    p.filters.weights=array(n,"weights");
    const auto centers=array(n,"centers_hz");
    if(!centers.empty() && (centers.size()!=p.filters.bands || !std::all_of(centers.begin(),centers.end(),[](float x){return std::isfinite(x);})))return false;
    if(spectral_novelty_uses_phase(p.pcm.novelty.method))p.pcm.spectrum.circular_shift=true;
    const auto fft=p.pcm.spectrum.fft_size?p.pcm.spectrum.fft_size:p.pcm.spectrum.framing.length;
    p.bins=fft/2+std::size_t(p.pcm.spectrum.include_nyquist);
    return PreparedSpectralFrameRecipe::geometry(p.samples,p.pcm.spectrum,p.filters,p.frames,p.features) &&
        PreparedPcmSpectralNovelty::storage_bytes(p.samples,p.pcm,array(n,"coefficients"),p.filters,p.kernel_bytes);
}
}
bool spectral_supports(const domain::ResolvedProcessingNode& n,std::size_t features,std::size_t bins) noexcept {
    const auto first=array(n,"support_first"),count=array(n,"support_count");
    if(first.empty() && count.empty())return true;
    if(first.size()!=features || count.size()!=features)return false;
    for(std::size_t i=0;i<features;++i)
        if(!std::isfinite(first[i]) || !std::isfinite(count[i]) || first[i]<0 || count[i]<1 ||
           first[i]!=std::trunc(first[i]) || count[i]!=std::trunc(count[i]) || double(first[i])+count[i]>double(bins))return false;
    return true;
}
std::vector<FilterBandSupport> own_spectral_supports(const domain::ResolvedProcessingNode& n) {
    const auto first=array(n,"support_first"),count=array(n,"support_count");std::vector<FilterBandSupport> result;result.reserve(first.size());
    for(std::size_t i=0;i<first.size();++i)result.push_back({std::size_t(first[i]),std::size_t(count[i])});
    return result;
}
bool spectral_sequence_plan(const domain::ResolvedProcessingNode& n,SpectralSequencePlan& p) noexcept {
    p={};p.pcm_input=n.kind=="PcmSpectralNovelty";p.matrix_peaks=n.kind=="ActivationPeakMatrix";
    if(p.matrix_peaks)return input_geometry(n,0,Rank::Matrix,p.frames,p.features) && peak_settings(n,p.peaks) &&
        PreparedActivationPeakMatrix::storage_bytes(p.frames,p.features,p.peaks,array(n,"smoothing"),p.kernel_bytes);
    if(!novelty_settings(n,p.pcm.novelty))return false;
    if(p.pcm_input){if(!pcm_geometry(n,p))return false;}
    else {
        if(!input_geometry(n,0,Rank::Matrix,p.frames,p.features))return false;
        p.bins=p.features;std::size_t frames=0;
        if(contract(n,domain::PortDirection::Input,1) && (!input_geometry(n,1,Rank::Matrix,frames,p.bins) || frames!=p.frames))return false;
        if(spectral_novelty_uses_phase(p.pcm.novelty.method) && !contract(n,domain::PortDirection::Input,1))return false;
        const auto supports=array(n,"support_first").size();
        if(p.pcm.novelty.method==SpectralNoveltyMethod::PhaseMaskedDifference && p.features!=p.bins && supports!=p.features)return false;
        if(!spectral_supports(n,p.features,p.bins) || !PreparedSpectralNovelty::storage_bytes(p.frames,p.features,p.bins,p.pcm.novelty,supports,p.kernel_bytes))return false;
    }
    p.columns=p.pcm.novelty.method==SpectralNoveltyMethod::AlignedDifference?p.features:1;
    return true;
}
SpectralOutputShape spectral_output_shape(const SpectralSequencePlan& p,domain::PortId port) noexcept {
    if(p.matrix_peaks)return {Rank::Vector,p.frames*p.features,1,DType::UInt64};
    if(!port)return {Rank::Matrix,std::max(std::size_t{1},p.frames),p.columns};
    const auto mapped=p.pcm_input?port-4:port;
    if(p.pcm_input && port<4)return port==1?SpectralOutputShape{Rank::Matrix,p.bins,2}:SpectralOutputShape{Rank::Vector,p.bins};
    if(mapped==4)return {Rank::Scalar,1,1,DType::Int64};
    if(mapped==3)return {Rank::Vector,2*p.pcm.novelty.alignment_radius+1};
    return {Rank::Vector,mapped==2?p.bins:p.features};
}
bool spectral_sequence_outputs(const domain::ResolvedProcessingNode& n,const SpectralSequencePlan& p) noexcept {
    for(domain::PortId i=0;i<n.outputs.size();++i){const auto shape=spectral_output_shape(p,i);
        if(!output_geometry(n,i,shape.rank,shape.rows,shape.columns,shape.dtype))return false;
    }
    return n.outputs.size()==(p.matrix_peaks?2u:p.pcm_input?9u:5u);
}
}
