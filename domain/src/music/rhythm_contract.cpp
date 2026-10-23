#include <domain/music/contracts.h>
#include <cmath>

namespace domain {
using namespace signal_contracts;
namespace {
bool number(const ConfiguredProcessingNode& node,std::string_view key,double& result) noexcept {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView value;
    if(resolve_processing_parameter(view,key,value)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);
    if(!literal)return false;
    result=std::visit([](auto v){return double(v);},*literal);return std::isfinite(result);
}
bool geometry(const NumericContract& input,double bins) noexcept {
    return input.dtype==DType::UInt8 && input.shape.rank==Rank::Matrix
        && input.shape.axes[0]==Axis::Time && input.shape.axes[1]==Axis::Band
        && input.shape.extents[0].maximum==512 && input.shape.extents[1]==ExtentBounds{std::uint64_t(bins),std::uint64_t(bins)}
        && input.channel_order.size()==1 && input.representation.encoding==StorageEncoding::Native
        && input.representation.layout==ChannelLayout::Planar && input.representation.normalization==Normalization::None
        && !input.representation.valid_bits && !input.clock.domain.empty() && input.clock.tick_seconds.numerator>0 && input.clock.tick_seconds.denominator;
}
bool parameters(const ConfiguredProcessingNode& node,const NumericContract& input) noexcept {
    double fps=0,bins=0,rate=0,budget=0;
    if(!number(node,"feature_rate_fps",fps) || (fps!=30 && fps!=45 && fps!=60))return false;
    if(!number(node,"feature_bins",bins) || (bins!=64 && bins!=96) || !geometry(input,bins))return false;
    if(!number(node,"sample_rate",rate) || rate<=0 || rate!=input.clock.sample_rate)return false;
    return number(node,"budget_ops",budget) && budget>=0 && budget<=UINT32_MAX && std::floor(budget)==budget;
}
bool mode_valid(const ConfiguredProcessingNode& node) noexcept {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
    ProcessingParameterView parameter;
    if(resolve_processing_parameter(view,"mode",parameter)!=Status::ok)return false;
    const auto* value=std::get_if<std::string_view>(&parameter);
    return value && (*value=="fixed-light" || *value=="fixed-full" || *value=="gated");
}
NumericContract scalar(const NumericContract& input,PortId port) noexcept {
    auto output=input;output.dtype=DType::Float32;output.shape={};output.semantic=Semantic::Scalar;
    output.history_element=Semantic::Numeric;output.quantity=Quantity::Unspecified;output.unit=port==5?NumericUnit::Seconds:NumericUnit::Values;
    output.coordinates={};output.representation.alignment=alignof(float);return output;
}
bool complex_input(const NumericContract& c) noexcept {
    return c.dtype==DType::Float32 && c.quantity==Quantity::SpectrumComplex && c.unit==NumericUnit::RawAmplitude
        && c.shape.rank==Rank::Matrix && c.shape.axes==std::array{Axis::Bin,Axis::Component}
        && c.shape.extents==std::array{ExtentBounds{2049,2049},ExtentBounds{2,2}}
        && c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar
        && c.representation.normalization==Normalization::None && !c.representation.valid_bits && c.channel_order.size()==1;
}
bool peak_amplitude_input(const NumericContract& c) noexcept {
    return c.dtype==DType::Float32 && c.quantity==Quantity::SpectrumPeakAmplitude
        && c.unit==NumericUnit::RawAmplitude && c.shape.rank==Rank::Vector
        && c.shape.axes[0]==Axis::Bin && c.shape.extents[0]==ExtentBounds{2048,2048}
        && c.coordinates[0].mode==CoordinateMode::Regular && c.coordinates[0].unit==NumericUnit::Hertz
        && c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar
        && c.representation.normalization==Normalization::None && !c.representation.valid_bits && c.channel_order.size()==1;
}
}
Status rhythm_features_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& owner) noexcept {
    double bins=0;
    if(!number(node,"feature_bins",bins) || (bins!=64 && bins!=96) ||
       (!complex_input(input) && !peak_amplitude_input(input)))return Status::incompatible_contract;
    auto output=input;output.dtype=DType::UInt8;output.semantic=Semantic::Numeric;output.quantity=Quantity::Unspecified;output.unit=NumericUnit::Values;
    output.shape={Rank::Vector,{Axis::Band,Axis::None},{ExtentBounds{std::uint64_t(bins),std::uint64_t(bins)},ExtentBounds{}}};
    output.coordinates={};output.representation.alignment=1;
    return own_processing_contract(output,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,owner);
}
Status rhythm_output_contracts(const ConfiguredProcessingNode& node,const NumericContract& input,
                              std::array<OwnedProcessingContract,rhythm_output_count>& output) noexcept {
    if(node.kind!="RhythmAnalyzer" || !std::holds_alternative<TransformProcessingParameters>(node.parameters) || !parameters(node,input) || !mode_valid(node))return Status::incompatible_contract;
    std::array<OwnedProcessingContract,rhythm_output_count> prepared;
    const FrameLimits limits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX};
    for(PortId port=0;port<rhythm_output_count;++port){
        const auto status=own_processing_contract(scalar(input,port),limits,SIZE_MAX,prepared[port]);
        if(status!=Status::ok)return status;
    }
    output=std::move(prepared);return Status::ok;
}
}
