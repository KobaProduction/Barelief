#include "parameter_sets.h"
#include <limits>
namespace domain::detail {
namespace {
constexpr ParameterDescriptor frequency_view[]{
        {"channel",C::Presentation,V::Integer,0,2,0,{}},{"merge",C::Presentation,V::Integer,0,2,2,{}},
        {"bin_hz",C::Processing,V::Number,0,4294967295.0,0,{}},
        {"first_bin",C::Processing,V::Integer,0,4294967295.0,0,{}}};
constexpr ParameterDescriptor audio_vector[]{
        {"bin_hz",C::Processing,V::Number,0,4294967295.0,0,{}},
        {"first_bin",C::Processing,V::Integer,0,4294967295.0,0,{}}};
constexpr ParameterDescriptor piano[]{{"first_midi",C::Processing,V::Integer,0,127,21,{}}};
constexpr ParameterSet sets[]{
    {NodeKind::FFTView,frequency_view},
    {NodeKind::AudioLineView,audio_vector},
    {NodeKind::AudioPointView,audio_vector},
    {NodeKind::AudioBarView,audio_vector},
    {NodeKind::AudioPieView,audio_vector},
    {NodeKind::PianoView,piano},
};
}
std::span<const ParameterDescriptor> view_parameter_descriptors(NodeKind kind) noexcept {
    return find_parameters(sets,kind);
}
}
