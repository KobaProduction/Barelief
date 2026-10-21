#include "runtime_internal.h"
#include "preparation_support.h"

namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
namespace {
std::optional<DataContract> find_port_contract(std::span<const ProcessingPort> ports,PortId port) noexcept {
    for(const auto& candidate:ports)if(candidate.id==port)return candidate.contract.view();
    return {};
}
std::optional<DataContract> source_port_contract(const NumericProcessingGraph& graph,NodeId id,
                                                 PortDirection direction,PortId port,bool reroute_only) noexcept {
    for(const auto& node:graph.nodes)if(node.id==id && (!reroute_only || node.kind=="Reroute"))
        return find_port_contract(direction==PortDirection::Input?node.inputs:node.outputs,port);
    return {};
}
bool admission_within(const ProcessingAdmissionLimits& requested,const ProcessingAdmissionLimits& maximum) noexcept {
    return requested.max_nodes<=maximum.max_nodes && requested.max_edges<=maximum.max_edges &&
        requested.max_ports_per_node<=maximum.max_ports_per_node &&
        requested.max_parameters_per_node<=maximum.max_parameters_per_node &&
        requested.max_text_bytes<=maximum.max_text_bytes &&
        requested.max_owned_metadata_bytes<=maximum.max_owned_metadata_bytes &&
        requested.frame.max_planes<=maximum.frame.max_planes &&
        requested.frame.max_elements<=maximum.frame.max_elements &&
        requested.frame.max_payload_bytes<=maximum.frame.max_payload_bytes &&
        requested.frame.max_metadata_bytes<=maximum.frame.max_metadata_bytes &&
        requested.frame.max_string_bytes<=maximum.frame.max_string_bytes &&
        requested.frame.max_coordinate_bytes<=maximum.frame.max_coordinate_bytes;
}
bool resources_within(const ProcessingResourceProfile& requested,const ProcessingResourceProfile& maximum) noexcept {
    return requested.maximum.plan_bytes<=maximum.maximum.plan_bytes &&
        requested.maximum.payload_bytes<=maximum.maximum.payload_bytes &&
        requested.maximum.scratch_bytes<=maximum.maximum.scratch_bytes &&
        requested.maximum.stack_bytes<=maximum.maximum.stack_bytes &&
        requested.max_peak_bytes<=maximum.max_peak_bytes;
}
Status retain_snapshot_allocation(NumericRuntimeProfile& profile,const PreparedGraph* plan,
                                  const RuntimeContext* context) noexcept {
    if(!plan || !context)return Status::ok;
    const auto memory=plan->memory_requirements();
    if(!numeric_add(profile.resources.retained_working_bytes,memory.state_bytes) ||
       !numeric_add(profile.resources.retained_working_bytes,memory.buffer_bytes) ||
       !numeric_add(profile.resources.retained_working_bytes,memory.scratch_bytes))return Status::overflow;
    return Status::ok;
}
}
PreparedGraph::PreparedGraph() : impl_(std::make_unique<Impl>()) {}
PreparedGraph::~PreparedGraph() = default;
application::MemoryRequirements PreparedGraph::memory_requirements() const noexcept {return impl_->memory;}
std::optional<DataContract> PreparedGraph::port_contract(NodeId id,PortDirection direction,std::uint16_t port) const noexcept {
    if(!impl_->numeric_graph)return {};
    const auto at=index(impl_->nodes,id);if(at==absent){
        return impl_->numeric_source_graph?
            source_port_contract(*impl_->numeric_source_graph,id,direction,port,true):std::nullopt;
    }
    const auto& node=impl_->numeric_graph->nodes[impl_->nodes[at].configured];
    if(const auto prepared=find_port_contract(direction==PortDirection::Input?node.inputs:node.outputs,port))return prepared;
    if(impl_->numeric_source_graph)
        return source_port_contract(*impl_->numeric_source_graph,id,direction,port,false);
    return {};
}
const NumericProcessingGraph* PreparedGraph::numeric_definition() const noexcept {return impl_->numeric_source_graph?&*impl_->numeric_source_graph:nullptr;}
std::shared_ptr<const NumericProcessingGraph> PreparedGraph::numeric_snapshot() const noexcept {return impl_->numeric_source_graph;}
std::optional<ProcessingGraphAdmission> PreparedGraph::numeric_admission() const noexcept {return impl_->numeric_admission;}
GraphExecutor::GraphExecutor(NumericRuntimeProfile profile):numeric_profile_(std::move(profile)) {}
Status GraphExecutor::prepare_numeric(const NumericProcessingGraph& graph,const application::NumericRuntimeLimits& limits,application::PreparedExecution& result) {
    PreparationBase base;base.owner=this;base.plan=plan_;base.borrowed_context=context_.get();
    const auto status=prepare_numeric_detached(graph,limits,base,result);
    return status==Status::ok?register_prepared(result,base):status;
}
GraphExecutor::PreparationBase GraphExecutor::snapshot_preparation_base() const {
    PreparationBase base;base.owner=this;base.plan=plan_;if(context_)base.context=std::make_unique<RuntimeContext>(*context_);return base;
}
Status GraphExecutor::register_prepared(const application::PreparedExecution& result,const PreparationBase& base) noexcept {
    if(base.owner!=this)return Status::invalid_argument;
    const auto candidate=base.prepared.lock();
    if(!result || !candidate || candidate.get()!=result.plan.get() || result.context.get()!=base.prepared_context)return Status::invalid_argument;
    if(plan_.get()!=base.plan.get())return Status::would_block;
    prepared_=candidate;
    prepared_context_=result.context.get();prepared_base_plan_=base.plan.get();return Status::ok;
}
static Status detached_profile(const NumericRuntimeProfile& configured,
    const application::NumericRuntimeLimits& limits,NumericRuntimeProfile& profile,
    const PreparedGraph* plan,const RuntimeContext* context) {
    if(!admission_within(limits.admission,configured.admission) ||
       !resources_within(limits.resources,configured.resources))return Status::exhausted;
    profile=configured;
    profile.admission=limits.admission;
    profile.resources=limits.resources;
    profile.preparation_values=limits.preparation_values;
    // Detached preparation retains both the live context and its owning snapshot.
    // Charge the snapshot in addition to the existing old-plan reservation.
    return retain_snapshot_allocation(profile,plan,context);
}
static Status prepare_detached_graph(const NumericProcessingGraph& graph,const NumericRuntimeProfile& profile,
    const PreparedGraph* previous,const RuntimeContext* previous_context,application::PreparedExecution& result,
    std::weak_ptr<const PreparedGraph>& prepared,const application::RuntimeContext*& prepared_context) {
    result={};std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> context;
    const auto status=PreparedGraph::prepare_numeric(graph,profile,plan,context,previous,previous_context);
    if(status!=Status::ok)return status;
    prepared=plan;prepared_context=context.get();
    result.plan=std::move(plan);result.context=std::move(context);return Status::ok;
}
Status GraphExecutor::prepare_numeric_detached(const NumericProcessingGraph& graph,const application::NumericRuntimeLimits& limits,
    const PreparationBase& base,application::PreparedExecution& result) const {
    if(base.owner!=this){result={};return Status::invalid_argument;}
    NumericRuntimeProfile profile;
    const auto profile_status=detached_profile(numeric_profile_,limits,profile,base.plan.get(),base.context.get());
    if(profile_status!=Status::ok){result={};return profile_status;}
    return prepare_detached_graph(graph,profile,base.plan.get(),base.context?base.context.get():base.borrowed_context,
        result,base.prepared,base.prepared_context);
}
Status GraphExecutor::process_numeric_ports(NodeId ingress,const DataFrameView& frame,DeliveryStamp stamp,NumericRuntimeCallbacks cb) noexcept {
    if(!running_ || !plan_)return Status::would_block;
    return plan_->process_numeric_ports(ingress,frame,*context_,stamp,cb);
}
Status GraphExecutor::process_numeric_owned_ports(NodeId ingress,const OwnedDataFrame& frame,DeliveryStamp stamp,NumericRuntimeCallbacks cb,
    std::optional<std::uint64_t> history_ordinal,std::shared_ptr<const void> retention_owner,std::optional<std::uint64_t> history_filled_count) noexcept {
    if(!running_ || !plan_)return Status::would_block;
    return plan_->process_numeric_owned_ports(ingress,frame,*context_,stamp,cb,history_ordinal,std::move(retention_owner),history_filled_count);
}
Status GraphExecutor::apply(application::PreparedExecution &&execution) {
    const auto expected = prepared_.lock();
    if (!execution || !expected || execution.plan.get() != expected.get() ||
        execution.context.get() != prepared_context_ || plan_.get() != prepared_base_plan_)
        return Status::invalid_argument;
    if(plan_ && !retained_streams_fit(*expected,*static_cast<const RuntimeContext*>(execution.context.get())))
        return Status::exhausted;
    // Identity is checked before casts, so no RTTI or foreign-plan downcast is
    // needed. The bundle came from this preparation port and remains untouched
    // on failure. Successful application consumes it at the owner's boundary.
    if (plan_)
        expected->refresh_state(*static_cast<RuntimeContext *>(execution.context.get()), *plan_, *context_);
    plan_ = std::static_pointer_cast<const PreparedGraph>(std::move(execution.plan));
    context_.reset(static_cast<RuntimeContext *>(execution.context.release()));
    prepared_.reset();
    prepared_context_ = nullptr;
    running_ = true;
    return Status::ok;
}
bool GraphExecutor::retained_streams_fit(const PreparedGraph& prepared,const RuntimeContext& destination) const noexcept {
    for(std::size_t slot=0;slot<prepared.impl_->nodes.size();++slot)if(!prepared.impl_->nodes[slot].reset) {
        const auto old=index(plan_->impl_->nodes,prepared.impl_->nodes[slot].node.id);
        if(old!=absent && context_->impl_->states[old].numeric_stamp.stream_id.size()>
           destination.impl_->states[slot].stream_storage.size())return false;
    }
    return true;
}
Status GraphExecutor::shutdown() {
    running_=false;prepared_.reset();prepared_context_=nullptr;prepared_base_plan_=nullptr;
    context_.reset();plan_.reset();return Status::ok;
}
Status GraphExecutor::process_numeric_ports(domain::NodeId ingress,domain::PortId port,const signal_contracts::DataFrameView& frame,DeliveryStamp stamp,NumericRuntimeCallbacks cb) noexcept {
    if(!running_ || !plan_ || !context_)return signal_contracts::Status::unavailable;
    return plan_->process_numeric_ports(ingress,port,frame,*context_,stamp,cb);
}
Status GraphExecutor::process_clock(domain::NodeId clock_node,const signal_contracts::FrameStamp& event,DeliveryStamp stamp,NumericRuntimeCallbacks callbacks) noexcept {
    if(!running_ || !plan_ || !context_)return signal_contracts::Status::unavailable;
    return plan_->process_clock(clock_node,event,*context_,stamp,callbacks);
}
std::optional<NodeStats> GraphExecutor::node_stats(domain::NodeId node) const noexcept {return plan_ && context_?plan_->node_stats(node,*context_):std::nullopt;}
std::optional<MixerInputStats> GraphExecutor::mixer_input_stats(domain::NodeId node,domain::PortId port) const noexcept {return plan_ && context_?plan_->mixer_input_stats(node,port,*context_):std::nullopt;}
Status GraphExecutor::advance_unavailable_source(domain::NodeId node,const signal_contracts::FrameStamp& event,DeliveryStamp delivery,NumericRuntimeCallbacks callbacks,domain::PortId port) noexcept {
    if(!running_ || !plan_ || !context_)return signal_contracts::Status::unavailable;
    return plan_->advance_unavailable_source(node,event,*context_,delivery,callbacks,port);
}

} // namespace dataflow_runtime
