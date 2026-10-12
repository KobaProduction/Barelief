#include <application/processing_control.h>
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace application {
namespace {
bool numeric_parameters_bounded(const domain::ProcessingParameters& params,const ProcessingCapabilities& caps) noexcept {
    if(!caps.numeric || params.valueless_by_exception())return false;
    const auto& limits=caps.numeric->runtime.admission;
    const auto text=std::min(limits.max_text_bytes,caps.control.max_field_string_bytes);
    return std::visit([&](const auto& p){using T=std::decay_t<decltype(p)>;
        if constexpr(std::is_same_v<T,domain::EndpointProcessingParameters> || std::is_same_v<T,domain::TransformProcessingParameters>) {
            if(p.values.size()>limits.max_parameters_per_node)return false;
            for(const auto& field:p.values){if(field.key.size()>text)return false;
                if(const auto* value=std::get_if<std::string>(&field.value);value && value->size()>text)return false;}
        } else if constexpr(std::is_same_v<T,domain::SignalGeneratorProcessingParameters>) {
            if(p.oscillators.empty() || p.oscillators.size()>128)return false;
        } else if constexpr(std::is_same_v<T,domain::SampleClockProcessingParameters>) {
            if(p.domain.empty() || p.domain.size()>text)return false;
        } else if constexpr(std::is_same_v<T,domain::MixerProcessingParameters>) {
            if(p.weights.size()<2 || p.weights.size()>128 || p.weights.size()>limits.max_ports_per_node)return false;
        } else if constexpr(std::is_same_v<T,domain::ReduceProcessingParameters>) {
            if(p.selected_channels.size()>limits.frame.max_planes)return false;
        }
        return true;
    },params);
}
bool numeric_graph_bounded(const domain::NumericProcessingGraph& g,const ProcessingCapabilities& caps) noexcept {
    if(!caps.numeric)return false;
    const auto& limits=caps.numeric->runtime.admission;
    if(g.graph_id.size()>caps.control.max_graph_identity_bytes || g.graph_id.size()>limits.max_text_bytes ||
       g.nodes.size()>limits.max_nodes || g.edges.size()>limits.max_edges)return false;
    std::size_t metadata=0;
    for(const auto& node:g.nodes){
        if(node.kind.size()>limits.max_text_bytes || node.kind.size()>caps.control.max_field_string_bytes ||
           node.inputs.size()>limits.max_ports_per_node || node.outputs.size()>limits.max_ports_per_node ||
           !numeric_parameters_bounded(node.parameters,caps))return false;
        for(const auto* ports:{&node.inputs,&node.outputs})for(const auto& port:*ports){
            const auto bytes=port.contract.owned_bytes();
            if(bytes>limits.max_owned_metadata_bytes-metadata)return false;
            metadata+=bytes;
        }
    }
    std::size_t owned=0;return domain::processing_graph_owned_bytes(g,limits,owned)==Status::ok;
}

bool same_command(const ProcessingCommand& a,const ProcessingCommand& b) noexcept {
    if(a.valueless_by_exception() || b.valueless_by_exception() || a.index()!=b.index())return false;
    return std::visit([&](const auto& x){
        using T=std::decay_t<decltype(x)>;const auto& y=std::get<T>(b);
        if constexpr(std::is_same_v<T,PrepareProcessing>)
            return x.processing&&y.processing&&x.processing->numeric&&y.processing->numeric&&
                same_numeric_processing_graph(*x.processing->numeric,*y.processing->numeric);
        else if constexpr(std::is_same_v<T,ApplyProcessing>)return x.token==y.token&&x.graph_identity==y.graph_identity;
        else if constexpr(std::is_same_v<T,ReleasePrepared>)return x.token==y.token;
        else if constexpr(std::is_same_v<T,SetNumericProcessingParameters>)
            return x.node==y.node&&x.enabled==y.enabled&&domain::same_processing_parameters(x.parameters,y.parameters);
        else if constexpr(std::is_same_v<T,ExplicitPersist>)return x.graph_revision==y.graph_revision;
        else return true;
    },a);
}
bool control_payload_budget_fits(const ProcessingCapabilities& caps,std::size_t budget) noexcept {
    const auto required=control_snapshot_bytes(caps);return required && *required<=budget;
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
    // Standalone SetParameters request is not a graph; reserve its active variant.
    std::size_t parameters=0;
    if(n.max_parameters_per_node>(SIZE_MAX-parameters)/sizeof(domain::ProcessingParameter))return {};
    parameters=n.max_parameters_per_node*sizeof(domain::ProcessingParameter);
    if(n.max_parameters_per_node>(SIZE_MAX-parameters)/(text+1)/2)return {};
    parameters+=n.max_parameters_per_node*(text+1)*2;
    if(n.frame.max_planes>(SIZE_MAX-parameters)/sizeof(std::uint32_t))return {};
    parameters+=n.frame.max_planes*sizeof(std::uint32_t);
    if(parameters>n.max_owned_metadata_bytes && !add(parameters-n.max_owned_metadata_bytes,1))return {};
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
    valid_config_=caps.numeric&&caps.numeric->device.query&&epoch&&l.max_pending_prepared==1&&l.prepared_ttl_ms&&l.max_sessions&&
        l.max_sessions<=slots.size()&&l.max_graph_identity_bytes&&l.max_graph_identity_bytes<=graph_identity_.size()&&
        l.max_prepared_bytes>=caps.numeric->runtime.resources.max_peak_bytes&&
        control_payload_budget_fits(caps,l.max_replay_bytes_per_session)&&control_payload_budget_fits(caps,l.max_control_scratch_bytes)&&
        l.max_replay_bytes_per_session&&l.max_sessions<=l.max_replay_bytes_total/l.max_replay_bytes_per_session&&
        (!caps.explicit_persist||persistence);
    if(valid_config_){sessions_=slots.first(l.max_sessions);for(auto& s:sessions_)s=ControlSessionSlot{};}
    else if(l.max_sessions>slots.size()||!control_payload_budget_fits(caps,l.max_replay_bytes_per_session)||
            !control_payload_budget_fits(caps,l.max_control_scratch_bytes)||
            (l.max_replay_bytes_per_session&&l.max_sessions>l.max_replay_bytes_total/l.max_replay_bytes_per_session))
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
    for(auto& slot:sessions_)if(!slot.identity.session){slot.identity={client,next_session_++,epoch_};return {ControlError::none,slot.identity};}
    return {ControlError::limit_exceeded,{}};
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
ProcessingReply ProcessingControlService::lifecycle_result(SessionIdentity id,CommandHeader header,const LifecycleResult& r) const {
    auto error=ControlError::none;
    if(!r.numeric_validation){using E=domain::ProcessingError;const auto e=r.numeric_validation.error;
        error=e==E::UnsupportedVersion||e==E::UnknownKind||e==E::UnsupportedCombination?ControlError::unsupported:
            e==E::Limit||e==E::ResourceLimit||e==E::InsufficientWorkspace?ControlError::limit_exceeded:ControlError::invalid_graph;
    }else if(r.status!=Status::ok)error=r.status==Status::exhausted||r.status==Status::buffer_too_small?ControlError::limit_exceeded:
        r.status==Status::unsupported_version?ControlError::unsupported:r.status==Status::invalid_argument||r.status==Status::incompatible_contract?
        ControlError::invalid_graph:ControlError::resource_failure;
    auto reply=result(id,header,error,r.status);reply.numeric_validation=r.numeric_validation;return reply;
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
ProcessingReply ProcessingControlService::prepare(SessionIdentity id,CommandHeader header,const ProcessingDTO& dto){
    if(token_&&owner_!=id)return result(id,header,ControlError::busy,Status::would_block);
    if(!dto.numeric)return result(id,header,ControlError::malformed,Status::invalid_argument);
    const auto now=clock_.now_ms();
    if(next_token_==std::numeric_limits<std::uint64_t>::max()||caps_.control.prepared_ttl_ms>std::numeric_limits<std::uint64_t>::max()-now)
        return result(id,header,ControlError::limit_exceeded,Status::overflow);
    const bool had_token=token_!=0;owner_=id;
    const auto prepared=runtime_.prepare_numeric_transactional(*dto.numeric,caps_.numeric->runtime);
    if(!prepared){if(!had_token)owner_={};return lifecycle_result(id,header,prepared);}
    token_=next_token_++;base_revision_=revision_;expires_at_=now+caps_.control.prepared_ttl_ms;
    const auto& identity=dto.numeric->graph_id;graph_identity_size_=identity.size();std::copy(identity.begin(),identity.end(),graph_identity_.begin());
    auto reply=result(id,header);reply.prepared_token=token_;reply.expires_at_ms=expires_at_;
    reply.memory=runtime_.pending_memory_requirements();reply.numeric_resources=runtime_.pending_numeric_admission();return reply;
}
ProcessingReply ProcessingControlService::submit(SessionIdentity id,CommandHeader header,const ProcessingCommand& command){
    const auto prior_token=token_;const auto prior_owner=owner_;const auto expired=expire_prepared()?prior_token:0;
    auto* slot=session(id);if(!slot)return result(id,header,ControlError::session_closed,Status::disconnected);
    if(!header.request_id)return result(id,header,ControlError::malformed,Status::invalid_argument);
    if(slot->completed&&slot->last_header.request_id==header.request_id){
        if(slot->last_header.expected_revision!=header.expected_revision||!bounded_command(command)||!same_command(slot->last_command,command))
            return result(id,header,ControlError::conflict,Status::invalid_argument);
        return slot->last_reply;
    }
    if(!bounded_command(command)){
        if((std::holds_alternative<PrepareProcessing>(command)&&token_&&owner_!=id)||
           (std::holds_alternative<SetNumericProcessingParameters>(command)&&token_))return result(id,header,ControlError::busy,Status::would_block);
        if(const auto* p=std::get_if<PrepareProcessing>(&command);p&&(!p->processing||!p->processing->numeric))
            return result(id,header,ControlError::malformed,Status::invalid_argument);
        return result(id,header,ControlError::limit_exceeded,Status::exhausted);
    }
    if(const auto* working=runtime_.working_numeric_graph();working&&!numeric_graph_bounded(*working,caps_))
        return result(id,header,ControlError::limit_exceeded,Status::exhausted);
    ControlSessionSlot staged;staged.identity=slot->identity;
#if defined(__cpp_exceptions)
    try {stage_request(staged,header,command);}catch(const std::bad_alloc&){return result(id,header,ControlError::resource_failure,Status::exhausted);}
    catch(const std::length_error&){return result(id,header,ControlError::limit_exceeded,Status::exhausted);}
#else
    stage_request(staged,header,command);
#endif
    ProcessingReply reply;
#if defined(__cpp_exceptions)
    try {reply=dispatch(id,header,command,expired,prior_owner);}
    catch(const std::bad_alloc&){return result(id,header,ControlError::resource_failure,Status::exhausted);}
    catch(const std::length_error&){return result(id,header,ControlError::limit_exceeded,Status::exhausted);}
#else
    reply=dispatch(id,header,command,expired,prior_owner);
#endif
    remember(*slot,std::move(staged),reply);return reply;
}
ProcessingReply ProcessingControlService::dispatch(SessionIdentity id,CommandHeader header,const ProcessingCommand& command,
    std::uint64_t expired,SessionIdentity expired_owner){
    if(std::holds_alternative<GetCapabilities>(command)){auto reply=result(id,header);reply.capabilities=&caps_;return reply;}
    if(std::holds_alternative<GetState>(command))return result(id,header);
    if(const auto* p=std::get_if<ApplyProcessing>(&command);p&&expired&&expired_owner==id&&expired==p->token)
        return result(id,header,ControlError::preparation_expired,Status::unavailable);
    if((std::holds_alternative<PrepareProcessing>(command)&&token_&&owner_!=id)||
       (std::holds_alternative<SetNumericProcessingParameters>(command)&&token_))return result(id,header,ControlError::busy,Status::would_block);
    if(header.expected_revision!=revision_)return result(id,header,ControlError::conflict,Status::invalid_argument);
    if(revision_==std::numeric_limits<std::uint64_t>::max())return result(id,header,ControlError::limit_exceeded,Status::overflow);
    if(const auto* p=std::get_if<PrepareProcessing>(&command))return prepare(id,header,*p->processing);
    if(const auto* p=std::get_if<ApplyProcessing>(&command)){
        if(!token_||p->token!=token_)return result(id,header,ControlError::no_prepared,Status::unavailable);
        if(owner_!=id)return result(id,header,ControlError::foreign_preparation,Status::invalid_argument);
        if(base_revision_!=revision_)return result(id,header,ControlError::conflict,Status::invalid_argument);
        if(p->graph_identity!=std::string_view(graph_identity_.data(),graph_identity_size_))return result(id,header,ControlError::invalid_graph,Status::invalid_argument);
        const auto applied=runtime_.apply_transactional();if(!applied)return lifecycle_result(id,header,applied);
        owner_={};token_=0;expires_at_=0;graph_identity_size_=0;++revision_;++graph_revision_;return result(id,header);
    }
    if(const auto* p=std::get_if<ReleasePrepared>(&command)){
        if(!token_||p->token!=token_)return result(id,header,ControlError::no_prepared,Status::unavailable);
        if(owner_!=id)return result(id,header,ControlError::foreign_preparation,Status::invalid_argument);
        release();return result(id,header);
    }
    if(const auto* p=std::get_if<SetNumericProcessingParameters>(&command)){
        const auto* working=runtime_.working_numeric_graph();if(!working)return result(id,header,ControlError::invalid_graph,Status::unavailable);
        ProcessingDTO candidate;candidate.numeric=*working;
        auto node=std::find_if(candidate.numeric->nodes.begin(),candidate.numeric->nodes.end(),[&](const auto& n){return n.id==p->node;});
        if(node==candidate.numeric->nodes.end())return result(id,header,ControlError::invalid_graph,Status::invalid_argument);
        node->parameters=p->parameters;node->enabled=p->enabled;
        auto staged=prepare(id,header,candidate);if(!staged.accepted)return staged;
        const auto applied=runtime_.apply_transactional();if(!applied){release();return lifecycle_result(id,header,applied);}
        owner_={};token_=0;expires_at_=0;graph_identity_size_=0;++revision_;++graph_revision_;return result(id,header);
    }
    if(std::holds_alternative<StartProcessing>(command)||std::holds_alternative<StopProcessing>(command))
        return result(id,header,ControlError::unsupported,Status::unsupported_version);
    const auto& persist=std::get<ExplicitPersist>(command);
    if(!caps_.explicit_persist||!persistence_)return result(id,header,ControlError::unsupported,Status::unavailable);
    const auto* working=runtime_.working_numeric_graph();
    if(persist.graph_revision!=graph_revision_||!working)return result(id,header,ControlError::conflict,Status::invalid_argument);
    ProcessingDTO dto;dto.numeric=*working;const auto status=persistence_->persist_applied(dto,graph_revision_);
    if(status!=Status::ok)return result(id,header,ControlError::persistence_failure,status);
    persisted_revision_=graph_revision_;return result(id,header);
}
} // namespace application

