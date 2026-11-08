#include "common_support.h"
#include "operation_constraints.h"
#include <domain/numeric/common_operations.h>

namespace dsp_graph_adapters::detail {
using namespace dataflow_runtime;using namespace signal_contracts;
bool common_operation_kind(std::string_view kind) noexcept {return domain::common_operation_kind(kind);}
namespace {
void common_value_policy(const NumericOperationNodeView& node,NumericOperationPolicy& policy) {
    policy.inputs.reserve(node.inputs.size());policy.output_values.reserve(node.outputs.size());
    for(const auto& port:node.inputs){
        const bool held=((node.kind=="CurveInterpolation" || node.kind=="SameConvolution") && port.id==1) || (node.kind=="ArrayGather" && !port.id);
        policy.inputs.push_back({port.id,held?NumericInputUse::HeldValue:NumericInputUse::Frame,true,node.kind=="CurveInterpolation" && held?ValueDomain::AnyIeee:ValueDomain::FiniteOnly});
    }
    for(std::size_t i=0;i<node.outputs.size();++i)
        policy.output_values.push_back(node.kind=="LogCompression" || node.kind=="CurveInterpolation"?ValueDomain::AnyIeee:ValueDomain::FiniteOnly);
}
PreparedCommon prepare_common_kernel(const NumericOperationNodeView& node) {
    if(node.kind=="AbsoluteValue")return prepare_absolute_value(node);
    auto axis=prepare_common_axis(node);if(axis)return axis;
    auto next=prepare_common_values(node);if(!next)next=prepare_common_filters(node);
    return next?std::move(next):prepare_common_sequences(node);
}
bool common_stream_policy(const NumericOperationNodeView& n,NumericOperationPolicy& policy) {
    std::size_t state_policy=0;
    if(n.kind=="RecursiveFilter" && !integer(n,"state_policy",0,state_policy))return false;
    bool reset_comb=false;
    if(n.kind=="CombFilterBank" && !flag(n,"reset_per_call",false,reset_comb))return false;
    if((n.kind=="RecursiveFilter" && !state_policy) || (n.kind=="CombFilterBank" && !reset_comb))policy.mode=NumericOperationMode::ContinuingStream;
    return true;
}
}
Status describe_common_operation(const dataflow_runtime::NumericOperationNodeView& n,const NumericOperationFactory* identity,
    std::shared_ptr<const NumericOperationDescriptor>& out){
    if(!common_operation_kind(n.kind))return Status::unavailable;
    NumericOperationPolicy policy;
    if(!common_stream_policy(n,policy))return Status::invalid_argument;
    common_value_policy(n,policy);
    if(n.kind=="AbsoluteValue"){
        NumericOperationMeaningRule meaning;meaning.direction=domain::PortDirection::Output;meaning.same_unit_as_input=0;
        policy.meanings.push_back(std::move(meaning));
    }
    if(!common_constraints(n,policy))return Status::invalid_argument;
    return own_numeric_operation_descriptor(n,std::move(policy),out,identity);
}
domain::ProcessingSupport assess_common_operation(const dataflow_runtime::NumericOperationNodeView& n,const NumericOperationProfile& p) noexcept {
    if(n.kind=="AbsoluteValue")return assess_absolute_value(n,p);
    if(n.kind=="AxisMaximum" || n.kind=="AxisSum" || n.kind=="ArrayGather" || n.kind=="NumericMap" || n.kind=="OrderedPeaks")return assess_common_axis(n,p);
    if(n.kind=="FilterMatrixProjection" || n.kind=="RecursiveFilter")return assess_common_filters(n,p);
    if(n.kind=="LinearExpansion" || n.kind=="CurveInterpolation" || n.kind=="CombFilterBank" || n.kind=="ValueHistogram" || n.kind=="InterpolatedPeaks")return assess_common_sequences(n,p);
    return common_operation_kind(n.kind)?assess_common_values(n,p):domain::ProcessingSupport{};
}
Status prepare_common_operation(std::shared_ptr<const NumericOperationDescriptor> descriptor,const NumericOperationProfile& p,
    std::unique_ptr<PreparedNumericOperation>& output){
    if(!descriptor)return Status::invalid_argument;
    const auto n=definition(descriptor->node);const auto support=assess_common_operation(n,p);
    if(!support.supported)return Status::incompatible_contract;
    auto next=prepare_common_kernel(n);
    if(!next)return Status::invalid_argument;
    next->bind(std::move(descriptor),p.admission);domain::ProcessingResources actual;
    if(!next->owned_resources(actual) || !within(actual,support.resources))return Status::exhausted;
    output=std::move(next);return Status::ok;
}
}
