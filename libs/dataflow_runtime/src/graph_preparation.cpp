#include "numeric_preparation.h"
#include "preparation_support.h"
#include <algorithm>
#include <stdexcept>
#include <new>

namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
Status NumericPreparation::build(std::shared_ptr<const PreparedGraph>& result,std::unique_ptr<RuntimeContext>& context){
#if defined(__cpp_exceptions)
    try {
#endif
    {const auto status=prepare_definition();if(status!=Status::ok)return status;}
    {const auto status=prepare_execution();if(status!=Status::ok)return status;}
    c->impl_->plan=plan.get();if(previous && old_context)plan->refresh_state(*c,*previous,*old_context);
    result=std::move(plan);context=std::move(c);return Status::ok;
#if defined(__cpp_exceptions)
    }catch(const std::bad_alloc&){return Status::exhausted;}
    catch(const std::length_error&){return Status::exhausted;}
#endif
}
Status NumericPreparation::prepare_definition(){
    auto status=bind_definition();if(status!=Status::ok)return status;
    status=prepare_lowered_definition();if(status!=Status::ok)return status;
    status=prepare_resource_profile();if(status!=Status::ok)return status;
    status=admit_definition();if(status!=Status::ok)return status;
    return allocate_bundle();
}
Status NumericPreparation::prepare_lowered_definition(){
    const auto status=lower_definition();if(status!=Status::ok)return status;
    append_parameter_aliases();
    return Status::ok;
}
Status NumericPreparation::prepare_execution(){
    {const auto status=prepare_nodes();if(status!=Status::ok)return status;}
    reconcile_clocks();
    {const auto status=reconcile_history();if(status!=Status::ok)return status;}
    reserve_mixer_destinations();
    bind_mixer_destinations();
    {const auto status=check_clock_cycles();if(status!=Status::ok)return status;}
    bind_destinations();
    return Status::ok;
}
Status NumericPreparation::bind_definition(){
    if(!valid_execution_interval(original.execution_interval))return Status::invalid_argument;
    std::size_t preflight=0;const auto footprint=processing_graph_owned_bytes(original,profile.admission,preflight);
    if(footprint!=Status::ok)return footprint;
    const auto values=resolve_numeric_parameter_bindings(original,profile.admission,evaluated,values_lowered,-1,&profile.preparation_values);
    if(!values)return values.error==ProcessingError::Limit?Status::exhausted:Status::invalid_argument;
    return Status::ok;
}
Status NumericPreparation::lower_definition(){
    const auto metadata=lower_numeric_metadata_outputs(values_lowered,profile.admission,metadata_lowered,metadata_aliases,&profile.preparation_values);
    if(!metadata)return metadata.error==ProcessingError::Limit?Status::exhausted:Status::invalid_argument;
    const auto lowering=lower_dtype_edges(metadata_lowered,profile.admission,converted);
    if(!lowering)return lowering.error==ProcessingError::Limit?Status::exhausted:Status::invalid_argument;
    const auto routing=lower_routing_points(converted,profile.admission,lowered,aliases);
    if(!routing)return routing.error==ProcessingError::Limit?Status::exhausted:Status::invalid_argument;
    aliases.insert(aliases.end(),metadata_aliases.begin(),metadata_aliases.end());
    return Status::ok;
}
void NumericPreparation::append_parameter_aliases(){
    for(const auto& edge:evaluated.edges)append_parameter_alias(edge);
}
namespace {
void resolve_parameter_alias_source(std::span<const RoutingAlias> aliases,const Edge& edge,RoutingAlias& value) noexcept {
    for(const auto& alias:aliases)if(alias.node==edge.from && alias.port==edge.output_port) {
        value.source=alias.source;value.source_port=alias.source_port;return;
    }
}
}
void NumericPreparation::append_parameter_alias(const Edge& edge){
        const auto target=std::find_if(evaluated.nodes.begin(),evaluated.nodes.end(),[&](const auto& n){return n.id==edge.to;});
        if(target==evaluated.nodes.end() || numeric_parameter_key(target->kind,edge.input_port).empty())return;
        RoutingAlias value_alias;value_alias.source=edge.from;value_alias.source_port=edge.output_port;
        value_alias.node=edge.to;value_alias.port=edge.input_port;value_alias.direction=PortDirection::Input;value_alias.include_input=false;
        value_alias.kind=*node_kind(target->kind);
        resolve_parameter_alias_source(aliases,edge,value_alias);
        aliases.push_back(value_alias);
}
Status NumericPreparation::prepare_resource_profile(){
    if((previous && (!old_context || !old_context->impl_ || old_context->impl_->plan!=previous)) || (!previous && old_context))return Status::invalid_argument;
    if(g.nodes.size()>profile.admission.max_nodes || g.nodes.size()>SIZE_MAX/2)return Status::exhausted;
    order.resize(g.nodes.size()*2);
    {const auto status=prepare_source_ownership();if(status!=Status::ok)return status;}
    alias_bytes=aliases.size()*sizeof(RoutingAlias);
    if(!numeric_add(checked_profile.resources.retained_working_bytes,alias_bytes))return Status::overflow;
    {const auto status=prepare_content_resources();if(status!=Status::ok)return status;}
    return retain_previous_resources();
}
Status NumericPreparation::prepare_source_ownership(){
    const bool parameter_ports=std::any_of(evaluated.nodes.begin(),evaluated.nodes.end(),[](const auto& n){return std::any_of(n.inputs.begin(),n.inputs.end(),[&](const auto& p){return !numeric_parameter_key(n.kind,p.id).empty();});});
    if(g.nodes.size()!=original.nodes.size() || !aliases.empty() || parameter_ports){
        const auto footprint=processing_graph_owned_bytes(evaluated,profile.admission,source_bytes);if(footprint!=Status::ok)return footprint;
        if(!numeric_add(checked_profile.resources.retained_working_bytes,source_bytes))return Status::overflow;
    }
    return Status::ok;
}
Status NumericPreparation::prepare_content_resources(){
    {const auto status=count_content_outputs();if(status!=Status::ok)return status;}
    content_capacity=content_outputs;
    if(previous){
        retained_content_budget=previous->impl_->content_owned_bytes;
        if(!numeric_add(content_capacity,previous->impl_->content_token_count))return Status::overflow;
    }
    std::size_t entries=content_capacity,workspace=0;
    if(!numeric_add(entries,content_outputs) || !numeric_mul(entries,sizeof(const ContentLineage*),workspace))return Status::overflow;
    if(!numeric_add(checked_profile.resources.retained_working_bytes,workspace) ||
       !numeric_add(checked_profile.resources.retained_working_bytes,retained_content_budget))return Status::overflow;
    return Status::ok;
}
Status NumericPreparation::count_content_outputs(){
    for(const auto& node:g.nodes)
        if(!numeric_add(content_outputs,node.outputs.size()))return Status::overflow;
    return Status::ok;
}
Status NumericPreparation::retain_previous_resources(){
    if(previous && old_context){const auto mem=previous->memory_requirements();std::size_t old=mem.state_bytes;
        if(!numeric_add(old,mem.buffer_bytes) || !numeric_add(old,mem.scratch_bytes))return Status::overflow;
        if(previous->impl_->silence && previous->impl_->silence->size()==profile.admission.frame.max_payload_bytes)old-=previous->impl_->silence->size();
        if(!numeric_add(checked_profile.resources.retained_working_bytes,old))return Status::overflow;}
    return Status::ok;
}
Status NumericPreparation::admit_definition(){
    const auto check=admit_processing_graph(g,profile.admission,PreparedGraph::numeric_capabilities(checked_profile),order,admission);
    if(!check)return check.error==ProcessingError::ResourceLimit || check.error==ProcessingError::Limit?Status::exhausted:Status::invalid_argument;
    if(!numeric_add(admission.resources.plan_bytes,retained_content_budget))return Status::overflow;
    if(admission.resources.plan_bytes>profile.resources.maximum.plan_bytes)return Status::exhausted;
    // Domain peak already includes DTO ownership and topology workspace. Publish
    // retained plan ownership too, so a later Prepare cannot omit the old DTO.
    metadata_and_plan=admission.peak_bytes-checked_profile.resources.retained_working_bytes-order.size()*sizeof(std::size_t)-
        admission.resources.payload_bytes-admission.resources.scratch_bytes-admission.resources.stack_bytes-profile.admission.frame.max_payload_bytes;
    return Status::ok;
}
Status NumericPreparation::allocate_bundle(){
    plan=std::shared_ptr<PreparedGraph>(new PreparedGraph);c=std::make_unique<RuntimeContext>();
    plan->impl_->numeric_graph=std::make_shared<const NumericProcessingGraph>(g);plan->impl_->numeric_profile=profile;
    plan->impl_->numeric_source_graph=source_bytes?std::make_shared<const NumericProcessingGraph>(evaluated):plan->impl_->numeric_graph;
    plan->impl_->routing_aliases=std::move(aliases);
    plan->impl_->silence=previous && previous->impl_->silence && previous->impl_->silence->size()==profile.admission.frame.max_payload_bytes?
        previous->impl_->silence:std::make_shared<const std::vector<std::byte>>(profile.admission.frame.max_payload_bytes);
    plan->impl_->memory={metadata_and_plan+source_bytes+alias_bytes+retained_content_budget,admission.resources.payload_bytes+profile.admission.frame.max_payload_bytes,admission.resources.scratch_bytes};
    plan->impl_->numeric_admission=admission;
    plan->impl_->nodes.reserve(g.nodes.size());c->impl_->states.resize(g.nodes.size());
    return Status::ok;
}
Status PreparedGraph::prepare_numeric(const NumericProcessingGraph& original,const NumericRuntimeProfile& profile,
    std::shared_ptr<const PreparedGraph>& result,std::unique_ptr<RuntimeContext>& context,const PreparedGraph* previous,const RuntimeContext* old_context){
    return NumericPreparation(original,profile,previous,old_context).build(result,context);
}

} // namespace dataflow_runtime
