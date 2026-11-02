#include <domain/numeric/sequence_operations.h>
#include <domain/graph_definition.h>
#include <array>

namespace domain {
namespace {
using C=ParameterClassification;using V=ParameterValueKind;
constexpr ParameterDescriptor novelty[]{
    {"frame_rate",C::Processing,V::Number,0.000001,192000,100,{}},
    {"method",C::Processing,V::Integer,0,11,2,{}},
    {"lag",C::Processing,V::Integer,1,65536,1,{}},
    {"frequency_maximum",C::Processing,V::Integer,0,65536,3,{}},
    {"temporal_maximum",C::Processing,V::Integer,0,65536,3,{}},
    {"temporal_origin",C::Processing,V::Integer,-65536,65536,0,{}},
    {"alignment_radius",C::Processing,V::Integer,1,65536,1,{}},
    {"epsilon",C::Processing,V::Number,1e-30,1e20,2.220446049250313e-16,{}},
    {"supplied_differences",C::Processing,V::Boolean,0,1,0,{}},
    {"positive_alignment",C::Processing,V::Boolean,0,1,0,{}},
    {"support_first",C::Processing,V::NumericArray,0,0,0,{}},
    {"support_count",C::Processing,V::NumericArray,0,0,0,{}}
};
constexpr ParameterDescriptor pcm[]{
    {"sample_rate",C::Processing,V::Number,1,192000,44100,{}},
    {"frame_length",C::Processing,V::Integer,2,65536,2048,{}},
    {"hop_samples",C::Processing,V::Number,0.000001,65536,441,{}},
    {"fft_size",C::Processing,V::Integer,0,65536,0,{}},
    {"window_samples",C::Processing,V::Integer,0,65536,0,{}},
    {"window_function",C::Processing,V::Integer,0,6,0,{}},
    {"periodic",C::Processing,V::Boolean,0,1,0,{}},
    {"center",C::Processing,V::Boolean,0,1,1,{}},
    {"tail",C::Processing,V::Integer,0,2,0,{}},
    {"origin",C::Processing,V::Integer,-65536,65536,0,{}},
    {"fixed_frames",C::Processing,V::Boolean,0,1,0,{}},
    {"frame_count",C::Processing,V::Integer,0,65536,0,{}},
    {"include_nyquist",C::Processing,V::Boolean,0,1,0,{}},
    {"circular_shift",C::Processing,V::Boolean,0,1,0,{}},
    {"padding",C::Processing,V::Integer,0,4,0,{}},
    {"power",C::Processing,V::Number,-16,16,1,{}},
    {"magnitude_norm",C::Processing,V::Integer,0,1,0,{}},
    {"zero_phase",C::Processing,V::Integer,0,1,1,{}},
    {"filtering",C::Processing,V::Integer,0,3,0,{}},
    {"physical_filter_axis",C::Processing,V::Boolean,0,1,0,{}},
    {"logarithm",C::Processing,V::Boolean,0,1,0,{}},
    {"logarithm_multiply",C::Processing,V::Number,-1e20,1e20,1,{}},
    {"logarithm_add",C::Processing,V::Number,-1e20,1e20,1,{}},
    {"logarithm_base",C::Processing,V::Integer,0,2,0,{}},
    {"bands_per_octave",C::Processing,V::Number,0.000001,1024,12,{}},
    {"minimum_hz",C::Processing,V::Number,0,96000,30,{}},
    {"maximum_hz",C::Processing,V::Number,0,192000,17000,{}},
    {"reference_hz",C::Processing,V::Number,0.000001,192000,440,{}},
    {"unique_filters",C::Processing,V::Boolean,0,1,1,{}},
    {"normalize_filters",C::Processing,V::Boolean,0,1,1,{}},
    {"mel_bands",C::Processing,V::Integer,1,65536,12,{}},
    {"mel_scale",C::Processing,V::Integer,0,1,0,{}},
    {"mel_normalization",C::Processing,V::Integer,0,6,3,{}},
    {"mel_weighting",C::Processing,V::Integer,0,1,0,{}},
    {"mel_construction",C::Processing,V::Integer,0,1,1,{}},
    {"mel_norm_p",C::Processing,V::Number,0.000001,16,1,{}},
    {"explicit_bands",C::Processing,V::Integer,1,65536,1,{}},
    {"weights",C::Processing,V::NumericArray,0,0,0,{}},
    {"centers_hz",C::Processing,V::NumericArray,0,0,0,{}},
    {"coefficients",C::Processing,V::NumericArray,0,0,0,{}},
    {"automatic_difference_lag",C::Processing,V::Boolean,0,1,1,{}},
    {"difference_overlap",C::Processing,V::Number,0,1,0.5,{}}
};
constexpr ParameterDescriptor peaks[]{
    {"frame_rate",C::Processing,V::Number,0.000001,192000,100,{}},
    {"threshold",C::Processing,V::Number,-1e20,1e20,0.5,{}},
    {"pre_average",C::Processing,V::Integer,0,65536,0,{}},
    {"post_average",C::Processing,V::Integer,0,65536,0,{}},
    {"pre_maximum",C::Processing,V::Integer,0,65536,1,{}},
    {"post_maximum",C::Processing,V::Integer,0,65536,1,{}},
    {"smoothing",C::Processing,V::NumericArray,0,0,0,{}}
};
constexpr auto pcm_parameters=[] {
    std::array<ParameterDescriptor,std::size(novelty)+std::size(pcm)> values{};
    for(std::size_t i=0;i<std::size(novelty);++i)values[i]=novelty[i];
    for(std::size_t i=0;i<std::size(pcm);++i)values[i+std::size(novelty)]=pcm[i];
    return values;
}();
}
std::span<const ParameterDescriptor> spectral_sequence_parameters(std::string_view kind) noexcept {
    if(kind=="SpectralNovelty")return novelty;
    return kind=="PcmSpectralNovelty"?std::span<const ParameterDescriptor>(pcm_parameters):std::span<const ParameterDescriptor>(peaks);
}
}

