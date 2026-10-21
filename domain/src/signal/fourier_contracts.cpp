#include <domain/signal/contracts.h>
#include <cmath>
#include <numeric>
#include <utility>
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
    if(input.dtype!=DType::Float32 || (input.shape.rank!=Rank::Scalar && input.shape.rank!=Rank::Vector) ||
       input.shape.extents[0].minimum!=input.shape.extents[0].maximum || input.channel_order.size()!=1 ||
       input.representation.encoding!=StorageEncoding::Native || input.representation.valid_bits ||
       input.representation.layout!=ChannelLayout::Planar)return Status::incompatible_contract;
    // Explicit coordinate payloads require a separate owning axis join; this
    // arithmetic primitive currently supports index/regular-coordinate inputs.
    for(const auto& axis:input.coordinates)if(axis.mode==CoordinateMode::ExplicitFloat64LE)return Status::incompatible_contract;
    return materialize_polar_contracts(input,out);
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
    if(n<2 || n>UINT32_MAX || std::floor(n)!=n || (std::size_t(n)&(std::size_t(n)-1)) ||
       (method!="radix2" && method!="radix4") ||
       (node.kind=="Fourier" && method=="radix4" && (std::size_t(n)&0x55555555)==0))return Status::invalid_argument;
    return Status::ok;
}
NumericContract fourier_bin_contract(const ConfiguredProcessingNode& node,const NumericContract& input,double n) {
    auto value=input;value.semantic=Semantic::Numeric;value.history_element=Semantic::Numeric;
    value.quantity=Quantity::SpectrumReal;value.unit=NumericUnit::RawAmplitude;
    value.representation.normalization=Normalization::None;value.coordinates={};
    value.shape.axes={Axis::Bin,Axis::None};
    const auto count=node.kind=="Fourier"?std::uint64_t(n)-1:std::uint64_t(n)/2;
    value.shape.extents[0]={count,count};
    return value;
}
Status materialize_fourier_contracts(const NumericContract& value,std::array<OwnedProcessingContract,4>& out) {
    std::array<OwnedProcessingContract,4> candidate;
    constexpr FrameLimits bounds{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX};
    for(std::size_t port=0;port<4;++port){
        auto c=value;c.quantity=port%2?Quantity::SpectrumImaginary:Quantity::SpectrumReal;
        if(port>=2)c.shape={Rank::Scalar,{Axis::None,Axis::None},{ExtentBounds{},ExtentBounds{}}};
        const auto status=own_processing_contract(c,bounds,16384,candidate[port]);if(status!=Status::ok)return status;
    }
    out=std::move(candidate);return Status::ok;
}
}
Status fourier_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,
    std::array<OwnedProcessingContract,4>& out) noexcept {
    if(node.kind!="Fourier" && node.kind!="RealFourier")return Status::invalid_argument;
    double n=0;
    if(fourier_parameters(node,n)!=Status::ok)return Status::invalid_argument;
    if(input.dtype!=DType::Float32 || input.shape.rank!=Rank::Vector || input.channel_order.size()!=1 ||
       input.shape.extents[0]!=ExtentBounds{std::uint64_t(n),std::uint64_t(n)} ||
       input.representation.encoding!=StorageEncoding::Native || input.representation.valid_bits ||
       input.representation.layout!=ChannelLayout::Planar)return Status::incompatible_contract;
    return materialize_fourier_contracts(fourier_bin_contract(node,input,n),out);
}

}
