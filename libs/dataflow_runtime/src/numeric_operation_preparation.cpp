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
void prepare_product_factors(OperationKernelState& kernel,const ResolvedProcessingNode& n) {
    if(std::get<NumericContract>(n.outputs[0].contract.view()).dtype==DType::Float64)kernel.factors64.resize(kernel.input.size()*n.inputs.size());
    else kernel.factors.resize(kernel.input.size()*n.inputs.size());
    kernel.factor_ready.resize(n.inputs.size());kernel.factor_initial_ready.resize(n.inputs.size());
    kernel.factor_actual.resize(n.inputs.size());
}
void prepare_concat_coordinates(OperationKernelState& kernel,const ResolvedProcessingNode& node) {
    for(std::size_t lane=0;lane<2;++lane){
        const auto input=std::get<NumericContract>(node.inputs[lane].contract.view());
        if(input.shape.rank!=Rank::Matrix)continue;
        for(std::size_t dimension=0;dimension<2;++dimension)
            if(input.coordinates[dimension].mode==CoordinateMode::ExplicitFloat64LE)
                kernel.pair_coordinates[lane][dimension].resize(std::size_t(input.shape.extents[dimension].maximum)*sizeof(double));
    }
}
Status prepare_concat(OperationKernelState& kernel,const ResolvedProcessingNode& node) {
    const auto axis=domain::array_concat_axis(node);if(!axis)return Status::invalid_argument;
    kernel.concat_axis=*axis;
    const auto right=std::get<NumericContract>(node.inputs[1].contract.view());
    kernel.imaginary.resize(std::size_t(right.shape.extents[0].maximum)*(right.shape.rank==Rank::Matrix?std::size_t(right.shape.extents[1].maximum):1));
    prepare_concat_coordinates(kernel,node);
    return Status::ok;
}
void initialize_product_inputs(OperationKernelState& kernel,const PlanNode& node,const ResolvedProcessingNode& n) {
    for(std::size_t i=0;i<n.inputs.size();++i)if(n.inputs[i].id==PortId(BinaryInput::Left))kernel.binary_left_input=i;
    for(std::size_t i=0;i<n.inputs.size();++i)if(node.endpoint_plan->inputs[i].parent==absent){
        kernel.factor_ready[i]=kernel.factor_initial_ready[i]=true;
        // An unwired factor is a ready typed zero on the leading clock.
        if(kernel.trigger_input==i)kernel.trigger_input=automatic_array_trigger;
    }
}
Status prepare_product(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& n,const NumericRuntimeProfile& profile) {
    const auto operation=domain::array_math_operation(operation_text(n,"operation"));
    if(!operation)return Status::invalid_argument;
    kernel.array_operation=*operation;
    if(*operation==domain::ArrayMathOperation::Divide && !domain::zero_division_policy(operation_text(n,"zero_division")))return Status::invalid_argument;
    ProcessingParameterView clamp;
    if(resolve_processing_parameter(n,"floor_enabled",clamp)!=Status::ok || !std::holds_alternative<bool>(clamp) || !numeric_parameter(n,"floor",kernel.minimum))return Status::invalid_argument;
    kernel.clamp_floor=std::get<bool>(clamp);
    kernel.frame_input=node.product_frame_input;kernel.trigger_input=node.product_trigger_input;
    prepare_product_factors(kernel,n);
    kernel.data_stream.resize(profile.admission.frame.max_string_bytes);
    initialize_product_inputs(kernel,node,n);
    return Status::ok;
}
}
Status prepare_numeric_operation(OperationKernelState& kernel,PlanNode& node,const ResolvedProcessingNode& n,const NumericRuntimeProfile& profile) {
    if(n.operation==ProcessingOperation::ArrayProduct)return prepare_product(kernel,node,n,profile);
    if(n.operation==ProcessingOperation::ArrayConcat)return prepare_concat(kernel,n);
    return Status::unavailable;
}
}
