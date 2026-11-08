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
}
std::span<const ParameterDescriptor> spectral_sequence_parameters(std::string_view kind) noexcept {
    if(kind=="SpectralNovelty")return novelty;
    return {};
}
}

