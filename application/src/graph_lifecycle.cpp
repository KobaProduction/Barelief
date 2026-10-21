#include <application/graph_lifecycle.h>
#include <algorithm>
#include <utility>
#include <new>
#include <stdexcept>
namespace application {
namespace {
bool same_ports(std::span<const domain::ProcessingPort> left,std::span<const domain::ProcessingPort> right) noexcept {
    if(left.size()!=right.size())return false;
    for(std::size_t i=0;i<left.size();++i)
        if(left[i].id!=right[i].id || left[i].direction!=right[i].direction || !signal_contracts::same_contract(left[i].contract.view(),right[i].contract.view()))return false;
    return true;
}
bool same_node(const domain::ConfiguredProcessingNode& left,const domain::ConfiguredProcessingNode& right) noexcept {
    // clock_node is a derived cache; only explicit associations define identity.
    return left.id==right.id && left.kind==right.kind && left.enabled==right.enabled && domain::same_processing_parameters(left.parameters,right.parameters) && same_ports(left.inputs,right.inputs) && same_ports(left.outputs,right.outputs);
}
}
bool same_numeric_processing_graph(const domain::NumericProcessingGraph& a,const domain::NumericProcessingGraph& b) noexcept {
    const auto& x=a.versions;const auto& y=b.versions;
    if(a.graph_id!=b.graph_id || a.execution_interval!=b.execution_interval || x.graph!=y.graph || x.processing!=y.processing ||
       a.edges!=b.edges || a.configuration_bindings!=b.configuration_bindings || a.nodes.size()!=b.nodes.size())return false;
    // clock_node is derived during preparation from explicit configuration
    // associations. It is absent on the wire and cannot define graph identity.
    for(std::size_t i=0;i<a.nodes.size();++i)if(!same_node(a.nodes[i],b.nodes[i]))return false;
    return true;
}

bool MemoryRequirements::fits(std::size_t budget) const noexcept {
    if(state_bytes>budget)return false;
    budget-=state_bytes;if(buffer_bytes>budget)return false;
    return scratch_bytes<=budget-buffer_bytes;
}
GraphLifecycle::GraphLifecycle(NumericGraphPreparationPort& preparation,GraphRuntimePort& runtime,
    domain::NumericDeviceCapabilities capabilities,NumericRuntimeLimits limits)
    :preparation_(preparation),runtime_(runtime),capabilities_(capabilities),limits_(limits) {}
LifecycleResult GraphLifecycle::prepare_transactional(const domain::NumericProcessingGraph& graph) {
    return prepare_numeric_transactional(graph,limits_);
}
namespace {
LifecycleResult evaluate_preparation_graph(const domain::NumericProcessingGraph& requested,const NumericRuntimeLimits& limits,
    domain::NumericProcessingGraph& evaluated,domain::NumericProcessingGraph& executable) {
    domain::NumericProcessingGraph processing;
    const auto values=domain::resolve_numeric_parameter_bindings(requested,limits.admission,evaluated,processing,-1,&limits.preparation_values);
    if(!values)return {Status::invalid_argument,values};
    std::vector<domain::RoutingAlias> aliases;
    const auto outputs=domain::lower_numeric_metadata_outputs(processing,limits.admission,executable,aliases,&limits.preparation_values);
    return outputs?LifecycleResult{}:LifecycleResult{Status::invalid_argument,outputs};
}
LifecycleResult preparation_capabilities(const domain::NumericDeviceCapabilities& configured,const NumericRuntimeLimits& limits,
    const NumericRuntimeLimits& baseline,domain::NumericDeviceCapabilities& out) {
    auto candidate=configured;candidate.profile=limits.resources;
    const auto overhead=configured.profile.retained_working_bytes>baseline.resources.retained_working_bytes?
        configured.profile.retained_working_bytes-baseline.resources.retained_working_bytes:0;
    if(overhead>SIZE_MAX-candidate.profile.retained_working_bytes)return {Status::exhausted,{domain::ProcessingError::ResourceLimit}};
    candidate.profile.retained_working_bytes+=overhead;out=candidate;return {};
}
LifecycleResult admit_preparation_graph(const domain::NumericProcessingGraph& graph,const NumericRuntimeLimits& limits,
    const domain::NumericDeviceCapabilities& capabilities) {
    if(graph.nodes.size()>limits.admission.max_nodes)return {Status::exhausted,{domain::ProcessingError::Limit}};
    if(graph.nodes.size()>std::vector<std::size_t>().max_size()/2)return {Status::overflow,{domain::ProcessingError::Limit}};
    std::vector<std::size_t> workspace(graph.nodes.size()*2);domain::ProcessingGraphAdmission admitted;
    const auto diagnostic=domain::admit_processing_graph(graph,limits.admission,capabilities,workspace,admitted);
    const auto status=diagnostic.error==domain::ProcessingError::ResourceLimit?Status::exhausted:Status::invalid_argument;
    return diagnostic?LifecycleResult{}:LifecycleResult{status,diagnostic};
}
LifecycleResult prepare_execution(NumericGraphPreparationPort& port,const domain::NumericProcessingGraph& graph,
    const NumericRuntimeLimits& limits,PreparedExecution& candidate) {
    const auto status=port.prepare_numeric(graph,limits,candidate);
    if(status!=Status::ok)return {status,{}};
    const auto* numeric=candidate?candidate.plan->numeric_definition():nullptr;
    if(!numeric || !same_numeric_processing_graph(*numeric,graph))return {Status::invalid_argument,{domain::ProcessingError::InvalidContract}};
    if(!candidate.plan->memory_requirements().fits(limits.resources.max_peak_bytes))return {Status::exhausted,{domain::ProcessingError::ResourceLimit}};
    return {};
}
LifecycleResult stage_numeric_execution(NumericGraphPreparationPort& preparation,const domain::NumericProcessingGraph& requested,
    const NumericRuntimeLimits& limits,const NumericRuntimeLimits& baseline,const domain::NumericDeviceCapabilities& configured,PreparedExecution& candidate) {
    domain::NumericProcessingGraph graph,executable;
    auto result=evaluate_preparation_graph(requested,limits,graph,executable);if(!result)return result;
    domain::NumericDeviceCapabilities capabilities;
    result=preparation_capabilities(configured,limits,baseline,capabilities);if(!result)return result;
    result=admit_preparation_graph(executable,limits,capabilities);if(!result)return result;
    return prepare_execution(preparation,graph,limits,candidate);
}
}
LifecycleResult GraphLifecycle::prepare_numeric_transactional(const domain::NumericProcessingGraph& requested,const NumericRuntimeLimits& limits) {
#if defined(__cpp_exceptions)
    try {
#endif
    PreparedExecution candidate;
    const auto result=stage_numeric_execution(preparation_,requested,limits,limits_,capabilities_,candidate);if(!result)return result;
    pending_=std::move(candidate);return {};
#if defined(__cpp_exceptions)
    }catch(const std::bad_alloc&){return {Status::exhausted,{domain::ProcessingError::ResourceLimit}};}
    catch(const std::length_error&){return {Status::exhausted,{domain::ProcessingError::ResourceLimit}};}
#endif
}
LifecycleResult GraphLifecycle::apply_transactional() {
    if(!pending_)return {Status::invalid_argument,{}};
    const auto plan=pending_.plan;
    const auto status=runtime_.apply(std::move(pending_));
    if(status!=Status::ok)return {status,{}};
    pending_={};working_=plan;state_=LifecycleState::Running;
    return {};
}
LifecycleResult GraphLifecycle::connect(const domain::Edge& edge) {
    const auto* working=working_numeric_graph();if(!working)return {Status::invalid_argument,{}};
    auto graph=*working;graph.edges.push_back(edge);
    auto result=prepare_transactional(graph);return result?apply_transactional():result;
}
LifecycleResult GraphLifecycle::disconnect(domain::NodeId to,std::uint16_t port) {
    const auto* working=working_numeric_graph();if(!working)return {Status::invalid_argument,{}};
    auto graph=*working;
    if(!std::erase_if(graph.edges,[&](const auto& edge){return edge.to==to&&edge.input_port==port;}))return {};
    auto result=prepare_transactional(graph);return result?apply_transactional():result;
}
Status GraphLifecycle::shutdown_transactional() {
    const auto status=runtime_.shutdown();
    if(status==Status::ok){pending_={};working_.reset();state_=LifecycleState::Empty;}return status;
}
} // namespace application
