#include <application/graph_lifecycle.h>
#include <algorithm>
#include <utility>
namespace application {
bool MemoryRequirements::fits(std::size_t budget) const noexcept {
    if (state_bytes>budget) return false;
    budget-=state_bytes;
    if (buffer_bytes>budget) return false;
    return scratch_bytes<=budget-buffer_bytes;
}
GraphLifecycle::GraphLifecycle(GraphPreparationPort& preparation, GraphRuntimePort& runtime,
                               domain::DeviceCapabilities capabilities, RuntimeLimits limits)
    : preparation_(preparation),runtime_(runtime),capabilities_(std::move(capabilities)),limits_(limits) {}
LifecycleResult GraphLifecycle::prepare(const domain::GraphDefinition& graph) {
    pending_={}; // An invalid new draft must not leave an older draft applicable.
    auto validation=domain::validate_graph(graph,capabilities_,limits_.graph);
    if (!validation) return {Status::invalid_argument,std::move(validation)};
    for (const auto& n:graph.nodes) {
        if (const auto* p=std::get_if<domain::WindowParameters>(&n.parameters))
            if (p->size>limits_.max_frame_samples) return {Status::exhausted,{}};
        if (const auto* p=std::get_if<domain::HistoryParameters>(&n.parameters))
            if (p->history_size>limits_.max_history_frames) return {Status::exhausted,{}};
    }
    PreparedExecution candidate;
    const auto status=preparation_.prepare(graph,validation.topological_order,limits_,candidate);
    if (status!=Status::ok) return {status,{}};
    if (!candidate || candidate.plan->definition()!=graph) return {Status::invalid_argument,{}};
    if (!candidate.plan->memory_requirements().fits(limits_.max_memory_bytes)) return {Status::exhausted,{}};
    pending_=std::move(candidate);
    return {Status::ok,std::move(validation)};
}
LifecycleResult GraphLifecycle::apply() {
    if (!pending_) return {Status::invalid_argument,{}};
    const auto plan=pending_.plan;
    const auto status=runtime_.apply(std::move(pending_),run_requested_);
    pending_={};
    if (status!=Status::ok) return {status,{}};
    working_=plan;
    state_=run_requested_ ? LifecycleState::Running : LifecycleState::Stopped;
    return {};
}
LifecycleResult GraphLifecycle::connect(const domain::Edge& edge) {
    if (!working_) return {Status::invalid_argument,{}};
    auto graph=working_->definition();
    // The caller must explicitly disconnect an occupied input first.
    graph.edges.push_back(edge);
    auto result=prepare(graph);
    return result ? apply() : result;
}
LifecycleResult GraphLifecycle::disconnect(domain::NodeId to,std::uint16_t input_port) {
    if (!working_ || input_port!=0) return {Status::invalid_argument,{}};
    auto graph=working_->definition();
    if (std::none_of(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==to && domain::input_type(n).has_value();}))
        return {Status::invalid_argument,{}};
    const auto removed=std::erase_if(graph.edges,[&](const auto& e){return e.to==to && e.input_port==input_port;});
    if (!removed) return {};
    auto result=prepare(graph);
    return result ? apply() : result;
}
Status GraphLifecycle::start() {
    if (!working_) return Status::invalid_argument;
    run_requested_=true;
    const auto status=runtime_.start();
    if (status==Status::ok) state_=LifecycleState::Running;
    return status;
}
Status GraphLifecycle::stop() {
    // Intent changes even if the adapter fails, fencing future Apply/reconnect.
    run_requested_=false;
    const auto status=runtime_.stop();
    if (status==Status::ok) state_=LifecycleState::Stopped;
    return status;
}
const domain::GraphDefinition* GraphLifecycle::working_graph() const noexcept {
    return working_ ? &working_->definition() : nullptr;
}
}
