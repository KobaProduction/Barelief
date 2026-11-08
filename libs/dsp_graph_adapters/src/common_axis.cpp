#include "common_array.h"
#include <domain/numeric/atomic_operation_ports.h>
#include <dsp_kernels/axis_math.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dsp_graph_adapters::detail {
namespace {
using namespace signal_contracts;
namespace gather_ports=domain::atomic_ports::gather;
bool floating(const dataflow_runtime::NumericOperationNodeView& n,domain::PortId port,CommonArrayGeometry& g) noexcept {
    return common_array_geometry(n,port,g,true,true);
}
bool map_geometry(const dataflow_runtime::NumericOperationNodeView& node,CommonArrayGeometry& geometry) noexcept {
    const auto input=contract(node,domain::PortDirection::Input,0);
    if(!input || input->shape.rank!=Rank::Scalar)return floating(node,0,geometry);
    auto vector=*input;vector.shape.rank=Rank::Vector;
    if(!common_array_storage(vector,true))return false;
    geometry={Rank::Scalar,1,1,0};return common_array_axis(node,Rank::Scalar,geometry.axis);
}
bool map_offset(const dataflow_runtime::NumericOperationNodeView& node,double& value) noexcept {
    const auto* field=parameter(node,"offset");if(!field){value=0;return true;}
    const auto* literal=std::get_if<NumericLiteral>(field);if(!literal)return false;
    value=std::visit([](auto x){return double(x);},*literal);return std::isfinite(value);
}
std::optional<NumericBufferContract> gather_indices(const dataflow_runtime::NumericOperationNodeView& node) noexcept {
    const auto indices=contract(node,domain::PortDirection::Input,gather_ports::indices);
    return indices && indices->dtype==DType::UInt64 && indices->shape.rank==Rank::Vector && indices->plane_count==1?indices:std::nullopt;
}
bool load_number(const NumericBufferView& input,ElementIndex index,double& value) noexcept {
    if(input.contract->dtype==DType::Float64)return load_element(input,index,value)==Status::ok;
    float narrowed=0;if(load_element(input,index,narrowed)!=Status::ok)return false;
    value=narrowed;return true;
}
bool store_number(MutableNumericBufferView& output,ElementIndex index,double value) noexcept {
    if(output.contract->dtype==DType::Float64)return store_element(output,index,value)==Status::ok;
    if(output.contract->dtype==DType::Float32)return store_element(output,index,float(value))==Status::ok;
    if(!std::isfinite(value) || value<0 || value>=double(UINT64_MAX))return false;
    return store_element(output,index,std::uint64_t(value))==Status::ok;
}
template<typename Scalar> class Maximum final:public CommonKernel<Maximum<Scalar>> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){if(!floating(n,0,geometry_))return false;values_.resize(geometry_.width());return true;}
    std::size_t workspace_bytes() const noexcept override {return values_.capacity()*sizeof(Scalar);}
private:
    bool row(const NumericBufferView& input,std::span<NumericOperationOutput> out,std::size_t at) noexcept {
        auto values=std::span(values_).first(input.actual[geometry_.axis]);std::size_t index=0;
        if(!common_array_read(input,at,values,geometry_))return false;
        if(!dsp_kernels::first_maximum_index(std::span<const Scalar>(values),index))return false;
        const auto maximum=values[index];
        if(store_element(out[0].value,{0,at,0},std::uint64_t(index))!=Status::ok || store_element(out[1].value,{0,at,0},maximum)!=Status::ok)return false;
        for(std::size_t i=0;i<values.size();++i)
            if(store_element(out[2].value,common_array_index(at,i,geometry_),values[i]==maximum?values[i]:Scalar{0})!=Status::ok)return false;
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto count=common_array_rows(in[0].value,geometry_);
        out[0].value.actual=out[1].value.actual={count,1};out[2].value.actual=in[0].value.actual;
        for(std::size_t i=0;i<count;++i)if(!row(in[0].value,out,i))return false;
        return true;
    }
    CommonArrayGeometry geometry_;std::vector<Scalar> values_;
};
bool sum_configuration(const dataflow_runtime::NumericOperationNodeView& node,bool& cumulative,
    dsp_kernels::AxisSumAccumulation& accumulation) noexcept {
    std::size_t mode=0;
    if(!flag(node,"cumulative",false,cumulative) || !integer(node,"accumulation",0,mode) || mode>1 || (mode && cumulative))return false;
    accumulation=dsp_kernels::AxisSumAccumulation(mode);return true;
}
template<typename Scalar> class Sum final:public CommonKernel<Sum<Scalar>> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        if(!floating(n,0,geometry_) || !sum_configuration(n,cumulative_,accumulation_))return false;
        values_.resize(geometry_.width());result_.resize(cumulative_?geometry_.width():1);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return (values_.capacity()+result_.capacity())*sizeof(Scalar);}
private:
    bool row(const NumericBufferView& input,MutableNumericBufferView& output,std::size_t at) noexcept {
        auto values=std::span(values_).first(input.actual[geometry_.axis]);
        auto result=std::span(result_).first(cumulative_?values.size():1);
        if(!common_array_read(input,at,values,geometry_))return false;
        if(!dsp_kernels::axis_sum(std::span<const Scalar>(values),result,cumulative_,accumulation_))return false;
        if(!cumulative_)return store_element(output,{0,at,0},result[0])==Status::ok;
        return common_array_write(output,at,std::span<const Scalar>(result),geometry_);
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        const auto count=common_array_rows(in[0].value,geometry_);
        out[0].value.actual=cumulative_?in[0].value.actual:std::array<std::size_t,2>{count,1};
        for(std::size_t i=0;i<count;++i)if(!row(in[0].value,out[0].value,i))return false;
        return true;
    }
    CommonArrayGeometry geometry_;bool cumulative_=false;std::vector<Scalar> values_,result_;
    dsp_kernels::AxisSumAccumulation accumulation_=dsp_kernels::AxisSumAccumulation::Sequential;
};
class Gather final:public CommonKernel<Gather> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        CommonArrayGeometry g;
        return floating(n,0,g) && g.rank==Rank::Vector && gather_indices(n);
    }
    std::size_t workspace_bytes() const noexcept override {return 0;}
private:
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        out[0].value.actual=in[1].value.actual;
        for(std::size_t i=0;i<in[1].value.actual[0];++i){std::uint64_t index=0;double value=0;
            if(load_element(in[1].value,{0,i,0},index)!=Status::ok || index>=in[0].value.actual[0] ||
               !load_number(in[0].value,{0,std::size_t(index),0},value) || !store_number(out[0].value,{0,i,0},value))return false;
        }
        return true;
    }
};
class Map final:public CommonKernel<Map> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        CommonArrayGeometry g;return map_geometry(n,g) && integer(n,"operation",0,operation_) && operation_<=1 && number(n,"scale",1,scale_) && map_offset(n,offset_);
    }
    std::size_t workspace_bytes() const noexcept override {return 0;}
private:
    double mapped(double value) const noexcept {
        const double scaled=operation_?double(scale_)/value:double(scale_)*value;
        if(offset_==0)return scaled;
        // The explicit intermediate preserves multiply/divide then add rounding.
        volatile double rounded=scaled;return rounded+offset_;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        out[0].value.actual=in[0].value.actual;
        for(std::size_t row=0;row<in[0].value.actual[0];++row)
            for(std::size_t column=0;column<in[0].value.actual[1];++column){double value=0;const ElementIndex index{0,row,column};
                if(!load_number(in[0].value,index,value))return false;
                value=mapped(value);
                if(!store_number(out[0].value,index,value))return false;
            }
        return true;
    }
    std::size_t operation_=0;float scale_=1;double offset_=0;
};
class OrderedPeaks final:public CommonKernel<OrderedPeaks> {
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){
        CommonArrayGeometry g;if(!floating(n,0,g) || g.rank!=Rank::Vector || !integer(n,"maximum_count",5,maximum_) || !maximum_)return false;
        values_.resize(g.rows);indices_.resize(g.rows);return true;
    }
    std::size_t workspace_bytes() const noexcept override {return values_.capacity()*sizeof(double)+indices_.capacity()*sizeof(std::uint64_t);}
private:
    bool locate(const NumericBufferView& input,std::size_t& count) noexcept {
        const auto size=input.actual[0];
        for(std::size_t i=0;i<size;++i)if(!load_number(input,{0,i,0},values_[i]))return false;
        return dsp_kernels::ordered_wrapped_peaks(std::span<const double>(values_).first(size),std::span(indices_),count);
    }
    bool write(std::span<NumericOperationOutput> out,std::size_t count) const noexcept {
        for(std::size_t i=0;i<count;++i)
            if(store_element(out[0].value,{0,i,0},indices_[i])!=Status::ok || !store_number(out[1].value,{0,i,0},values_[indices_[i]]))return false;
        return true;
    }
    bool run(std::span<const NumericOperationInput> in,std::span<NumericOperationOutput> out) noexcept override {
        std::size_t count=0;if(!locate(in[0].value,count))return false;
        count=std::min(count,maximum_);out[0].value.actual=out[1].value.actual={count,1};
        return write(out,count);
    }
    std::size_t maximum_=5;std::vector<double> values_;std::vector<std::uint64_t> indices_;
};
bool reduction_outputs(const dataflow_runtime::NumericOperationNodeView& n,const CommonArrayGeometry& g,DType type) noexcept {
    const auto count=g.rank==Rank::Matrix?(g.axis?g.rows:g.columns):1;
    if(n.kind=="AxisMaximum")return common_output(n,0,Rank::Vector,count,1,DType::UInt64) && common_output(n,1,Rank::Vector,count,1,type) && common_output(n,2,g.rank,g.rows,g.columns,type);
    bool cumulative=false;if(!flag(n,"cumulative",false,cumulative))return false;
    return cumulative?common_output(n,0,g.rank,g.rows,g.columns,type):common_output(n,0,Rank::Vector,count,1,type);
}
bool gather_output(const dataflow_runtime::NumericOperationNodeView& n,const CommonArrayGeometry& g,DType type) noexcept {
    const auto indices=gather_indices(n);
    return g.rank==Rank::Vector && indices &&
        common_output(n,0,Rank::Vector,std::size_t(indices->shape.extents[0].maximum),1,type);
}
bool map_output(const dataflow_runtime::NumericOperationNodeView& n,const CommonArrayGeometry& g) noexcept {
    std::size_t type=0,operation=0;float scale=1;double offset=0;
    if(!integer(n,"operation",0,operation) || operation>1 || !integer(n,"output_dtype",0,type) || type>2 || !number(n,"scale",1,scale) || !map_offset(n,offset))return false;
    return common_output(n,0,g.rank,g.rows,g.columns,type==2?DType::UInt64:type==1?DType::Float64:DType::Float32);
}
bool peak_outputs(const dataflow_runtime::NumericOperationNodeView& n,const CommonArrayGeometry& g,DType type,std::size_t& bytes) noexcept {
    std::size_t count=0;if(g.rank!=Rank::Vector || !integer(n,"maximum_count",5,count) || !count)return false;
    return dsp_kernels::prepared_storage::add_array(bytes,g.rows,sizeof(double)+sizeof(std::uint64_t)) &&
        common_output(n,0,Rank::Vector,std::min(count,g.rows),1,DType::UInt64) && common_output(n,1,Rank::Vector,std::min(count,g.rows),1,type);
}
bool reduction_resources(const dataflow_runtime::NumericOperationNodeView& n,const CommonArrayGeometry& g,DType type,std::size_t& object,std::size_t& bytes) noexcept {
    const auto size=common_array_element_bytes(type);auto count=g.width();
    if(n.kind=="AxisMaximum")object=type==DType::Float64?sizeof(Maximum<double>):sizeof(Maximum<float>);
    else {
        object=type==DType::Float64?sizeof(Sum<double>):sizeof(Sum<float>);bool cumulative=false;
        dsp_kernels::AxisSumAccumulation accumulation;
        if(!sum_configuration(n,cumulative,accumulation) || !dsp_kernels::prepared_storage::add_bytes(count,cumulative?g.width():1))return false;
    }
    return dsp_kernels::prepared_storage::add_array(bytes,count,size) && reduction_outputs(n,g,type);
}
bool axis_resources(const dataflow_runtime::NumericOperationNodeView& n,const CommonArrayGeometry& g,DType type,std::size_t& object,std::size_t& bytes) noexcept {
    if(n.kind=="AxisMaximum" || n.kind=="AxisSum")return reduction_resources(n,g,type,object,bytes);
    if(n.kind=="ArrayGather"){object=sizeof(Gather);return gather_output(n,g,type);}
    if(n.kind=="NumericMap"){object=sizeof(Map);return map_output(n,g);}
    if(n.kind=="OrderedPeaks"){object=sizeof(OrderedPeaks);return peak_outputs(n,g,type,bytes);}
    return false;
}

template<class T> PreparedCommon make(const dataflow_runtime::NumericOperationNodeView& n){auto value=std::make_unique<T>();return value->prepare(n)?std::move(value):nullptr;}
}
PreparedCommon prepare_common_axis(const dataflow_runtime::NumericOperationNodeView& n){
    if(n.kind=="AxisMaximum")return contract(n,domain::PortDirection::Input,0)->dtype==DType::Float64?make<Maximum<double>>(n):make<Maximum<float>>(n);
    if(n.kind=="AxisSum")return contract(n,domain::PortDirection::Input,0)->dtype==DType::Float64?make<Sum<double>>(n):make<Sum<float>>(n);
    if(n.kind=="ArrayGather")return make<Gather>(n);
    if(n.kind=="NumericMap")return make<Map>(n);
    if(n.kind=="OrderedPeaks")return make<OrderedPeaks>(n);
    return {};
}
domain::ProcessingSupport assess_common_axis(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    CommonArrayGeometry geometry;std::size_t object=0,bytes=0;domain::ProcessingSupport support;
    if(!(n.kind=="NumericMap"?map_geometry(n,geometry):floating(n,0,geometry)))return {};
    const auto type=contract(n,domain::PortDirection::Input,0)->dtype;
    if(!axis_resources(n,geometry,type,object,bytes))return {};
    return common_resources(n,p,object,bytes,support)?support:domain::ProcessingSupport{};
}
}
