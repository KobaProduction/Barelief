#include <domain/numeric/array_extraction.h>
#include <cmath>
#include <algorithm>

namespace domain {
using namespace signal_contracts;
namespace {
bool number(const ResolvedProcessingNode& node,const char* key,double& result) noexcept {
    ProcessingParameterView field;
    if(resolve_processing_parameter(node,key,field)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&field);if(!literal)return false;
    result=std::visit([](auto value){return double(value);},*literal);
    return std::isfinite(result);
}
bool size(const ResolvedProcessingNode& node,const char* key,std::size_t& result) noexcept {
    double value=0;if(!number(node,key,value) || value<0 || value>2147483647. || std::floor(value)!=value)return false;
    result=std::size_t(value);return true;
}
bool constant_value(const ResolvedProcessingNode& node,NumericLiteral& result) noexcept {
    ProcessingParameterView field;
    if(resolve_processing_parameter(node,"constant",field)!=Status::ok)return false;
    const auto* literal=std::get_if<NumericLiteral>(&field);if(!literal || validate_literal(*literal)!=Status::ok)return false;
    result=*literal;return true;
}
bool extraction_indices(const ResolvedProcessingNode& node,std::size_t maximum,AxisExtraction& result) noexcept {
    ProcessingParameterView field;
    if(resolve_processing_parameter(node,"indices",field)!=Status::ok)return false;
    const auto* indices=std::get_if<std::span<const float>>(&field);if(!indices)return false;
    result.indices=*indices;
    for(float index:result.indices)
        if(!std::isfinite(index) || std::floor(index)!=index || double(index)>=double(maximum) || double(index)<-double(maximum))return false;
    return true;
}
bool extraction_geometry(const ResolvedProcessingNode& node,const NumericContract& input,AxisExtraction& result) noexcept {
    if(input.shape.rank!=Rank::Vector && input.shape.rank!=Rank::Matrix)return false;
    if(!size(node,"axis",result.axis) || result.axis>2)return false;
    if(result.axis==2)result.axis=input.shape.rank==Rank::Matrix?1:0;
    if(input.shape.rank==Rank::Vector && result.axis)return false;
    const auto maximum=input.shape.extents[result.axis].maximum;
    return maximum && maximum<=PTRDIFF_MAX && size(node,"first",result.first) && result.first<=maximum &&
        size(node,"length",result.length) && result.length<=maximum-result.first && extraction_indices(node,std::size_t(maximum),result);
}
bool extraction_padding(const ResolvedProcessingNode& node,std::size_t maximum,AxisExtraction& result) noexcept {
    std::size_t padding=0;
    if(!size(node,"pad_before",result.before) || !size(node,"pad_after",result.after) ||
       !size(node,"output_length",result.output_length) || !size(node,"padding",padding) || padding>1 ||
       !constant_value(node,result.constant))return false;
    result.padding=ArrayPadding(padding);
    const auto count=extraction_selected(result,maximum);
    if(result.before>SIZE_MAX-count || result.after>SIZE_MAX-count-result.before)return false;
    const auto total=count+result.before+result.after;
    return total && (!result.output_length || result.output_length>=total);
}
Status element_contract(const ResolvedProcessingNode& node,const NumericContract& input,NumericContract& result) noexcept {
    std::size_t channel=0,row=0,column=0;
    if(!size(node,"channel_index",channel) || !size(node,"index",row) || !size(node,"column",column) ||
       input.shape.rank==Rank::Scalar || channel>=input.channel_order.size() || row>=input.shape.extents[0].maximum ||
       (input.shape.rank==Rank::Vector?column!=0:column>=input.shape.extents[1].maximum))return Status::invalid_argument;
    auto output=input;output.shape={};output.coordinates={};output.channel_order=input.channel_order.subspan(channel,1);
    result=output;return Status::ok;
}
Status axis_contract(const NumericContract& input,const AxisExtraction& selection,NumericContract& result) noexcept {
    auto output=input;const auto axis=selection.axis;
    const auto maximum=extraction_length(selection,std::size_t(input.shape.extents[axis].maximum));
    const auto minimum=extraction_length(selection,std::size_t(input.shape.extents[axis].minimum));
    output.shape.axes[axis]=axis?Axis::Component:Axis::Declared;
    output.shape.extents[axis]={minimum,std::max(std::size_t{1},maximum)};
    output.coordinates[axis]={};output.semantic=Semantic::Numeric;output.history_element=Semantic::Numeric;
    result=output;return Status::ok;
}
}
bool extraction_mode(const ResolvedProcessingNode& node,ExtractionMode& result) noexcept {
    std::size_t mode=0;if(!size(node,"mode",mode) || mode>1)return false;
    result=ExtractionMode(mode);return true;
}
std::size_t extraction_selected(const AxisExtraction& selection,std::size_t actual) noexcept {
    if(!selection.indices.empty())return selection.indices.size();
    if(actual<selection.first)return 0;
    const auto available=actual-selection.first;
    return selection.length?std::min(selection.length,available):available;
}
std::size_t extraction_length(const AxisExtraction& selection,std::size_t actual) noexcept {
    return selection.output_length?selection.output_length:selection.before+extraction_selected(selection,actual)+selection.after;
}
bool axis_extraction(const ResolvedProcessingNode& node,const NumericContract& input,AxisExtraction& result) noexcept {
    return extraction_geometry(node,input,result) && extraction_padding(node,std::size_t(input.shape.extents[result.axis].maximum),result);
}
Status extraction_output_contract(const ConfiguredProcessingNode& node,const NumericContract& input,NumericContract& result) noexcept {
    const ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::Extract,node.inputs,node.outputs,&node.parameters,node.enabled};
    if(input.shape.rank!=Rank::Vector && input.shape.rank!=Rank::Matrix)return Status::invalid_argument;
    ExtractionMode mode;if(!extraction_mode(resolved,mode))return Status::invalid_argument;
    if(mode==ExtractionMode::Element)return element_contract(resolved,input,result);
    AxisExtraction selection;if(!axis_extraction(resolved,input,selection))return Status::invalid_argument;
    return axis_contract(input,selection,result);
}
}
