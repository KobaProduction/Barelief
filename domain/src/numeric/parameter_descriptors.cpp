#include "../graph/parameter_sets.h"
#include <domain/graph/processing_model.h>
#include <limits>
namespace domain::detail {
namespace {
std::optional<NumericParameterBounds> matrix_row_bounds(const ConfiguredProcessingNode& node) noexcept {
    const auto input=std::find_if(node.inputs.begin(),node.inputs.end(),[](const auto& port){return port.id==0;});
    if(input==node.inputs.end() || !input->contract)return {};
    const auto contract=input->contract.view();const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract);
    if(!numeric || numeric->shape.rank!=signal_contracts::Rank::Matrix)return {};
    const auto rows=numeric->shape.extents[0],columns=numeric->shape.extents[1];
    if(!rows.maximum || rows.minimum>rows.maximum || !columns.maximum || columns.minimum>columns.maximum)return {};
    return NumericParameterBounds{0,double(rows.maximum-1)};
}
constexpr ParameterDescriptor integer[]{{"value",C::Processing,V::Integer,0,4294967295.,4096,{}}};
constexpr ParameterDescriptor length[]{{"axis",C::Processing,V::Integer,0,1,0,{}}};
constexpr ParameterDescriptor matrix_fft[]{{"fft_size",C::Processing,V::Integer,2,4294967295.,512,{}}};
constexpr ParameterDescriptor matrix_window[]{{"function",C::Processing,V::WindowFunction,0,0,0,"hann"}};
constexpr ParameterDescriptor matrix_row[]{{"row",C::Processing,V::Integer,0,2147483647,0,{},{},matrix_row_bounds}};
constexpr ParameterDescriptor matrix_center[]{
        {"method",C::Processing,V::Integer,0,2,0,{}},
        {"offset",C::Processing,V::Number,-double(std::numeric_limits<float>::max()),double(std::numeric_limits<float>::max()),0,{}}};
constexpr ParameterDescriptor extract[]{
        {"channel_index",C::Processing,V::Integer,0,65535,0,{}},
        {"index",C::Processing,V::Integer,0,2147483647,0,{}},
        {"column",C::Processing,V::Integer,0,2147483647,0,{}}};
constexpr ParameterDescriptor value[]{{"value",C::Processing,V::Number,-1e30,1e30,0,{}}};
constexpr ParameterDescriptor product[]{{"frame_input",C::Processing,V::Integer,0,65535,0,{}},{"operation",C::Processing,V::String,0,0,0,"multiply"},{"floor_enabled",C::Processing,V::Boolean,0,1,0,{}},{"floor",C::Processing,V::Number,-1e30,1e30,-120,{}}};
constexpr ParameterDescriptor reduce[]{
        {"data_type",C::Processing,V::FrameType,0,0,0,"mel"},{"operation",C::Processing,V::Integer,0,2,0,{}}};
constexpr ParameterDescriptor history[]{{"history_size",C::Processing,V::Integer,2,2000,400,{}}};
constexpr ParameterDescriptor number_history[]{{"capacity",C::Processing,V::Integer,2,2000,400,{}}};
constexpr ParameterSet sets[]{
    {NodeKind::MatrixFFT,matrix_fft},
    {NodeKind::MatrixWindowFunction,matrix_window},
    {NodeKind::MatrixRow,matrix_row},
    {NodeKind::MatrixCenter,matrix_center},
    {NodeKind::UInt32Constant,integer},
    {NodeKind::Number,value},
    {NodeKind::ArrayFill,fill_parameters},
    {NodeKind::ArrayMultiply,product},
    {NodeKind::ArrayLength,length},
    {NodeKind::IndexExtract,extract},
    {NodeKind::Spectrogram,history},
    {NodeKind::NumberAccumulator,number_history},
    {NodeKind::ScalarReduce,reduce},
};
}
std::span<const ParameterDescriptor> numeric_parameter_descriptors(NodeKind kind) noexcept {
    return find_parameters(sets,kind);
}
}
