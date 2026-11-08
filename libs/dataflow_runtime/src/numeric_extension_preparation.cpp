#include "numeric_extension_state.h"
#include "preparation_support.h"
#include <algorithm>
#include <numeric>
#include <utility>

namespace dataflow_runtime::detail {
namespace {
bool same_ports(std::span<const ProcessingPort> left,std::span<const ProcessingPort> right) noexcept {
    if(left.size()!=right.size())return false;
    for(std::size_t i=0;i<left.size();++i)if(left[i].id!=right[i].id || left[i].direction!=right[i].direction || !same_physical_contract(left[i].contract.view(),right[i].contract.view()))return false;
    return true;
}
bool rules_valid(const NumericOperationDescriptor& descriptor) noexcept {
    const auto& policy=descriptor.policy;bool trigger=false,control=!policy.control;
    if(policy.inputs.size()!=descriptor.node.inputs.size())return false;
    for(std::size_t i=0;i<policy.inputs.size();++i){
        const auto rule=policy.inputs[i];if(rule.port!=descriptor.node.inputs[i].id || unsigned(rule.use)>unsigned(NumericInputUse::Control) || unsigned(rule.values)>unsigned(ValueDomain::AnyIeee))return false;
        trigger|=rule.port==policy.trigger && rule.use!=NumericInputUse::HeldValue;
        if(policy.control && rule.port==*policy.control){
            const auto& numeric=descriptor.node.inputs[i].contract;
            control=rule.use==NumericInputUse::Control && numeric.dtype==DType::UInt8 && numeric.shape.rank==Rank::Scalar;
        }
    }
    return trigger && control;
}
bool enum_policy_valid(const NumericOperationPolicy& policy) noexcept {
    return unsigned(policy.mode)<=unsigned(NumericOperationMode::ContinuingStream) &&
        unsigned(policy.match)<=unsigned(NumericInputMatch::ExactInterval) &&
        unsigned(policy.arithmetic_failure)<=unsigned(NumericOperationFailure::ResetState) &&
        unsigned(policy.session)<=unsigned(NumericInputSession::SharedProvenance);
}
bool compatible_clock(const ClockContract& left,const ClockContract& right) noexcept {
    if(left.domain.empty() || left.domain!=right.domain || left.sample_rate!=right.sample_rate ||
       left.tick_seconds.numerator<=0 || right.tick_seconds.numerator<=0 || !left.tick_seconds.denominator || !right.tick_seconds.denominator)return false;
    const auto a=std::gcd(std::uint64_t(left.tick_seconds.numerator),left.tick_seconds.denominator);
    const auto b=std::gcd(std::uint64_t(right.tick_seconds.numerator),right.tick_seconds.denominator);
    return std::uint64_t(left.tick_seconds.numerator)/a==std::uint64_t(right.tick_seconds.numerator)/b &&
        left.tick_seconds.denominator/a==right.tick_seconds.denominator/b;
}
bool join_clock(const ProcessingPort& port,ClockContract& anchor) noexcept {
    const auto contract=port.contract.view();
    const auto& clock=std::get<NumericContract>(contract).clock;
    if(!compatible_clock(anchor.domain.empty()?clock:anchor,clock))return false;
    anchor=clock;return true;
}
bool interval_clocks_valid(const ResolvedProcessingNode& source,const NumericOperationPolicy& policy) noexcept {
    ClockContract anchor;
    for(std::size_t i=0;i<policy.inputs.size();++i)
        if(policy.inputs[i].use==NumericInputUse::Frame && !join_clock(source.inputs[i],anchor))return false;
    return true;
}
bool join_policy_valid(const ResolvedProcessingNode& source,const NumericOperationDescriptor& descriptor) noexcept {
    const auto& policy=descriptor.policy;
    if(policy.match==NumericInputMatch::IndependentBatch)return true;
    if(policy.session!=NumericInputSession::SharedProvenance)return false;
    return policy.match!=NumericInputMatch::ExactInterval || interval_clocks_valid(source,policy);
}
bool output_policy_valid(const NumericOperationDescriptor& descriptor) noexcept {
    const auto& values=descriptor.policy.output_values;
    return (values.empty() || values.size()==descriptor.node.outputs.size()) &&
        std::all_of(values.begin(),values.end(),[](auto value){return unsigned(value)<=unsigned(ValueDomain::AnyIeee);});
}
bool fits(const ProcessingResources& actual,const ProcessingResources& admitted) noexcept {
    return actual.plan_bytes<=admitted.plan_bytes && actual.payload_bytes<=admitted.payload_bytes &&
        actual.scratch_bytes<=admitted.scratch_bytes && actual.stack_bytes<=admitted.stack_bytes;
}
bool numeric_ports(const ResolvedProcessingNode& node) noexcept {
    const auto numeric=[](const ProcessingPort& port){return std::holds_alternative<NumericContract>(port.contract.view());};
    return std::all_of(node.inputs.begin(),node.inputs.end(),numeric) && std::all_of(node.outputs.begin(),node.outputs.end(),numeric);
}
bool admitted_instance(const PreparedNumericOperation& operation,const NumericOperationDescriptor& descriptor,
                       const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile,const ProcessingSupport& support) noexcept {
    std::size_t bound=0,actual=0;ProcessingResources owned;
    return numeric_operation_descriptor_required_bytes(numeric_operation_node_view(source),profile.admission,bound) &&
        numeric_operation_descriptor_owned_bytes(descriptor,profile.admission,actual) && actual<=bound &&
        operation.owned_resources(owned) && fits(owned,support.resources);
}
}
namespace {
bool projected_ports(std::span<const ProcessingPort> source,const std::vector<NumericOperationPort>& ports) noexcept {
    if(source.size()!=ports.size())return false;
    for(std::size_t i=0;i<source.size();++i){
        const auto full=source[i].contract.view();const auto* numeric=std::get_if<NumericContract>(&full);
        if(!numeric || source[i].id!=ports[i].id || source[i].direction!=ports[i].direction ||
           NumericOperationBindingAccess::storage(*numeric)!=ports[i].contract)return false;
    }
    return true;
}
}
bool extension_descriptor_valid(const ResolvedProcessingNode& source,const NumericOperationDescriptor& descriptor) noexcept {
    if(!source.parameters || descriptor.node.id!=source.id || descriptor.node.kind!=source.kind || descriptor.node.enabled!=source.enabled ||
       !same_processing_parameters(descriptor.node.parameters,*source.parameters) || !projected_ports(source.inputs,descriptor.node.inputs) ||
       !projected_ports(source.outputs,descriptor.node.outputs))return false;
    for(std::size_t i=0;i<descriptor.node.outputs.size();++i)if(descriptor.node.outputs[i].id!=i)return false;
    return enum_policy_valid(descriptor.policy) && rules_valid(descriptor) && output_policy_valid(descriptor) &&
        join_policy_valid(source,descriptor) && numeric_operation_metadata_valid(source,descriptor.policy);
}
Status PreparedNumericExtension::Impl::prepare_latches(const NumericRuntimeProfile& profile) {
    latches.resize(descriptor->node.inputs.size());inputs.resize(latches.size());
    for(std::size_t i=0;i<latches.size();++i){
        auto& latch=latches[i];latch.rule=descriptor->policy.inputs[i];
        latch.contract=std::get<NumericContract>(input_ports[i].contract.view());
        latch.storage_contract=NumericOperationBindingAccess::storage(latch.contract);
        const auto status=numeric_buffer(latch.contract,latch.buffer);if(status!=Status::ok)return status;
        latch.stream.resize(profile.admission.frame.max_string_bytes);
    }
    return prepare_outputs(profile);
}
Status PreparedNumericExtension::Impl::prepare_outputs(const NumericRuntimeProfile& profile) {
    outputs.resize(descriptor->node.outputs.size());output_contracts.resize(outputs.size());
    output_streams.resize(outputs.size());output_stream_sizes.resize(outputs.size());
    output_stamps.resize(outputs.size());output_statuses.resize(outputs.size(),Status::unavailable);
    for(auto& stamp:output_stamps)stamp.available=false;
    for(std::size_t i=0;i<outputs.size();++i){output_contracts[i]=std::get<NumericContract>(output_ports[i].contract.view());output_streams[i].resize(profile.admission.frame.max_string_bytes);}
    return Status::ok;
}
namespace {
template<class Extension> Status bind_descriptor(Extension& state,const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile) {
    state.factory=profile.operation_factory;if(!state.factory)return Status::unavailable;
    if(!numeric_ports(source))return Status::incompatible_contract;
    const auto status=state.factory->describe(numeric_operation_node_view(source),state.descriptor);if(status!=Status::ok)return status;
    return state.descriptor && extension_descriptor_valid(source,*state.descriptor)?Status::ok:Status::incompatible_contract;
}
}
Status PreparedNumericExtension::Impl::bind(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile) {
    auto status=bind_descriptor(*this,source,profile);if(status!=Status::ok)return status;
    input_ports.assign(source.inputs.begin(),source.inputs.end());output_ports.assign(source.outputs.begin(),source.outputs.end());
    status=prepare_latches(profile);if(status!=Status::ok)return status;
    prepare_invocation();return Status::ok;
}
Status PreparedNumericExtension::Impl::configure(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile) {
    auto status=bind(source,profile);if(status!=Status::ok)return status;
    const auto limited=numeric_operation_profile(profile);
    const auto support=factory->assess(numeric_operation_node_view(source),limited);if(!support.supported)return Status::incompatible_contract;
    ProcessingResources actual,bound;
    if(!extension_owned_resources(source,profile,bound) || !owned_resources(actual) || !fits(actual,bound))return Status::exhausted;
    status=factory->prepare(descriptor,limited,processor);if(status!=Status::ok)return status;
    return processor && admitted_instance(*processor,*descriptor,source,profile,support)?Status::ok:Status::exhausted;
}
PreparedNumericExtension::Impl::Impl(const Impl& other)
    :factory(other.factory),descriptor(other.descriptor),processor(other.processor?other.processor->clone():nullptr),
     input_ports(other.input_ports),output_ports(other.output_ports),
     latches(other.latches),inputs(other.inputs.size()),outputs(other.outputs.size()),output_contracts(other.output_contracts),
     output_streams(other.output_streams),output_stream_sizes(other.output_stream_sizes),output_stamps(other.output_stamps),
     output_statuses(other.output_statuses),triggered(other.triggered),control(other.control) {
    prepare_invocation();authority=other.authority;input_clocks=other.input_clocks;
}
PreparedNumericExtension::PreparedNumericExtension():value_(std::make_unique<Impl>()){}
PreparedNumericExtension::~PreparedNumericExtension()=default;
PreparedNumericExtension::PreparedNumericExtension(const PreparedNumericExtension& other):value_(std::make_unique<Impl>(*other.value_)){}
PreparedNumericExtension& PreparedNumericExtension::operator=(const PreparedNumericExtension& other){if(this!=&other){auto next=std::make_unique<Impl>(*other.value_);value_.swap(next);}return *this;}
PreparedNumericExtension::PreparedNumericExtension(PreparedNumericExtension&&) noexcept=default;
PreparedNumericExtension& PreparedNumericExtension::operator=(PreparedNumericExtension&&) noexcept=default;
Status PreparedNumericExtension::prepare(const ResolvedProcessingNode& source,const NumericRuntimeProfile& profile) {
    auto next=std::make_unique<Impl>();const auto status=next->configure(source,profile);
    if(status==Status::ok)value_.swap(next);return status;
}
bool PreparedNumericExtension::Impl::compatible(const Impl& other) const noexcept {
    if(!descriptor || !other.descriptor || !processor || !other.processor)return false;
    const auto& node=descriptor->node;const auto& before=other.descriptor->node;
    return factory.get()==other.factory.get() && descriptor->policy==other.descriptor->policy && node.kind==before.kind && same_processing_parameters(node.parameters,before.parameters) &&
        same_ports(input_ports,other.input_ports) && same_ports(output_ports,other.output_ports);
}
namespace {
template<class Extension> bool copy_latches(Extension& target,const Extension& source) noexcept {
    for(std::size_t i=0;i<target.latches.size();++i)
        if(!target.latches[i].copy_state_from(source.latches[i]))return false;
    return true;
}
template<class Extension> Status restore_extension(Extension& target,const Extension& source) noexcept {
    const auto status=target.processor->copy_state_from(*source.processor);if(status!=Status::ok)return status;
    if(!copy_latches(target,source) || !target.copy_publication(source))return Status::incompatible_contract;
    target.authority=source.authority;target.input_clocks=source.input_clocks;
    target.triggered=source.triggered;target.control=source.control;return Status::ok;
}
}
Status PreparedNumericExtension::copy_state_from(const PreparedNumericExtension& other) noexcept {
    if(!value_->compatible(*other.value_)){reset();return Status::incompatible_contract;}
    const auto status=restore_extension(*value_,*other.value_);
    if(status!=Status::ok)reset();
    return status;
}
void PreparedNumericExtension::reset() noexcept {value_->reset();}
} // namespace dataflow_runtime::detail
