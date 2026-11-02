#include <domain/signal/contracts.h>
#include <algorithm>
#include <cmath>

namespace domain {
using namespace signal_contracts;
namespace {
bool number(const ResolvedProcessingNode& node,std::string_view key,double& out) noexcept {
    ProcessingParameterView field;if(resolve_processing_parameter(node,key,field)!=Status::ok)return false;
    const auto* value=std::get_if<NumericLiteral>(&field);if(!value)return false;
    out=std::visit([](auto v){return double(v);},*value);return std::isfinite(out);
}
bool boolean(const ResolvedProcessingNode& node,std::string_view key,bool& out) noexcept {
    ProcessingParameterView field;if(resolve_processing_parameter(node,key,field)!=Status::ok)return false;
    const auto* value=std::get_if<bool>(&field);if(!value)return false;
    out=*value;return true;
}
std::string_view text(const ResolvedProcessingNode& node,std::string_view key) noexcept {
    ProcessingParameterView field;if(resolve_processing_parameter(node,key,field)!=Status::ok)return {};
    const auto* value=std::get_if<std::string_view>(&field);return value?*value:std::string_view{};
}
bool one_of(std::string_view value,std::initializer_list<std::string_view> choices) noexcept {
    return std::find(choices.begin(),choices.end(),value)!=choices.end();
}
ResolvedProcessingNode resolved(const ConfiguredProcessingNode& n) noexcept {
    return {n.id,n.kind,NodeRole::Processor,ProcessingOperation::Transform,n.inputs,n.outputs,&n.parameters,n.enabled};
}
bool native_float(const NumericContract& c) noexcept {
    return c.dtype==DType::Float32 && c.channel_order.size()==1 && c.representation.encoding==StorageEncoding::Native &&
        c.representation.layout==ChannelLayout::Planar && !c.representation.valid_bits && c.representation.normalization==Normalization::None;
}
bool db_input(const NumericContract& c) noexcept {
    return native_float(c) && (c.shape.rank==Rank::Scalar || c.shape.rank==Rank::Vector || c.shape.rank==Rank::Matrix) &&
        (c.shape.rank==Rank::Scalar || c.shape.extents[0].maximum) &&
        (c.shape.rank!=Rank::Matrix || c.shape.extents[1].maximum) &&
        (c.unit==NumericUnit::SquaredAmplitude || c.unit==NumericUnit::Values);
}
NumericContract db_output(const NumericContract& input) noexcept {
    auto c=input;c.semantic=Semantic::Numeric;c.history_element=Semantic::Numeric;c.unit=NumericUnit::Decibels;
    c.quantity=input.quantity==Quantity::SpectrumPower?Quantity::SpectrumPowerDb:Quantity::Unspecified;return c;
}
}
bool power_to_db_parameters(const ResolvedProcessingNode& node,PowerToDbParameters& out) noexcept {
    PowerToDbParameters p;
    if(!number(node,"reference",p.reference) || !number(node,"amin",p.amin) || !number(node,"top_db",p.top_db) ||
       !boolean(node,"top_db_enabled",p.top_db_enabled))return false;
    p.reference_mode=text(node,"reference_mode");p.axes=text(node,"reduction_axes");
    if(float(p.amin)<=0 || !std::isfinite(float(p.amin)) || p.top_db<0 || !std::isfinite(float(p.top_db)) ||
       !one_of(p.reference_mode,{"value","maximum"}) || !one_of(p.axes,{"all","rows","columns"}))return false;
    out=p;return true;
}
Status power_to_db_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,OwnedProcessingContract& out) noexcept {
    PowerToDbParameters p;if(!power_to_db_parameters(resolved(node),p) || !db_input(input))return Status::incompatible_contract;
    return own_processing_contract(db_output(input),FrameLimits{1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX},SIZE_MAX,out);
}
}
