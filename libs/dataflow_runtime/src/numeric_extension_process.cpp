#include "numeric_extension_state.h"
#include <algorithm>

namespace dataflow_runtime::detail {
namespace {
template<class Extension> void apply_processing_control(Extension& state,bool enabled) noexcept {
    const bool discontinuity=std::any_of(state.latches.begin(),state.latches.end(),[](const auto& input){
        return input.ready && input.rule.use==NumericInputUse::Frame && input.stamp.discontinuity;
    });
    state.control.reset=state.control.reset || discontinuity;
    if(state.control.reset)state.processor->reset();
    state.control.enabled=enabled;
}
template<class Extension> Status invoke_processor(Extension& extension,const State& state,const FrameLimits& limits) noexcept {
    auto status=numeric_operation_metadata_values(extension.invocation,extension.descriptor->policy,PortDirection::Input);
    if(status==Status::ok)status=extension.processor->process(extension.inputs,extension.outputs,extension.control);
    if(status==Status::ok)status=extension.validate_outputs(state,limits);
    return status;
}
template<class Extension> Status recover_failure(Extension& extension,State& state,Status status) noexcept {
    if(extension.descriptor->policy.arithmetic_failure==NumericOperationFailure::ResetState)extension.reset();else extension.consume();
    extension.invalidate(state,status);return status;
}
}
Status PreparedNumericExtension::Impl::process(const PlanNode& node,State& state,const FrameLimits& limits) noexcept {
    if(!processor || outputs.size()!=state.numeric_outputs.size())return Status::incompatible_contract;
    targets(node,state);apply_processing_control(*this,node.node.enabled);
    const auto status=invoke_processor(*this,state,limits);
    if(status!=Status::ok)return recover_failure(*this,state,status);
    publish(state);consume();return Status::ok;
}
} // namespace dataflow_runtime::detail
