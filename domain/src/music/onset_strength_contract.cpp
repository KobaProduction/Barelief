#include <domain/music/contracts.h>
#include <domain/processing_contracts.h>
#include <algorithm>
#include <cmath>

namespace domain {
using namespace signal_contracts;
namespace {
bool integer(const ResolvedProcessingNode& node,std::string_view key,std::size_t& out) noexcept {
    ProcessingParameterView value;if(resolve_processing_parameter(node,key,value)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&value);if(!literal)return false;
    const auto number=std::visit([](auto v){return double(v);},*literal);
    if(!std::isfinite(number) || number<1 || number>UINT32_MAX || std::floor(number)!=number)return false;
    out=std::size_t(number);return true;
}
bool boolean(const ResolvedProcessingNode& node,std::string_view key,bool& out) noexcept {
    ProcessingParameterView value;if(resolve_processing_parameter(node,key,value)!=Status::ok)return false;
    const auto* flag=std::get_if<bool>(&value);if(!flag)return false;
    out=*flag;return true;
}
bool geometry(const NumericContract& c) noexcept {
    return c.dtype==DType::Float32 && c.channel_order.size()==1 && c.shape.rank==Rank::Matrix &&
        c.shape.axes[0]==Axis::Time && (c.shape.axes[1]==Axis::Band || c.shape.axes[1]==Axis::Bin) &&
        c.shape.extents[0].maximum && c.shape.extents[1].minimum && c.shape.extents[1].minimum==c.shape.extents[1].maximum &&
        c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar &&
        !c.representation.valid_bits && c.representation.normalization==Normalization::None &&
        (c.unit==NumericUnit::Decibels || c.unit==NumericUnit::Values);
}
bool streaming_geometry(const NumericContract& c,const OnsetStrengthParameters& p) noexcept {
    return !p.center && !p.detrend && p.aggregation==OnsetStrengthAggregation::Features &&
        c.dtype==DType::Float32 && c.channel_order.size()==1 && c.shape.rank==Rank::Vector &&
        c.shape.extents[0].minimum && c.shape.extents[0].minimum==c.shape.extents[0].maximum &&
        c.representation.encoding==StorageEncoding::Native && c.representation.layout==ChannelLayout::Planar &&
        !c.representation.valid_bits && c.representation.normalization==Normalization::None &&
        (c.unit==NumericUnit::Decibels || c.unit==NumericUnit::Values);
}
bool aggregation(std::string_view text,OnsetStrengthAggregation& output) noexcept {
    if(text=="mean")output=OnsetStrengthAggregation::Mean;
    else if(text=="median")output=OnsetStrengthAggregation::Median;
    else if(text=="sum")output=OnsetStrengthAggregation::Sum;
    else if(text=="none")output=OnsetStrengthAggregation::Features;
    else return false;
    return true;
}
NumericContract envelope_contract(const NumericContract& input,const OnsetStrengthParameters& p) noexcept {
    auto c=input;c.dtype=DType::Float32;c.semantic=Semantic::Numeric;c.history_element=Semantic::Numeric;c.quantity=Quantity::Unspecified;
    c.unit=NumericUnit::Values;c.representation.alignment=alignof(float);
    if(p.aggregation!=OnsetStrengthAggregation::Features){
        c.shape.rank=Rank::Vector;c.shape.axes[1]=Axis::None;c.shape.extents[1]={};c.coordinates[1]={};
    }
    if(!p.center){
        if(c.shape.extents[0].minimum<p.lag)c.coordinates[0]={};
        c.shape.extents[0]={std::max(c.shape.extents[0].minimum,std::uint64_t(p.lag)),std::max(c.shape.extents[0].maximum,std::uint64_t(p.lag))};
    }
    return c;
}
}
bool onset_strength_parameters(const ResolvedProcessingNode& node,OnsetStrengthParameters& out) noexcept {
    OnsetStrengthParameters p;ProcessingParameterView value;
    if(!integer(node,"lag",p.lag) || !integer(node,"max_size",p.max_size) || !integer(node,"n_fft",p.n_fft) ||
       !integer(node,"hop_length",p.hop_length) || !boolean(node,"center",p.center) || !boolean(node,"detrend",p.detrend) ||
       resolve_processing_parameter(node,"aggregate",value)!=Status::ok)return false;
    const auto* text=std::get_if<std::string_view>(&value);
    if(!text || !aggregation(*text,p.aggregation))return false;
    out=p;return true;
}
Status onset_strength_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& out) noexcept {
    const ResolvedProcessingNode view{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,node.enabled};
    OnsetStrengthParameters p;if(!onset_strength_parameters(view,p))return Status::incompatible_contract;
    if(!geometry(input) && !streaming_geometry(input,p))return Status::incompatible_contract;
    auto c=input;
    if(input.shape.rank==Rank::Matrix)c=envelope_contract(input,p);
    else {c.semantic=Semantic::Numeric;c.unit=NumericUnit::Values;c.quantity=Quantity::Unspecified;}
    return own_processing_contract(c,FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
}
