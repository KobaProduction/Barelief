#include <application/processing_control.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace application {
namespace {
struct ParameterBudget {
    const domain::ProcessingAdmissionLimits& limits;
    std::size_t text_limit,owned=sizeof(domain::ProcessingParameters);
    bool add(std::size_t count,std::size_t width=1) noexcept {
        if(width && count>(SIZE_MAX-owned)/width)return false;
        owned+=count*width;return owned<=limits.max_owned_metadata_bytes;
    }
    bool field(const domain::ProcessingParameter& field) noexcept {
        if(field.key.size()>text_limit || !add(field.key.size()) || !add(1))return false;
        if(const auto* value=std::get_if<std::string>(&field.value))
            return value->size()<=text_limit && add(value->size()) && add(1);
        if(const auto* value=std::get_if<domain::NumericArray>(&field.value)) {
            return value->values.size()<=domain::max_numeric_array_values && add(value->values.size(),sizeof(float)) &&
                std::all_of(value->values.begin(),value->values.end(),[](float item){return std::isfinite(item);});
        }
        return true;
    }
    bool fields(std::span<const domain::ProcessingParameter> values) noexcept {
        if(values.size()>limits.max_parameters_per_node || !add(values.size(),sizeof(domain::ProcessingParameter)))return false;
        return std::all_of(values.begin(),values.end(),[&](const auto& value){return field(value);});
    }
    template<class T> bool operator()(const T&) noexcept{return owned<=limits.max_owned_metadata_bytes;}
    bool operator()(const domain::EndpointProcessingParameters& value) noexcept{return fields(value.values);}
    bool operator()(const domain::TransformProcessingParameters& value) noexcept{return fields(value.values);}
    bool operator()(const domain::SignalGeneratorProcessingParameters& value) noexcept {
        return !value.oscillators.empty() && value.oscillators.size()<=signal_contracts::max_oscillators &&
            add(value.oscillators.size(),sizeof(signal_contracts::OscillatorParameters));
    }
    bool operator()(const domain::SampleClockProcessingParameters& value) noexcept {
        return !value.domain.empty() && value.domain.size()<=text_limit && add(value.domain.size()) && add(1);
    }
    bool operator()(const domain::MixerProcessingParameters& value) noexcept {
        return value.weights.size()<=limits.max_ports_per_node && add(value.weights.size(),sizeof(domain::MixerInputWeight));
    }
    bool operator()(const domain::ReduceProcessingParameters& value) noexcept {
        return value.selected_channels.size()<=limits.frame.max_planes && add(value.selected_channels.size(),sizeof(std::uint32_t));
    }
};
bool numeric_parameters_bounded(const domain::ProcessingParameters& parameters,const ProcessingCapabilities& caps) noexcept {
    if(!caps.numeric || parameters.valueless_by_exception())return false;
    const auto& limits=caps.numeric->runtime.admission;
    ParameterBudget budget{limits,std::min(limits.max_text_bytes,caps.control.max_field_string_bytes)};
    return std::visit([&](const auto& value){return budget(value);},parameters);
}
struct GraphBudget {
    const ProcessingCapabilities& caps;
    const domain::ProcessingAdmissionLimits& limits;
    std::size_t metadata=0;
    bool ports(std::span<const domain::ProcessingPort> ports) noexcept {
        for(const auto& port:ports) {
            const auto bytes=port.contract.owned_bytes();
            if(bytes>limits.max_owned_metadata_bytes-metadata)return false;
            metadata+=bytes;
        }
        return true;
    }
    bool node(const domain::ConfiguredProcessingNode& node) noexcept {
        return node.kind.size()<=limits.max_text_bytes && node.kind.size()<=caps.control.max_field_string_bytes &&
            node.inputs.size()<=limits.max_ports_per_node && node.outputs.size()<=limits.max_ports_per_node &&
            numeric_parameters_bounded(node.parameters,caps) && ports(node.inputs) && ports(node.outputs);
    }
};
bool numeric_graph_bounded(const domain::NumericProcessingGraph& graph,const ProcessingCapabilities& caps) noexcept {
    if(!caps.numeric)return false;
    const auto& limits=caps.numeric->runtime.admission;
    if(graph.graph_id.size()>caps.control.max_graph_identity_bytes || graph.graph_id.size()>limits.max_text_bytes ||
       graph.nodes.size()>limits.max_nodes || graph.edges.size()>limits.max_edges)return false;
    GraphBudget budget{caps,limits};
    if(!std::all_of(graph.nodes.begin(),graph.nodes.end(),[&](const auto& node){return budget.node(node);}))return false;
    std::size_t owned=0;return domain::processing_graph_owned_bytes(graph,limits,owned)==Status::ok;
}
struct CommandEquality {
    const ProcessingCommand& other;
    template<class T> bool operator()(const T&) const noexcept{return true;}
    bool operator()(const PrepareProcessing& value) const noexcept {
        const auto& expected=std::get<PrepareProcessing>(other);
        return value.processing && expected.processing && value.processing->numeric && expected.processing->numeric &&
            same_numeric_processing_graph(*value.processing->numeric,*expected.processing->numeric);
    }
    bool operator()(const ApplyProcessing& value) const noexcept {
        const auto& expected=std::get<ApplyProcessing>(other);
        return value.token==expected.token && value.graph_identity==expected.graph_identity;
    }
    bool operator()(const ReleasePrepared& value) const noexcept{return value.token==std::get<ReleasePrepared>(other).token;}
    bool operator()(const SetNumericProcessingParameters& value) const noexcept {
        const auto& expected=std::get<SetNumericProcessingParameters>(other);
        return value.node==expected.node && value.enabled==expected.enabled && domain::same_processing_parameters(value.parameters,expected.parameters);
    }
    bool operator()(const ExplicitPersist& value) const noexcept{return value.graph_revision==std::get<ExplicitPersist>(other).graph_revision;}
};
bool same_command(const ProcessingCommand& a,const ProcessingCommand& b) noexcept {
    if(a.valueless_by_exception() || b.valueless_by_exception() || a.index()!=b.index())return false;
    return std::visit(CommandEquality{b},a);
}
ControlError diagnostic_error(domain::ProcessingError error) noexcept {
    using E=domain::ProcessingError;
    switch(error) {
    case E::UnsupportedVersion:case E::UnknownKind:case E::UnsupportedCombination:return ControlError::unsupported;
    case E::Limit:case E::ResourceLimit:case E::InsufficientWorkspace:return ControlError::limit_exceeded;
    default:return ControlError::invalid_graph;
    }
}
ControlError lifecycle_error(Status status) noexcept {
    switch(status) {
    case Status::ok:return ControlError::none;
    case Status::exhausted:case Status::buffer_too_small:return ControlError::limit_exceeded;
    case Status::unsupported_version:return ControlError::unsupported;
    case Status::invalid_argument:case Status::incompatible_contract:return ControlError::invalid_graph;
    default:return ControlError::resource_failure;
    }
}
bool control_payload_budget_fits(const ProcessingCapabilities& caps,std::size_t budget) noexcept {
    const auto required=control_snapshot_bytes(caps);return required && *required<=budget;
}
bool replay_storage_valid(const ProcessingCapabilities& caps,std::size_t slots) noexcept {
    const auto& limits=caps.control;
    return limits.max_sessions<=slots && control_payload_budget_fits(caps,limits.max_replay_bytes_per_session) &&
        control_payload_budget_fits(caps,limits.max_control_scratch_bytes) && limits.max_replay_bytes_per_session &&
        limits.max_sessions<=limits.max_replay_bytes_total/limits.max_replay_bytes_per_session;
}
bool session_limits_valid(const ControlLimits& limits,std::uint64_t epoch,std::size_t identity_capacity) noexcept {
    return epoch && limits.max_pending_prepared==1 && limits.prepared_ttl_ms && limits.max_sessions &&
        limits.max_graph_identity_bytes && limits.max_graph_identity_bytes<=identity_capacity;
}
bool runtime_profile_valid(const ProcessingCapabilities& caps) noexcept {
    return caps.numeric && caps.numeric->device.query &&
        caps.control.max_prepared_bytes>=caps.numeric->runtime.resources.max_peak_bytes;
}
bool persistence_configured(const ProcessingCapabilities& caps,const ProcessingPersistencePort* persistence) noexcept {
    return !caps.explicit_persist || persistence;
}
bool edit_processing_node(domain::NumericProcessingGraph& graph,const SetNumericProcessingParameters& command) {
    const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& node){return node.id==command.node;});
    if(node==graph.nodes.end())return false;
    node->parameters=command.parameters;node->enabled=command.enabled;return true;
}
OpenControlSessionResult open_available_session(std::span<ControlSessionSlot> sessions,
    std::uint64_t client,std::uint64_t& next_session,std::uint64_t epoch) noexcept {
    for(auto& slot:sessions)if(!slot.identity.session){
        slot.identity={client,next_session++,epoch};
        return {ControlError::none,slot.identity};
    }
    return {ControlError::limit_exceeded,{}};
}
bool preparation_deadline_available(std::uint64_t now,std::uint64_t next_token,
    std::uint64_t ttl) noexcept {
    return next_token!=std::numeric_limits<std::uint64_t>::max() &&
        ttl<=std::numeric_limits<std::uint64_t>::max()-now;
}
}
std::optional<std::size_t> control_snapshot_bytes(const ProcessingCapabilities& caps) noexcept {
    if(!caps.numeric)return {};
    std::size_t bytes=sizeof(ControlSessionSlot);
    auto add=[&](std::size_t count,std::size_t width){if(width && count>(SIZE_MAX-bytes)/width)return false;bytes+=count*width;return true;};
    const auto& n=caps.numeric->runtime.admission;const auto text=std::min(n.max_text_bytes,caps.control.max_field_string_bytes);
    if(text==SIZE_MAX || caps.control.max_graph_identity_bytes==SIZE_MAX)return {};
    // Full DTO cap includes nodes, ports, parameters, strings and contracts.
    // Request and independent reply owners may coexist; no kernels are retained.
    if(!add(2,n.max_owned_metadata_bytes) || !add(n.max_nodes,2*sizeof(std::size_t)) || !add(caps.control.max_graph_identity_bytes+1,1))return {};
    // Standalone SetParameters now shares the same aggregate owning metadata
    // bound as a graph. The two complete owner ceilings above already cover
    // its request and an independent applied-graph reply; field maxima are not
    // multiplied as though every parameter could simultaneously consume them.
    return bytes;
}
LifecycleResult GraphLifecycleControlPort::prepare_numeric_transactional(const domain::NumericProcessingGraph& g,const NumericRuntimeLimits& limits) {
    return lifecycle_.prepare_numeric_transactional(g,limits);
}
LifecycleResult GraphLifecycleControlPort::apply_transactional(){return lifecycle_.apply_transactional();}
void GraphLifecycleControlPort::discard_prepared() noexcept{lifecycle_.discard_prepared();}


MemoryRequirements GraphLifecycleControlPort::pending_memory_requirements() const noexcept{return lifecycle_.pending_memory_requirements();}
LifecycleState GraphLifecycleControlPort::state() const noexcept{return lifecycle_.state();}

const domain::NumericProcessingGraph* GraphLifecycleControlPort::working_numeric_graph() const noexcept{return lifecycle_.working_numeric_graph();}
std::shared_ptr<const domain::NumericProcessingGraph> GraphLifecycleControlPort::working_numeric_snapshot() const noexcept{return lifecycle_.working_numeric_snapshot();}
std::optional<domain::ProcessingGraphAdmission> GraphLifecycleControlPort::pending_numeric_admission() const noexcept{return lifecycle_.pending_numeric_admission();}
std::optional<domain::ProcessingGraphAdmission> GraphLifecycleControlPort::working_numeric_admission() const noexcept{return lifecycle_.working_numeric_admission();}

ProcessingControlService::ProcessingControlService(ProcessingControlRuntimePort& runtime,ControlClock& clock,
    const ProcessingCapabilities& caps,std::span<ControlSessionSlot> slots,std::uint64_t epoch,ProcessingPersistencePort* persistence) noexcept
    :runtime_(runtime),clock_(clock),caps_(caps),sessions_(slots),persistence_(persistence),epoch_(epoch) {
    const auto& l=caps.control;
    const bool storage_valid=replay_storage_valid(caps,slots.size());
    valid_config_=session_limits_valid(l,epoch,graph_identity_.size()) && storage_valid &&
        runtime_profile_valid(caps) && persistence_configured(caps,persistence);
    if(valid_config_){sessions_=slots.first(l.max_sessions);for(auto& s:sessions_)s=ControlSessionSlot{};}
    else if(!storage_valid)
        config_error_=ControlError::limit_exceeded;
}
ProcessingControlService::~ProcessingControlService(){
    if(token_)release();
    if(valid_config_)for(auto& slot:sessions_)slot=ControlSessionSlot{};
}
ControlSessionSlot* ProcessingControlService::session(SessionIdentity id) noexcept {
    if(!valid_config_||!id.client||!id.session||id.epoch!=epoch_)return nullptr;
    for(auto& slot:sessions_)if(slot.identity==id)return &slot;
    return nullptr;
}
OpenControlSessionResult ProcessingControlService::open_session(std::uint64_t client) noexcept {
    expire_prepared();if(!valid_config_)return {config_error_,{}};
    if(!client)return {ControlError::malformed,{}};
    for(const auto& slot:sessions_)if(slot.identity.client==client)return {ControlError::busy,{}};
    if(!next_session_||next_session_==std::numeric_limits<std::uint64_t>::max())return {ControlError::limit_exceeded,{}};
    return open_available_session(sessions_,client,next_session_,epoch_);
}
ControlError ProcessingControlService::close_session(SessionIdentity id) noexcept {
    expire_prepared();auto* slot=session(id);if(!slot)return ControlError::session_closed;
    if(owner_==id)release();
    *slot=ControlSessionSlot{};return ControlError::none;
}
void ProcessingControlService::release() noexcept {
    runtime_.discard_prepared();owner_={};token_=0;base_revision_=0;expires_at_=0;graph_identity_size_=0;
}
bool ProcessingControlService::expire_prepared() noexcept {
    if(token_&&clock_.now_ms()>=expires_at_){release();return true;}return false;
}
ControlState ProcessingControlService::snapshot(SessionIdentity id) const noexcept {
    return {epoch_,revision_,graph_revision_,persisted_revision_,runtime_.state(),runtime_.state()==LifecycleState::Running,
        owner_==id?token_:0,runtime_.working_numeric_graph()};
}
ControlState ProcessingControlService::observe_state() noexcept {
    expire_prepared();
    return snapshot({});
}
ProcessingReply ProcessingControlService::result(SessionIdentity id,CommandHeader header,ControlError error,Status status) const {
    ProcessingReply reply;reply.request_id=header.request_id;reply.accepted=error==ControlError::none;
    reply.error=error;reply.status=status;reply.state=snapshot(id);reply.numeric_resources=runtime_.working_numeric_admission();
    return reply;
}
ProcessingReply ProcessingControlService::lifecycle_result(SessionIdentity id,CommandHeader header,const LifecycleResult& lifecycle) const {
    const auto error=lifecycle.numeric_validation?lifecycle_error(lifecycle.status):diagnostic_error(lifecycle.numeric_validation.error);
    auto reply=result(id,header,error,lifecycle.status);reply.numeric_validation=lifecycle.numeric_validation;return reply;
}
bool ProcessingControlService::bounded_command(const ProcessingCommand& command) const noexcept {
    if(command.valueless_by_exception())return false;
    if(const auto* p=std::get_if<PrepareProcessing>(&command))return p->processing&&p->processing->numeric&&numeric_graph_bounded(*p->processing->numeric,caps_);
    if(const auto* p=std::get_if<ApplyProcessing>(&command))return p->graph_identity.size()<=caps_.control.max_graph_identity_bytes;
    if(const auto* p=std::get_if<SetNumericProcessingParameters>(&command))return numeric_parameters_bounded(p->parameters,caps_);
    return true;
}
void ProcessingControlService::stage_request(ControlSessionSlot& slot,CommandHeader header,const ProcessingCommand& command){
    slot.completed=false;slot.last_header=header;slot.last_command=command;
    if(const auto* p=std::get_if<PrepareProcessing>(&command)){
        slot.request_processing=*p->processing;slot.last_command=PrepareProcessing{&*slot.request_processing};
    }else slot.request_processing.reset();
    if(const auto* p=std::get_if<ApplyProcessing>(&command)){
        slot.request_graph_identity=p->graph_identity;slot.last_command=ApplyProcessing{p->token,slot.request_graph_identity};
    }else slot.request_graph_identity.clear();
}
void ProcessingControlService::remember(ControlSessionSlot& slot,ControlSessionSlot&& staged,const ProcessingReply& reply) noexcept {
    staged.last_reply=reply;staged.reply_numeric_graph=runtime_.working_numeric_snapshot();
    staged.last_reply.state.applied_numeric=staged.reply_numeric_graph.get();staged.completed=true;
    slot=std::move(staged);
    if(std::holds_alternative<PrepareProcessing>(slot.last_command))slot.last_command=PrepareProcessing{&*slot.request_processing};
    else if(auto* apply=std::get_if<ApplyProcessing>(&slot.last_command))apply->graph_identity=slot.request_graph_identity;
}
void ProcessingControlService::install_prepared(SessionIdentity id,std::string_view identity,std::uint64_t now) {
    owner_=id;token_=next_token_++;base_revision_=revision_;expires_at_=now+caps_.control.prepared_ttl_ms;
    graph_identity_size_=identity.size();std::copy(identity.begin(),identity.end(),graph_identity_.begin());
}
ProcessingReply ProcessingControlService::prepared_reply(SessionIdentity id,CommandHeader header) const {
    auto reply=result(id,header);reply.prepared_token=token_;reply.expires_at_ms=expires_at_;
    reply.memory=runtime_.pending_memory_requirements();reply.numeric_resources=runtime_.pending_numeric_admission();return reply;
}
ProcessingReply ProcessingControlService::prepare(SessionIdentity id,CommandHeader header,const ProcessingDTO& dto) {
    if(token_&&owner_!=id)return result(id,header,ControlError::busy,Status::would_block);
    if(!dto.numeric)return result(id,header,ControlError::malformed,Status::invalid_argument);
    const auto now=clock_.now_ms();
    if(!preparation_deadline_available(now,next_token_,caps_.control.prepared_ttl_ms))
        return result(id,header,ControlError::limit_exceeded,Status::overflow);
    return prepare_transaction(id,header,dto,now);
}
ProcessingReply ProcessingControlService::prepare_transaction(SessionIdentity id,CommandHeader header,
    const ProcessingDTO& dto,std::uint64_t now) {
    const bool had_token=token_!=0;owner_=id;
    const auto prepared=runtime_.prepare_numeric_transactional(*dto.numeric,caps_.numeric->runtime);
    if(!prepared){if(!had_token)owner_={};return lifecycle_result(id,header,prepared);}
    install_prepared(id,dto.numeric->graph_id,now);return prepared_reply(id,header);
}
bool ProcessingControlService::command_busy(SessionIdentity id,const ProcessingCommand& command) const noexcept {
    return (std::holds_alternative<PrepareProcessing>(command) && token_ && owner_!=id) ||
        (std::holds_alternative<SetNumericProcessingParameters>(command) && token_);
}
ProcessingReply ProcessingControlService::replay_reply(SessionIdentity id,CommandHeader header,const ProcessingCommand& command,
                                                      const ControlSessionSlot& slot) const {
    if(slot.last_header.expected_revision!=header.expected_revision || !bounded_command(command) || !same_command(slot.last_command,command))
        return result(id,header,ControlError::conflict,Status::invalid_argument);
    return slot.last_reply;
}
ProcessingReply ProcessingControlService::unbounded_reply(SessionIdentity id,CommandHeader header,const ProcessingCommand& command) const {
    if(command_busy(id,command))return result(id,header,ControlError::busy,Status::would_block);
    if(const auto* prepare=std::get_if<PrepareProcessing>(&command);prepare && (!prepare->processing || !prepare->processing->numeric))
        return result(id,header,ControlError::malformed,Status::invalid_argument);
    return result(id,header,ControlError::limit_exceeded,Status::exhausted);
}
std::optional<ProcessingReply> ProcessingControlService::submission_preflight(SessionIdentity id,CommandHeader header,
                                const ProcessingCommand& command,const ControlSessionSlot& slot) const {
    if(slot.completed && slot.last_header.request_id==header.request_id)return replay_reply(id,header,command,slot);
    if(!bounded_command(command))return unbounded_reply(id,header,command);
    if(const auto* working=runtime_.working_numeric_graph();working && !numeric_graph_bounded(*working,caps_))
        return result(id,header,ControlError::limit_exceeded,Status::exhausted);
    return {};
}
ProcessingReply ProcessingControlService::submit_staged(SessionIdentity id,CommandHeader header,const ProcessingCommand& command,
                                                        ControlSessionSlot& slot,std::uint64_t expired,SessionIdentity expired_owner) {
    ControlSessionSlot staged;staged.identity=slot.identity;stage_request(staged,header,command);
    const auto reply=dispatch(id,header,command,expired,expired_owner);
    remember(slot,std::move(staged),reply);return reply;
}
ProcessingReply ProcessingControlService::submit(SessionIdentity id,CommandHeader header,const ProcessingCommand& command) {
    const auto prior_token=token_;const auto prior_owner=owner_;const auto expired=expire_prepared()?prior_token:0;
    auto* slot=session(id);if(!slot)return result(id,header,ControlError::session_closed,Status::disconnected);
    if(!header.request_id)return result(id,header,ControlError::malformed,Status::invalid_argument);
    const auto refused=submission_preflight(id,header,command,*slot);if(refused)return *refused;
#if defined(__cpp_exceptions)
    try {return submit_staged(id,header,command,*slot,expired,prior_owner);}
    catch(const std::bad_alloc&){return result(id,header,ControlError::resource_failure,Status::exhausted);}
    catch(const std::length_error&){return result(id,header,ControlError::limit_exceeded,Status::exhausted);}
#else
    return submit_staged(id,header,command,*slot,expired,prior_owner);
#endif
}
std::optional<ProcessingReply> ProcessingControlService::mutation_gate(SessionIdentity id,CommandHeader header,
                                const ProcessingCommand& command,std::uint64_t expired,SessionIdentity expired_owner) const {
    if(const auto* apply=std::get_if<ApplyProcessing>(&command);apply && expired && expired_owner==id && expired==apply->token)
        return result(id,header,ControlError::preparation_expired,Status::unavailable);
    if(command_busy(id,command))return result(id,header,ControlError::busy,Status::would_block);
    if(header.expected_revision!=revision_)return result(id,header,ControlError::conflict,Status::invalid_argument);
    if(revision_==std::numeric_limits<std::uint64_t>::max())return result(id,header,ControlError::limit_exceeded,Status::overflow);
    return {};
}
void ProcessingControlService::commit_candidate() noexcept {
    owner_={};token_=0;expires_at_=0;graph_identity_size_=0;++revision_;++graph_revision_;
}
ProcessingReply ProcessingControlService::apply_candidate(SessionIdentity id,CommandHeader header,const ApplyProcessing& command) {
    if(!token_ || command.token!=token_)return result(id,header,ControlError::no_prepared,Status::unavailable);
    if(owner_!=id)return result(id,header,ControlError::foreign_preparation,Status::invalid_argument);
    if(base_revision_!=revision_)return result(id,header,ControlError::conflict,Status::invalid_argument);
    if(command.graph_identity!=std::string_view(graph_identity_.data(),graph_identity_size_))
        return result(id,header,ControlError::invalid_graph,Status::invalid_argument);
    const auto applied=runtime_.apply_transactional();if(!applied)return lifecycle_result(id,header,applied);
    commit_candidate();return result(id,header);
}
ProcessingReply ProcessingControlService::release_candidate(SessionIdentity id,CommandHeader header,const ReleasePrepared& command) {
    if(!token_ || command.token!=token_)return result(id,header,ControlError::no_prepared,Status::unavailable);
    if(owner_!=id)return result(id,header,ControlError::foreign_preparation,Status::invalid_argument);
    release();return result(id,header);
}
ProcessingReply ProcessingControlService::set_parameters(SessionIdentity id,CommandHeader header,const SetNumericProcessingParameters& command) {
    const auto* working=runtime_.working_numeric_graph();if(!working)return result(id,header,ControlError::invalid_graph,Status::unavailable);
    ProcessingDTO candidate;candidate.numeric=*working;
    if(!edit_processing_node(*candidate.numeric,command))return result(id,header,ControlError::invalid_graph,Status::invalid_argument);
    const auto staged=prepare(id,header,candidate);if(!staged.accepted)return staged;
    const auto applied=runtime_.apply_transactional();if(!applied){release();return lifecycle_result(id,header,applied);}
    commit_candidate();return result(id,header);
}
ProcessingReply ProcessingControlService::persist(SessionIdentity id,CommandHeader header,const ExplicitPersist& command) {
    if(!caps_.explicit_persist || !persistence_)return result(id,header,ControlError::unsupported,Status::unavailable);
    const auto* working=runtime_.working_numeric_graph();
    if(command.graph_revision!=graph_revision_ || !working)return result(id,header,ControlError::conflict,Status::invalid_argument);
    ProcessingDTO dto;dto.numeric=*working;const auto status=persistence_->persist_applied(dto,graph_revision_);
    if(status!=Status::ok)return result(id,header,ControlError::persistence_failure,status);
    persisted_revision_=graph_revision_;return result(id,header);
}
ProcessingReply ProcessingControlService::dispatch_mutation(SessionIdentity id,CommandHeader header,const ProcessingCommand& command) {
    if(const auto* value=std::get_if<PrepareProcessing>(&command))return prepare(id,header,*value->processing);
    if(const auto* value=std::get_if<ApplyProcessing>(&command))return apply_candidate(id,header,*value);
    if(const auto* value=std::get_if<ReleasePrepared>(&command))return release_candidate(id,header,*value);
    if(const auto* value=std::get_if<SetNumericProcessingParameters>(&command))return set_parameters(id,header,*value);
    if(std::holds_alternative<StartProcessing>(command) || std::holds_alternative<StopProcessing>(command))
        return result(id,header,ControlError::unsupported,Status::unsupported_version);
    return persist(id,header,std::get<ExplicitPersist>(command));
}
ProcessingReply ProcessingControlService::dispatch(SessionIdentity id,CommandHeader header,const ProcessingCommand& command,
                                                    std::uint64_t expired,SessionIdentity expired_owner) {
    if(std::holds_alternative<GetCapabilities>(command)){auto reply=result(id,header);reply.capabilities=&caps_;return reply;}
    if(std::holds_alternative<GetState>(command))return result(id,header);
    const auto refused=mutation_gate(id,header,command,expired,expired_owner);if(refused)return *refused;
    return dispatch_mutation(id,header,command);
}} // namespace application

