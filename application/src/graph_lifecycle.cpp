#include <application/graph_lifecycle.h>
#include <algorithm>
#include <utility>
#include <new>
#include <stdexcept>
namespace application {
bool same_numeric_processing_graph(const domain::NumericProcessingGraph& a,const domain::NumericProcessingGraph& b) noexcept {
    const auto& x=a.versions;const auto& y=b.versions;
    if(a.graph_id!=b.graph_id || a.execution_interval!=b.execution_interval || x.document!=y.document || x.graph!=y.graph || x.processing!=y.processing ||
       x.control!=y.control || x.data!=y.data || a.edges!=b.edges || a.configuration_bindings!=b.configuration_bindings || a.nodes.size()!=b.nodes.size())return false;
    auto ports=[](const auto& left,const auto& right){
        if(left.size()!=right.size())return false;
        for(std::size_t i=0;i<left.size();++i)
            if(left[i].id!=right[i].id || left[i].direction!=right[i].direction ||
               !signal_contracts::same_contract(left[i].contract.view(),right[i].contract.view()))return false;
        return true;
    };
    // clock_node is derived during preparation from explicit configuration
    // associations. It is absent on the wire and cannot define graph identity.
    for(std::size_t i=0;i<a.nodes.size();++i){const auto& left=a.nodes[i];const auto& right=b.nodes[i];
        if(left.id!=right.id || left.kind!=right.kind || left.enabled!=right.enabled ||
           !domain::same_processing_parameters(left.parameters,right.parameters) ||
           !ports(left.inputs,right.inputs) || !ports(left.outputs,right.outputs))return false;
    }
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
LifecycleResult GraphLifecycle::prepare_numeric_transactional(const domain::NumericProcessingGraph& graph,const NumericRuntimeLimits& limits) {
#if defined(__cpp_exceptions)
    try {
#endif
    if(graph.nodes.size()>limits.admission.max_nodes)return {Status::exhausted,{}};
    if(graph.nodes.size()>std::vector<std::size_t>().max_size()/2)return {Status::overflow,{}};
    std::vector<std::size_t> workspace(graph.nodes.size()*2);
    auto caps=capabilities_;caps.profile=limits.resources;
    const auto overhead=capabilities_.profile.retained_working_bytes>limits_.resources.retained_working_bytes?
        capabilities_.profile.retained_working_bytes-limits_.resources.retained_working_bytes:0;
    if(overhead>SIZE_MAX-caps.profile.retained_working_bytes)return {Status::exhausted,{}};
    caps.profile.retained_working_bytes+=overhead;
    domain::ProcessingGraphAdmission admitted;
    auto diagnostic=domain::admit_processing_graph(graph,limits.admission,caps,workspace,admitted);
    if(!diagnostic)return {Status::invalid_argument,diagnostic};
    PreparedExecution candidate;
    const auto status=preparation_.prepare_numeric(graph,limits,candidate);
    if(status!=Status::ok)return {status,{}};
    const auto* numeric=candidate?candidate.plan->numeric_definition():nullptr;
    if(!numeric||!same_numeric_processing_graph(*numeric,graph))return {Status::invalid_argument,{}};
    if(!candidate.plan->memory_requirements().fits(limits.resources.max_peak_bytes))return {Status::exhausted,{}};
    pending_=std::move(candidate);return {};
#if defined(__cpp_exceptions)
    }catch(const std::bad_alloc&){return {Status::exhausted,{}};}
    catch(const std::length_error&){return {Status::exhausted,{}};}
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
