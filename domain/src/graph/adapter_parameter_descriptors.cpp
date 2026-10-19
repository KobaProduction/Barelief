#include "parameter_sets.h"
#include <limits>
namespace domain::detail {
namespace {
constexpr ParameterDescriptor source[]{{"source",C::Processing,V::Integer,0,1,0,{}},{"sample_rate",C::Processing,V::Integer,1000,192000,48000,{}},{"output_count",C::Processing,V::Integer,0,64,0,{}}};
constexpr ParameterDescriptor wled_output[]{
        {"destination",C::Processing,V::String,0,0,0,"255.255.255.255"},
        {"port",C::Processing,V::Integer,1,65535,11988,{}},
        {"interface",C::Processing,V::String,0,0,0,"0.0.0.0"},
        {"cadence_ms",C::Processing,V::Integer,1,60000,17,{}}};
constexpr ParameterDescriptor wled_encode[]{gain[0],
        {"peak_hz",C::Processing,V::Number,0,std::numeric_limits<float>::max(),0,{}},
        {"magnitude",C::Processing,V::Number,0,std::numeric_limits<float>::max(),0,{}}};
constexpr ParameterDescriptor network[]{
        {"data_type",C::Processing,V::FrameType,0,0,0,"mel"},
        {"sample_rate",C::Processing,V::Integer,1,4294967295.,48000,{}},
        {"provider_uuid",C::ApplicationAdapter,V::String,0,0,0,{}},{"stream_id",C::ApplicationAdapter,V::String,0,0,0,{}},
        {"host",C::ApplicationAdapter,V::String,0,0,0,{}},{"port",C::ApplicationAdapter,V::Integer,1,65535,8214,{}}};
constexpr ParameterSet sets[]{
    {NodeKind::AudioSource,source},
    {NodeKind::WLED,gain},
    {NodeKind::WLEDEncode,wled_encode},
    {NodeKind::WLEDOutput,wled_output},
    {NodeKind::NetworkInput,network},
    {NodeKind::NetworkOutput,network},
};
}
std::span<const ParameterDescriptor> adapter_parameter_descriptors(NodeKind kind) noexcept {
    return find_parameters(sets,kind);
}
}
