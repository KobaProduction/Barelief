#include "common_support.h"
#include <dsp_kernels/feature_reduction.h>
#include <dsp_kernels/prepared_storage.h>
namespace dsp_graph_adapters::detail {
namespace {
using namespace signal_contracts;using namespace dataflow_runtime;
struct Geometry {std::size_t rows=0,columns=0,method=0;Rank output=Rank::Vector;bool compensated=true;};
bool geometry(const dataflow_runtime::NumericOperationNodeView& n,Geometry& g) noexcept {
    if(!integer(n,"method",2,g.method) || g.method<1 || g.method>4 || !flag(n,"compensated",true,g.compensated))return false;
    const auto input=contract(n,domain::PortDirection::Input,0);if(!input)return false;
    if(input->shape.rank==Rank::Matrix)return input_geometry(n,0,Rank::Matrix,g.rows,g.columns);
    g.rows=1;g.output=Rank::Scalar;std::size_t unused=0;
    return input_geometry(n,0,Rank::Vector,g.columns,unused);
}
class Aggregate final:public CommonKernel<Aggregate> {
    Geometry shape_;
    std::vector<float> scratch_;
    bool row(const NumericBufferView& input,std::size_t at,float& value) noexcept {
        const bool matrix=input.contract->shape.rank==Rank::Matrix;
        auto values=std::span(scratch_).first(input.actual[matrix?1:0]);
        for(std::size_t column=0;column<values.size();++column)
            if(load_element(input,{0,matrix?at:column,matrix?column:0},values[column])!=Status::ok)return false;
        const auto summation=shape_.compensated?dsp_kernels::Float32Summation::Compensated:dsp_kernels::Float32Summation::Plain;
        return dsp_kernels::reduce_feature_values(values,dsp_kernels::FeatureReduction(shape_.method),value,false,summation);
    }
    bool run(std::span<const NumericOperationInput> input,std::span<NumericOperationOutput> output) noexcept override {
        const auto& source=input[0].value;const auto count=source.contract->shape.rank==Rank::Matrix?source.actual[0]:1;
        if(!set_shape(output[0],count))return false;
        for(std::size_t at=0;at<count;++at){float value=0;
            if(!row(source,at,value) || store_element(output[0].value,{0,at,0},value)!=Status::ok)return false;
        }
        return true;
    }
public:
    bool prepare(const dataflow_runtime::NumericOperationNodeView& n){if(!geometry(n,shape_))return false;scratch_.resize(shape_.columns);return true;}
    std::size_t workspace_bytes() const noexcept override {return scratch_.capacity()*sizeof(float);}
};
}
PreparedCommon prepare_feature_aggregate(const dataflow_runtime::NumericOperationNodeView& n){
    auto value=std::make_unique<Aggregate>();return value->prepare(n)?std::move(value):nullptr;
}
domain::ProcessingSupport assess_feature_aggregate(const dataflow_runtime::NumericOperationNodeView& n,const dataflow_runtime::NumericOperationProfile& p) noexcept {
    Geometry g;domain::ProcessingSupport support;std::size_t bytes=0;
    if(!geometry(n,g) || !dsp_kernels::prepared_storage::add_array(bytes,g.columns,sizeof(float)) ||
       !common_output(n,0,g.output,g.rows) || !common_resources(n,p,sizeof(Aggregate),bytes,support))return {};
    return support;
}
}
