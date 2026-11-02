#include "common_support.h"
#include <domain/numeric/common_operations.h>

namespace dsp_graph_adapters::detail {
using namespace dataflow_runtime;using namespace signal_contracts;
bool common_operation_kind(std::string_view kind) noexcept {return domain::common_operation_kind(kind);}
Status describe_common_operation(const domain::ResolvedProcessingNode& n,const NumericOperationFactory* identity,
    std::shared_ptr<const NumericOperationDescriptor>& out){
    if(!common_operation_kind(n.kind))return Status::unavailable;
    NumericOperationPolicy policy;
    policy.inputs.reserve(n.inputs.size());policy.output_values.reserve(n.outputs.size());
    if(n.kind=="RecursiveFilter" || n.kind=="CombFilterBank")policy.mode=NumericOperationMode::ContinuingStream;
    for(const auto& p:n.inputs)policy.inputs.push_back({p.id,n.kind=="CurveInterpolation" && p.id==1?NumericInputUse::HeldValue:NumericInputUse::Frame,true,
        n.kind=="CurveInterpolation" && p.id==1?ValueDomain::AnyIeee:ValueDomain::FiniteOnly});
    for(std::size_t k=0;k<n.outputs.size();++k)policy.output_values.push_back(n.kind=="LogCompression" || n.kind=="CurveInterpolation"?ValueDomain::AnyIeee:ValueDomain::FiniteOnly);
    return own_numeric_operation_descriptor(n,std::move(policy),out,identity);
}
domain::ProcessingSupport assess_common_operation(const domain::ResolvedProcessingNode& n,const NumericRuntimeProfile& p) noexcept {
    if(n.kind=="FilterMatrixProjection" || n.kind=="RecursiveFilter")return assess_common_filters(n,p);
    if(n.kind=="CurveInterpolation" || n.kind=="CombFilterBank" || n.kind=="ValueHistogram" || n.kind=="InterpolatedPeaks")return assess_common_sequences(n,p);
    return common_operation_kind(n.kind)?assess_common_values(n,p):domain::ProcessingSupport{};
}
Status prepare_common_operation(std::shared_ptr<const NumericOperationDescriptor> descriptor,const NumericRuntimeProfile& p,
    std::unique_ptr<PreparedNumericOperation>& output){
    if(!descriptor)return Status::invalid_argument;
    const auto n=definition(descriptor->node);const auto support=assess_common_operation(n,p);
    if(!support.supported)return Status::incompatible_contract;
    auto next=prepare_common_values(n);if(!next)next=prepare_common_filters(n);if(!next)next=prepare_common_sequences(n);
    if(!next)return Status::invalid_argument;
    next->bind(std::move(descriptor),p.admission);domain::ProcessingResources actual;
    if(!next->owned_resources(actual) || !within(actual,support.resources))return Status::exhausted;
    output=std::move(next);return Status::ok;
}
}
