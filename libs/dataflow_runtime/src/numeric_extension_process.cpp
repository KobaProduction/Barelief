#include "numeric_extension_state.h"
#include <algorithm>

namespace dataflow_runtime::detail {
Status PreparedNumericExtension::Impl::process(const PlanNode& node,State& state,const FrameLimits& limits) noexcept {
    if(!processor || outputs.size()!=state.numeric_outputs.size())return Status::incompatible_contract;
    targets(node,state);
    const bool discontinuity=std::any_of(latches.begin(),latches.end(),[](const auto& input){return input.ready && input.rule.use==NumericInputUse::Frame && input.stamp.discontinuity;});
    control.reset=control.reset || discontinuity;if(control.reset)processor->reset();
    control.enabled=node.node.enabled;
    auto status=processor->process(inputs,outputs,control);
    if(status==Status::ok)status=validate_outputs(state,limits);
    if(status!=Status::ok){
        if(descriptor->policy.arithmetic_failure==NumericOperationFailure::ResetState)reset();else consume();
        invalidate(state,status);return status;
    }
    publish(state);consume();return Status::ok;
}
} // namespace dataflow_runtime::detail
