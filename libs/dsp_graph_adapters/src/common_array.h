#pragma once
#include "common_support.h"
#include <domain/numeric/array_processing_ports.h>

namespace dsp_graph_adapters::detail {
struct CommonArrayGeometry {
    signal_contracts::Rank rank=signal_contracts::Rank::Vector;
    std::size_t rows=1,columns=1,axis=0;
    std::size_t width() const noexcept {return axis?columns:rows;}
};
inline bool common_array_axis(const dataflow_runtime::NumericOperationNodeView& node,
    signal_contracts::Rank rank,std::size_t& axis) noexcept {
    const auto last=std::size_t(domain::ArrayAxis::Last);std::size_t selected=last;
    if(!integer(node,"axis",last,selected) || selected>last)return false;
    axis=selected==last?(rank==signal_contracts::Rank::Matrix?1u:0u):selected;
    return rank==signal_contracts::Rank::Matrix || !axis;
}
inline bool common_array_storage(const signal_contracts::NumericBufferContract& source,bool allow_double) noexcept {
    using namespace signal_contracts;
    const bool numeric=source.dtype==DType::Float32 || (allow_double && source.dtype==DType::Float64);
    return numeric && source.plane_count==1 && source.representation.encoding==StorageEncoding::Native &&
        !source.representation.valid_bits && source.representation.layout==ChannelLayout::Planar &&
        (source.shape.rank==Rank::Vector || source.shape.rank==Rank::Matrix);
}
inline bool common_array_geometry(const dataflow_runtime::NumericOperationNodeView& node,
    domain::PortId port,CommonArrayGeometry& geometry,bool selectable_axis=false,bool allow_double=false) noexcept {
    using namespace signal_contracts;
    const auto source=contract(node,domain::PortDirection::Input,port);
    if(!source || !common_array_storage(*source,allow_double))return false;
    geometry.rank=source->shape.rank;geometry.rows=std::size_t(source->shape.extents[0].maximum);
    geometry.columns=std::size_t(source->shape.extents[1].maximum);
    geometry.axis=geometry.rank==Rank::Matrix?1:0;
    if(selectable_axis && !common_array_axis(node,geometry.rank,geometry.axis))return false;
    return geometry.rows && geometry.columns && (geometry.rank==Rank::Matrix || !geometry.axis);
}
inline signal_contracts::ElementIndex common_array_index(std::size_t row,std::size_t column,
    const CommonArrayGeometry& geometry) noexcept {
    if(geometry.rank==signal_contracts::Rank::Vector)return {0,column,0};
    return geometry.axis?signal_contracts::ElementIndex{0,row,column}:signal_contracts::ElementIndex{0,column,row};
}
inline std::size_t common_array_rows(const signal_contracts::NumericBufferView& input,
    const CommonArrayGeometry& geometry) noexcept {
    return geometry.rank==signal_contracts::Rank::Vector?1:input.actual[1-geometry.axis];
}
// Geometry permits Double only when the caller opts in. Row I/O keeps the
// selected scalar type; it never narrows a Double buffer through Float32 scratch.
template<typename Scalar> inline bool common_array_read_values(const signal_contracts::NumericBufferView& input,std::size_t row,
    std::span<Scalar> values,const CommonArrayGeometry& geometry) noexcept {
    for(std::size_t column=0;column<values.size();++column)
        if(load_element(input,common_array_index(row,column,geometry),values[column])!=signal_contracts::Status::ok)return false;
    return true;
}
template<typename Scalar> inline bool common_array_write_values(signal_contracts::MutableNumericBufferView& output,std::size_t row,
    std::span<const Scalar> values,const CommonArrayGeometry& geometry) noexcept {
    for(std::size_t column=0;column<values.size();++column)
        if(store_element(output,common_array_index(row,column,geometry),values[column])!=signal_contracts::Status::ok)return false;
    return true;
}
inline bool common_array_read(const signal_contracts::NumericBufferView& input,std::size_t row,
    std::span<float> values,const CommonArrayGeometry& geometry) noexcept {return common_array_read_values(input,row,values,geometry);}
inline bool common_array_read(const signal_contracts::NumericBufferView& input,std::size_t row,
    std::span<double> values,const CommonArrayGeometry& geometry) noexcept {return common_array_read_values(input,row,values,geometry);}
inline bool common_array_write(signal_contracts::MutableNumericBufferView& output,std::size_t row,
    std::span<const float> values,const CommonArrayGeometry& geometry) noexcept {return common_array_write_values(output,row,values,geometry);}
inline bool common_array_write(signal_contracts::MutableNumericBufferView& output,std::size_t row,
    std::span<const double> values,const CommonArrayGeometry& geometry) noexcept {return common_array_write_values(output,row,values,geometry);}
inline std::size_t common_array_element_bytes(signal_contracts::DType type) noexcept {
    if(type==signal_contracts::DType::Float32)return sizeof(float);
    return type==signal_contracts::DType::Float64?sizeof(double):0;
}

}
