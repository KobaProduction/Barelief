#include "operation_kernel_state.h"
#include "node_state.h"
#include "preparation_support.h"
#include "frame_access.h"
#include "signal_time.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cmath>
#include <limits>
namespace dataflow_runtime::detail {
using namespace domain;using namespace signal_contracts;
namespace {
Status prepare_product(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& n,const NumericRuntimeProfile& profile) {
    kernel.add_arrays=operation_text(n,"operation")=="add";
    ProcessingParameterView clamp;
    if(resolve_processing_parameter(n,"floor_enabled",clamp)!=Status::ok || !std::holds_alternative<bool>(clamp) || !numeric_parameter(n,"floor",kernel.minimum))return Status::invalid_argument;
    kernel.clamp_floor=std::get<bool>(clamp);
    kernel.frame_input=node.product_frame_input;
    kernel.factors.resize(kernel.input.size()*n.inputs.size());
    kernel.factor_ready.resize(n.inputs.size());kernel.factor_initial_ready.resize(n.inputs.size());
    kernel.data_stream.resize(profile.admission.frame.max_string_bytes);
    for(std::size_t i=0;i<n.inputs.size();++i)
        if(node.endpoint_plan->inputs[i].parent==absent)kernel.factor_ready[i]=kernel.factor_initial_ready[i]=true;
    return Status::ok;
}
}
Status prepare_numeric_operation(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& n,const NumericRuntimeProfile& profile) {
    if(n.operation==ProcessingOperation::ArrayProduct)return prepare_product(kernel,node,n,profile);
    if(n.operation==ProcessingOperation::ArrayConcat){
        const auto right=n.inputs[1].contract.view();kernel.imaginary.resize(std::size_t(std::get<NumericContract>(right).shape.extents[0].maximum));return Status::ok;
    }
    return Status::unavailable;
}
}
