#include <application/processing_control.h>
#include "canonical_runtime_test_fixture.h"
using namespace application;
using canonical_test::check;
struct Clock final:ControlClock{std::uint64_t now=100;std::uint64_t now_ms()const noexcept override{return now;}};
struct Runtime final:ProcessingControlRuntimePort {
 std::shared_ptr<const domain::NumericProcessingGraph> working,pending;int prepares=0,applies=0,discards=0;
 LifecycleState lifecycle=LifecycleState::Empty;Status stage=Status::ok,commit=Status::ok;
 LifecycleResult prepare_numeric_transactional(const domain::NumericProcessingGraph& g,const NumericRuntimeLimits&)override{auto p=canonical_test::profile();std::array<std::size_t,64> workspace{};domain::ProcessingGraphAdmission admitted;auto diagnostic=domain::admit_processing_graph(g,p.admission,dataflow_runtime::PreparedGraph::numeric_capabilities(p),workspace,admitted);if(!diagnostic)return {Status::invalid_argument,diagnostic};++prepares;if(stage!=Status::ok)return {stage,{}};pending=std::make_shared<const domain::NumericProcessingGraph>(g);return {};}
 LifecycleResult apply_transactional()override{++applies;if(commit!=Status::ok)return {commit,{}};if(!pending)return {Status::invalid_argument,{}};working=std::move(pending);lifecycle=LifecycleState::Running;return {};}
 void discard_prepared()noexcept override{pending.reset();++discards;}
 MemoryRequirements pending_memory_requirements()const noexcept override{return pending?MemoryRequirements{64,128,64}:MemoryRequirements{};}
 LifecycleState state()const noexcept override{return lifecycle;}
 const domain::NumericProcessingGraph* working_numeric_graph()const noexcept override{return working.get();}
 std::shared_ptr<const domain::NumericProcessingGraph> working_numeric_snapshot()const noexcept override{return working;}
};
struct Persistence final:ProcessingPersistencePort{int writes=0;Status status=Status::ok;std::uint64_t revision=0;Status persist_applied(const ProcessingDTO&,std::uint64_t r)override{++writes;if(status==Status::ok)revision=r;return status;}};
struct Fixture {
 Clock clock;Runtime runtime;Persistence persistence;ProcessingCapabilities caps;std::array<ControlSessionSlot,2> slots;
 Fixture(){auto p=canonical_test::profile();caps.numeric=NumericProcessingCapabilities{dataflow_runtime::PreparedGraph::numeric_capabilities(p),{p.admission,p.resources}};caps.explicit_persist=true;caps.control.max_sessions=2;caps.control.max_replay_bytes_per_session=2097152;caps.control.max_replay_bytes_total=4194304;caps.control.max_control_scratch_bytes=2097152;}
};
static ProcessingDTO dto(){ProcessingDTO d;d.numeric=numeric_test_graph();return d;}
static void replay_faults(){Fixture f;ProcessingControlService service(f.runtime,f.clock,f.caps,f.slots,77,&f.persistence);auto a=service.open_session(1),b=service.open_session(2);check(bool(a)&&bool(b),"open sessions");auto d=dto();
 auto staged=service.submit(a.identity,{1,0},PrepareProcessing{&d});check(staged.accepted,"stage");check(service.submit(a.identity,{1,0},PrepareProcessing{&d}).prepared_token==staged.prepared_token&&f.runtime.prepares==1,"exact prepare replay invokes runtime");
 auto changed=d;changed.numeric->nodes[0].enabled=false;check(service.submit(a.identity,{1,0},PrepareProcessing{&changed}).error==ControlError::conflict,"same request changed data");
 check(service.submit(b.identity,{1,0},PrepareProcessing{&d}).error==ControlError::busy,"pending ownership");f.runtime.commit=Status::unavailable;
 auto failed=service.submit(a.identity,{2,0},ApplyProcessing{staged.prepared_token,"main"});check(!failed.accepted&&f.runtime.pending&&failed.state.revision==0,"failed Apply loses pending/revision");
 check(!service.submit(a.identity,{2,0},ApplyProcessing{staged.prepared_token,"main"}).accepted&&f.runtime.applies==1,"failed Apply replay invokes runtime");f.runtime.commit=Status::ok;
 auto applied=service.submit(a.identity,{3,0},ApplyProcessing{staged.prepared_token,"main"});check(applied.accepted&&applied.state.revision==1,"Apply retry");
 auto params=d.numeric->nodes[0].parameters;check(service.submit(b.identity,{2,1},SetNumericProcessingParameters{1,params,false}).accepted,"parameter edit");
 auto replay=service.submit(a.identity,{3,0},ApplyProcessing{staged.prepared_token,"main"});check(replay.accepted&&replay.state.revision==1&&replay.state.applied_numeric->nodes[0].enabled&&!f.runtime.working->nodes[0].enabled,"replay snapshot changed after other client edit");
 check(service.submit(b.identity,{3,2},StopProcessing{}).error==ControlError::unsupported && f.runtime.lifecycle==LifecycleState::Running,"user Stop rejected without changing execution");
 check(service.submit(b.identity,{4,2},StartProcessing{}).error==ControlError::unsupported && f.runtime.lifecycle==LifecycleState::Running,"user Start rejected without changing execution");
 f.persistence.status=Status::unavailable;check(service.submit(a.identity,{4,2},ExplicitPersist{2}).error==ControlError::persistence_failure,"persistence failure hidden");check(service.submit(a.identity,{4,2},ExplicitPersist{2}).error==ControlError::persistence_failure&&f.persistence.writes==1,"failed persist replay writes twice");
 f.persistence.status=Status::ok;check(service.submit(a.identity,{5,2},ExplicitPersist{2}).accepted&&f.persistence.revision==2,"persist retry");check(service.submit(a.identity,{5,2},ExplicitPersist{2}).accepted&&f.persistence.writes==2,"successful persist replay writes twice");
}
static void boundaries(){Fixture f;f.caps.control.prepared_ttl_ms=17;ProcessingControlService service(f.runtime,f.clock,f.caps,f.slots,78,&f.persistence);auto a=service.open_session(1),b=service.open_session(2);check(bool(a)&&bool(b),"sessions");check(service.open_session(3).error==ControlError::limit_exceeded&&service.open_session(1).error==ControlError::busy,"session bounds");auto d=dto();auto staged=service.submit(a.identity,{1,0},PrepareProcessing{&d});check(staged.accepted&&staged.expires_at_ms==117,"TTL arithmetic");
 check(service.submit(a.identity,{2,0},PrepareProcessing{}).error==ControlError::malformed&&f.runtime.pending,"malformed Prepare destroys pending");
 check(service.submit(b.identity,{1,0},ReleasePrepared{staged.prepared_token}).error==ControlError::foreign_preparation,"foreign release");
 check(service.submit(a.identity,{3,0},ApplyProcessing{staged.prepared_token,"wrong"}).error==ControlError::invalid_graph&&f.runtime.applies==0,"wrong graph Apply reaches runtime");
 auto invalid=d;invalid.numeric->versions.processing=99;check(service.submit(a.identity,{4,0},PrepareProcessing{&invalid}).error==ControlError::unsupported&&f.runtime.prepares==1,"future graph reaches runtime");
 invalid=d;invalid.numeric->graph_id.assign(257,'x');check(service.submit(a.identity,{5,0},PrepareProcessing{&invalid}).error==ControlError::limit_exceeded,"identity limit");
 f.clock.now=116;check(!service.expire_prepared()&&f.runtime.pending,"early expiry");f.clock.now=117;check(service.expire_prepared()&&!f.runtime.pending,"expiry boundary");
 check(service.submit(b.identity,{2,0},PrepareProcessing{&d}).accepted,"prepare after expiry");check(service.close_session(a.identity)==ControlError::none&&f.runtime.pending,"unrelated close discards pending");check(service.close_session(b.identity)==ControlError::none&&!f.runtime.pending,"owner close retains pending");
 a=service.open_session(1);f.clock.now=UINT64_MAX-5;check(service.submit(a.identity,{1,0},PrepareProcessing{&d}).error==ControlError::limit_exceeded&&!f.runtime.pending,"TTL overflow");
 Fixture tiny;tiny.caps.control.max_replay_bytes_per_session=1;ProcessingControlService small(tiny.runtime,tiny.clock,tiny.caps,tiny.slots,79,&tiny.persistence);check(small.open_session(1).error==ControlError::limit_exceeded,"replay capacity");
}
static void expiry_before_returns(){for(int branch=0;branch<5;++branch){Fixture f;f.caps.control.prepared_ttl_ms=17;ProcessingControlService service(f.runtime,f.clock,f.caps,f.slots,80+branch,&f.persistence);auto a=service.open_session(1);auto d=dto();auto p=service.submit(a.identity,{1,0},PrepareProcessing{&d});check(p.accepted,"expiry fixture");f.clock.now=117;ProcessingReply reply;
 if(branch==0){reply=service.submit(a.identity,{0,0},GetState{});check(reply.error==ControlError::malformed,"zero request");}
 if(branch==1){d.numeric->nodes[0].enabled=false;reply=service.submit(a.identity,{1,0},PrepareProcessing{&d});check(reply.error==ControlError::conflict,"changed replay");}
 if(branch==2){auto stale=a.identity;++stale.epoch;reply=service.submit(stale,{2,0},GetState{});check(reply.error==ControlError::session_closed,"stale session");}
 if(branch==3){reply=service.submit(a.identity,{1,0},PrepareProcessing{&d});check(reply.accepted&&reply.prepared_token==p.prepared_token,"cached reply after expiry");}
 if(branch==4){reply=service.submit(a.identity,{2,0},ApplyProcessing{p.prepared_token,"main"});check(reply.error==ControlError::preparation_expired,"expired Apply");}
 check(!f.runtime.pending&&f.runtime.discards==1,"expiry skipped by early return");}}
static void exact_numeric_replay(){Fixture f;ProcessingControlService service(f.runtime,f.clock,f.caps,f.slots,91,&f.persistence);auto a=service.open_session(1);auto d=dto();auto& values=std::get<domain::EndpointProcessingParameters>(d.numeric->nodes[0].parameters).values;for(auto& v:values)if(v.key=="amplitude")v.value=signal_contracts::NumericLiteral{0.0};check(service.submit(a.identity,{1,0},PrepareProcessing{&d}).accepted,"exact numeric fixture");for(auto& v:values)if(v.key=="amplitude")v.value=signal_contracts::NumericLiteral{-0.0};check(service.submit(a.identity,{1,0},PrepareProcessing{&d}).error==ControlError::conflict&&f.runtime.prepares==1,"negative zero collapsed by replay");}
static void owner_observation(){
 Fixture f;f.caps.control.max_sessions=1;f.caps.control.prepared_ttl_ms=17;
 ProcessingControlService service(f.runtime,f.clock,f.caps,f.slots,92,&f.persistence);
 const auto local=service.open_session(1);check(bool(local),"single bootstrap session");
 auto d=dto();const auto staged=service.submit(local.identity,{1,0},PrepareProcessing{&d});
 check(staged.accepted,"single slot prepare");
 const auto observed=service.observe_state();
 check(observed.epoch==92 && observed.own_prepared_token==0 && f.runtime.pending,"observer exposes no prepare authority");
 check(f.slots[0].completed && f.slots[0].last_header.request_id==1 && f.slots[0].last_reply.prepared_token==staged.prepared_token,"observation preserves exact replay");
 check(service.open_session(2).error==ControlError::limit_exceeded,"observer does not bypass physical session bound");
 f.clock.now=117;service.observe_state();
 check(!f.runtime.pending && f.runtime.discards==1,"owner observation expires without a request");
 check(service.close_session(local.identity)==ControlError::none,"bootstrap releases slot");
 const auto remote=service.open_session(2);check(bool(remote),"wire owns released physical slot");
 check(service.observe_state().own_prepared_token==0 && !f.slots[0].completed,"observation retains no replay owner");
}
int main(){replay_faults();boundaries();expiry_before_returns();exact_numeric_replay();owner_observation();std::puts("canonical control replay/faults/TTL/limits/exact numeric oracles PASS");}


