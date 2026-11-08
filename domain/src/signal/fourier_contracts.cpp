#include <domain/signal/contracts.h>
#include <cmath>
#include <numeric>
#include <utility>
#include <new>
namespace domain {
using namespace signal_contracts;
Status numeric_metadata_output_contract(const NumericContract& input,bool sample_rate,OwnedProcessingContract& out) noexcept {
    if(input.channel_order.size()!=1 || input.shape.extents[0].maximum>UINT32_MAX || input.shape.extents[1].maximum>UINT32_MAX)return Status::incompatible_contract;
    auto scalar=input;scalar.dtype=DType::UInt32;scalar.semantic=Semantic::Scalar;scalar.history_element=Semantic::Numeric;
    scalar.quantity=Quantity::Unspecified;scalar.unit=sample_rate?NumericUnit::Hertz:NumericUnit::Values;
    scalar.shape={};scalar.coordinates={};scalar.representation={};scalar.representation.alignment=alignof(std::uint32_t);
    return own_processing_contract(scalar,{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},16384,out);
}
namespace {
Status materialize_polar_contracts(const NumericContract& input,std::array<OwnedProcessingContract,2>& out) {
    std::array<OwnedProcessingContract,2> candidate;
    for(std::size_t i=0;i<2;++i){auto c=input;c.semantic=Semantic::Numeric;c.representation.normalization=Normalization::None;
        c.quantity=i?Quantity::SpectrumPhase:Quantity::SpectrumMagnitude;c.unit=i?NumericUnit::Radians:input.unit;
        const auto status=own_processing_contract(c,{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},16384,candidate[i]);if(status!=Status::ok)return status;}
    out=std::move(candidate);return Status::ok;
}
}
Status complex_polar_output_contracts(const NumericContract& input,std::array<OwnedProcessingContract,2>& out) noexcept {
    if(input.dtype!=DType::Float32 ||
       (input.shape.rank!=Rank::Matrix && input.shape.extents[0].minimum!=input.shape.extents[0].maximum) || input.shape.extents[1].minimum!=input.shape.extents[1].maximum || input.channel_order.size()!=1 ||
       input.representation.encoding!=StorageEncoding::Native || input.representation.valid_bits ||
       input.representation.layout!=ChannelLayout::Planar)return Status::incompatible_contract;
    return materialize_polar_contracts(input,out);
}
namespace {
bool polar_policy(const ResolvedProcessingNode& node) {
    ProcessingParameterView norm,zero;
    if(resolve_processing_parameter(node,"magnitude_norm",norm)!=Status::ok || resolve_processing_parameter(node,"zero_phase",zero)!=Status::ok)return false;
    const auto* n=std::get_if<std::string_view>(&norm);const auto* z=std::get_if<std::string_view>(&zero);
    return n && z && (*n=="hypot" || *n=="squared_norm" || *n=="scaled_ratio_fma") && (*z=="zero" || *z=="atan2");
}
bool polar_geometry(const ResolvedProcessingNode& node,const NumericContract& input) {
    ProcessingParameterView size,first,half;
    if(resolve_processing_parameter(node,"fft_size",size)!=Status::ok || resolve_processing_parameter(node,"first_bin",first)!=Status::ok ||
       resolve_processing_parameter(node,"positive_half",half)!=Status::ok)return false;
    const auto* n=std::get_if<NumericLiteral>(&size);const auto* f=std::get_if<NumericLiteral>(&first);const auto* h=std::get_if<bool>(&half);
    if(!n || !f || !h)return false;
    const auto fft=std::visit([](auto v){return double(v);},*n),begin=std::visit([](auto v){return double(v);},*f);
    const auto available=*h?std::floor(fft/2)+1:fft;
    return fft>=1 && fft<=UINT32_MAX && std::floor(fft)==fft && begin>=0 && std::floor(begin)==begin &&
        begin<available && input.shape.extents[input.shape.rank==Rank::Matrix?1:0].maximum<=available-begin;
}
}
Status complex_polar_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,std::array<OwnedProcessingContract,2>& out) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::ComplexPolar,node.inputs,node.outputs,&node.parameters,node.enabled};
    if(!polar_policy(resolved) || !polar_geometry(resolved,input))return Status::invalid_argument;
    return complex_polar_output_contracts(input,out);
}
namespace {
Status fourier_parameters(const ConfiguredProcessingNode& node,double& n) {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Fourier,
        node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView size,radix;
    if(resolve_processing_parameter(resolved,"fft_size",size)!=Status::ok || !std::holds_alternative<NumericLiteral>(size) ||
       resolve_processing_parameter(resolved,"radix",radix)!=Status::ok || !std::holds_alternative<std::string_view>(radix))return Status::invalid_argument;
    n=std::visit([](auto value){return double(value);},std::get<NumericLiteral>(size));
    const auto method=std::get<std::string_view>(radix);
    if(n<(node.kind=="Fourier"?4:8) || n>UINT32_MAX || std::floor(n)!=n || (std::size_t(n)&(std::size_t(n)-1)) ||
       (method!="radix2" && method!="radix4") ||
       (node.kind=="Fourier" && method=="radix4" && (std::size_t(n)&0x55555555)==0))return Status::invalid_argument;
    return Status::ok;
}
NumericContract fourier_bin_contract(const ConfiguredProcessingNode& node,const NumericContract& input,double n) {
    auto value=input;value.semantic=Semantic::Numeric;value.history_element=Semantic::Numeric;
    if(input.dtype==DType::Float64){value.dtype=DType::Float32;value.representation.alignment=alignof(float);}
    value.quantity=Quantity::SpectrumReal;value.unit=NumericUnit::RawAmplitude;
    value.representation.normalization=Normalization::None;value.coordinates={};
    const auto axis=input.shape.rank==Rank::Matrix?1u:0u;
    if(axis)value.coordinates[0]=input.coordinates[0];
    value.coordinates[axis]={CoordinateMode::Regular,NumericUnit::Values,{0,1},{1,1}};
    value.shape.axes[axis]=Axis::Bin;
    const auto count=node.kind=="Fourier"?std::uint64_t(n):std::uint64_t(n)/2+1;
    value.shape.extents[axis]={count,count};
    return value;
}
Status materialize_fourier_contracts(NumericContract value,std::vector<OwnedProcessingContract>& out) {
#if defined(__cpp_exceptions)
    try {
#endif
    std::vector<OwnedProcessingContract> candidate;candidate.reserve(2);
    constexpr FrameLimits bounds{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX};
    for(std::size_t port=0;port<2;++port){
        value.quantity=port==0?Quantity::SpectrumReal:Quantity::SpectrumImaginary;
        OwnedProcessingContract contract;
        const auto status=own_processing_contract(value,bounds,SIZE_MAX,contract);if(status!=Status::ok)return status;
        candidate.push_back(std::move(contract));
    }
    out=std::move(candidate);return Status::ok;
#if defined(__cpp_exceptions)
    }catch(const std::bad_alloc&){return Status::exhausted;}
#endif
}
bool fourier_input_valid(const NumericContract& input,double n,bool real) {
    const auto axis=input.shape.rank==Rank::Matrix?1u:0u;
    return (input.dtype==DType::Float32 || (real && input.dtype==DType::Float64)) && (input.shape.rank==Rank::Vector || input.shape.rank==Rank::Matrix) && input.channel_order.size()==1 &&
        input.shape.extents[axis]==ExtentBounds{std::uint64_t(n),std::uint64_t(n)} &&
        input.representation.encoding==StorageEncoding::Native && !input.representation.valid_bits &&
        input.representation.layout==ChannelLayout::Planar;
}
}
Status fourier_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,
    std::vector<OwnedProcessingContract>& out) noexcept {
    if(node.kind!="Fourier" && node.kind!="RealFourier")return Status::invalid_argument;
    double n=0;
    if(fourier_parameters(node,n)!=Status::ok)return Status::invalid_argument;
    if(!fourier_input_valid(input,n,node.kind=="RealFourier"))return Status::incompatible_contract;
    return materialize_fourier_contracts(fourier_bin_contract(node,input,n),out);
}

}
