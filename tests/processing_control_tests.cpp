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
 explicit Fixture(std::uint64_t ttl=30000,std::size_t sessions=2){auto p=canonical_test::profile();caps.numeric=NumericProcessingCapabilities{dataflow_runtime::PreparedGraph::numeric_capabilities(p),{p.admission,p.resources}};caps.explicit_persist=true;caps.control.max_sessions=sessions;caps.control.prepared_ttl_ms=ttl;caps.control.max_replay_bytes_per_session=2097152;caps.control.max_replay_bytes_total=4194304;caps.control.max_control_scratch_bytes=2097152;}
};
static ProcessingDTO dto(){ProcessingDTO d;d.numeric=numeric_test_graph();return d;}
struct ControlScenario {
 Fixture fixture;
 ProcessingControlService service;
 OpenControlSessionResult a,b;
 ProcessingDTO data=dto();
 ProcessingReply staged;
 explicit ControlScenario(std::uint64_t epoch=77,std::uint64_t ttl=30000)
  :fixture(ttl),service(fixture.runtime,fixture.clock,fixture.caps,fixture.slots,epoch,&fixture.persistence),
   a(service.open_session(1)),b(service.open_session(2)) {
  check(bool(a)&&bool(b),"open sessions");
 }
 void prepare() {
  staged=service.submit(a.identity,{1,0},PrepareProcessing{&data});
  check(staged.accepted,"stage");
 }
 void prepare_replay() {
  const auto replay=service.submit(a.identity,{1,0},PrepareProcessing{&data});
  check(replay.prepared_token==staged.prepared_token&&fixture.runtime.prepares==1,"exact prepare replay invokes runtime");
  auto changed=data;changed.numeric->nodes[0].enabled=false;
  check(service.submit(a.identity,{1,0},PrepareProcessing{&changed}).error==ControlError::conflict,"same request changed data");
  check(service.submit(b.identity,{1,0},PrepareProcessing{&data}).error==ControlError::busy,"pending ownership");
 }
 void apply_retry() {
  fixture.runtime.commit=Status::unavailable;
  const auto failed=service.submit(a.identity,{2,0},ApplyProcessing{staged.prepared_token,"main"});
  check(!failed.accepted&&fixture.runtime.pending&&failed.state.revision==0,"failed Apply loses pending/revision");
  check(!service.submit(a.identity,{2,0},ApplyProcessing{staged.prepared_token,"main"}).accepted&&fixture.runtime.applies==1,"failed Apply replay invokes runtime");
  fixture.runtime.commit=Status::ok;
  const auto applied=service.submit(a.identity,{3,0},ApplyProcessing{staged.prepared_token,"main"});
  check(applied.accepted&&applied.state.revision==1,"Apply retry");
 }
 void immutable_applied_replay() {
  const auto parameters=data.numeric->nodes[0].parameters;
  check(service.submit(b.identity,{2,1},SetNumericProcessingParameters{1,parameters,false}).accepted,"parameter edit");
  const auto replay=service.submit(a.identity,{3,0},ApplyProcessing{staged.prepared_token,"main"});
  check(replay.accepted&&replay.state.revision==1&&replay.state.applied_numeric->nodes[0].enabled&&!fixture.runtime.working->nodes[0].enabled,"replay snapshot changed after other client edit");
 }
 void reject_execution_commands() {
  check(service.submit(b.identity,{3,2},StopProcessing{}).error==ControlError::unsupported&&fixture.runtime.lifecycle==LifecycleState::Running,"user Stop rejected without changing execution");
  check(service.submit(b.identity,{4,2},StartProcessing{}).error==ControlError::unsupported&&fixture.runtime.lifecycle==LifecycleState::Running,"user Start rejected without changing execution");
 }
 void persist_retry() {
  fixture.persistence.status=Status::unavailable;
  check(service.submit(a.identity,{4,2},ExplicitPersist{2}).error==ControlError::persistence_failure,"persistence failure hidden");
  check(service.submit(a.identity,{4,2},ExplicitPersist{2}).error==ControlError::persistence_failure&&fixture.persistence.writes==1,"failed persist replay writes twice");
  fixture.persistence.status=Status::ok;
  check(service.submit(a.identity,{5,2},ExplicitPersist{2}).accepted&&fixture.persistence.revision==2,"persist retry");
  check(service.submit(a.identity,{5,2},ExplicitPersist{2}).accepted&&fixture.persistence.writes==2,"successful persist replay writes twice");
 }
 void prepare_boundaries() {
  check(service.open_session(3).error==ControlError::limit_exceeded&&service.open_session(1).error==ControlError::busy,"session bounds");
  check(staged.expires_at_ms==117,"TTL arithmetic");
  check(service.submit(a.identity,{2,0},PrepareProcessing{}).error==ControlError::malformed&&fixture.runtime.pending,"malformed Prepare destroys pending");
  check(service.submit(b.identity,{1,0},ReleasePrepared{staged.prepared_token}).error==ControlError::foreign_preparation,"foreign release");
  check(service.submit(a.identity,{3,0},ApplyProcessing{staged.prepared_token,"wrong"}).error==ControlError::invalid_graph&&fixture.runtime.applies==0,"wrong graph Apply reaches runtime");
 }
 void reject_invalid_prepare() {
  auto invalid=data;invalid.numeric->versions.processing=99;
  check(service.submit(a.identity,{4,0},PrepareProcessing{&invalid}).error==ControlError::unsupported&&fixture.runtime.prepares==1,"future graph reaches runtime");
  invalid=data;invalid.numeric->graph_id.assign(257,'x');
  check(service.submit(a.identity,{5,0},PrepareProcessing{&invalid}).error==ControlError::limit_exceeded,"identity limit");
 }
 void expire_and_close() {
  fixture.clock.now=116;check(!service.expire_prepared()&&fixture.runtime.pending,"early expiry");
  fixture.clock.now=117;check(service.expire_prepared()&&!fixture.runtime.pending,"expiry boundary");
  check(service.submit(b.identity,{2,0},PrepareProcessing{&data}).accepted,"prepare after expiry");
  check(service.close_session(a.identity)==ControlError::none&&fixture.runtime.pending,"unrelated close discards pending");
  check(service.close_session(b.identity)==ControlError::none&&!fixture.runtime.pending,"owner close retains pending");
 }
 void reject_ttl_overflow() {
  a=service.open_session(1);fixture.clock.now=UINT64_MAX-5;
  check(service.submit(a.identity,{1,0},PrepareProcessing{&data}).error==ControlError::limit_exceeded&&!fixture.runtime.pending,"TTL overflow");
 }
};
static void replay_faults() {
 ControlScenario scenario;scenario.prepare();scenario.prepare_replay();scenario.apply_retry();
 scenario.immutable_applied_replay();scenario.reject_execution_commands();scenario.persist_retry();
}
static void replay_capacity_boundary() {
 Fixture tiny;tiny.caps.control.max_replay_bytes_per_session=1;
 ProcessingControlService service(tiny.runtime,tiny.clock,tiny.caps,tiny.slots,79,&tiny.persistence);
 check(service.open_session(1).error==ControlError::limit_exceeded,"replay capacity");
}
static void boundaries() {
 ControlScenario scenario(78,17);scenario.prepare();scenario.prepare_boundaries();scenario.reject_invalid_prepare();
 scenario.expire_and_close();scenario.reject_ttl_overflow();replay_capacity_boundary();
}
static void expired_request(ControlScenario& scenario,int branch) {
 auto& service=scenario.service;auto& data=scenario.data;const auto identity=scenario.a.identity;
 if(branch==0)check(service.submit(identity,{0,0},GetState{}).error==ControlError::malformed,"zero request");
 else if(branch==1) {
  data.numeric->nodes[0].enabled=false;
  check(service.submit(identity,{1,0},PrepareProcessing{&data}).error==ControlError::conflict,"changed replay");
 } else if(branch==2) {
  auto stale=identity;++stale.epoch;
  check(service.submit(stale,{2,0},GetState{}).error==ControlError::session_closed,"stale session");
 } else if(branch==3) {
  const auto reply=service.submit(identity,{1,0},PrepareProcessing{&data});
  check(reply.accepted&&reply.prepared_token==scenario.staged.prepared_token,"cached reply after expiry");
 } else check(service.submit(identity,{2,0},ApplyProcessing{scenario.staged.prepared_token,"main"}).error==ControlError::preparation_expired,"expired Apply");
}
static void expiry_before_returns() {
 for(int branch=0;branch<5;++branch) {
  ControlScenario scenario(80+branch,17);scenario.prepare();scenario.fixture.clock.now=117;
  expired_request(scenario,branch);
  check(!scenario.fixture.runtime.pending&&scenario.fixture.runtime.discards==1,"expiry skipped by early return");
 }
}
static void set_amplitude(ProcessingDTO& data,double amplitude) {
 auto& fields=std::get<domain::EndpointProcessingParameters>(data.numeric->nodes[0].parameters).values;
 for(auto& field:fields)if(field.key=="amplitude")field.value=signal_contracts::NumericLiteral{amplitude};
}
static void exact_numeric_replay() {
 ControlScenario scenario(91);set_amplitude(scenario.data,0.0);scenario.prepare();
 set_amplitude(scenario.data,-0.0);
 check(scenario.service.submit(scenario.a.identity,{1,0},PrepareProcessing{&scenario.data}).error==ControlError::conflict&&scenario.fixture.runtime.prepares==1,"negative zero collapsed by replay");
}
struct ObservationScenario {
 Fixture fixture;
 ProcessingControlService service;
 explicit ObservationScenario():fixture(17,1),service(fixture.runtime,fixture.clock,fixture.caps,fixture.slots,92,&fixture.persistence) {}
 void run() {
  const auto local=service.open_session(1);check(bool(local),"single bootstrap session");
  auto data=dto();const auto staged=service.submit(local.identity,{1,0},PrepareProcessing{&data});
  check(staged.accepted,"single slot prepare");
  check_pending_observation(staged);
  fixture.clock.now=117;service.observe_state();
  check(!fixture.runtime.pending&&fixture.runtime.discards==1,"owner observation expires without a request");
  check(service.close_session(local.identity)==ControlError::none,"bootstrap releases slot");
  const auto remote=service.open_session(2);check(bool(remote),"wire owns released physical slot");
  check(service.observe_state().own_prepared_token==0&&!fixture.slots[0].completed,"observation retains no replay owner");
 }
 void check_pending_observation(const ProcessingReply& staged) {
  const auto observed=service.observe_state();
  check(observed.epoch==92&&observed.own_prepared_token==0&&fixture.runtime.pending,"observer exposes no prepare authority");
  check(fixture.slots[0].completed&&fixture.slots[0].last_header.request_id==1&&fixture.slots[0].last_reply.prepared_token==staged.prepared_token,"observation preserves exact replay");
  check(service.open_session(2).error==ControlError::limit_exceeded,"observer does not bypass physical session bound");
 }
};
static void owner_observation(){ObservationScenario scenario;scenario.run();}
static void add_single_input_mixer(ProcessingDTO& data) {
 auto& graph=*data.numeric;const auto contract=graph.nodes.front().outputs.front().contract;
 domain::ConfiguredProcessingNode mixer;mixer.id=7000;mixer.kind="Mixer";
 mixer.parameters=domain::MixerProcessingParameters{domain::MixerMode::WeightedSum,{{0,1}},100,domain::MixerConsumption::SourceAligned};
 mixer.inputs={{0,domain::PortDirection::Input,contract}};mixer.outputs={{0,domain::PortDirection::Output,contract}};
 graph.nodes.push_back(std::move(mixer));graph.edges.push_back({1,7000,0,0});
}
static void single_input_mixer_control() {
 ControlScenario scenario(93);add_single_input_mixer(scenario.data);scenario.prepare();
 const auto applied=scenario.service.submit(scenario.a.identity,{2,0},ApplyProcessing{scenario.staged.prepared_token,"main"});
 check(applied.accepted&&applied.state.revision==1,"single input Mixer follows domain admission");
 const domain::ProcessingParameters parameters=domain::MixerProcessingParameters{domain::MixerMode::WeightedSum,{{0,.5}},100,domain::MixerConsumption::SourceAligned};
 const auto edited=scenario.service.submit(scenario.a.identity,{3,1},SetNumericProcessingParameters{7000,parameters,true});
 check(edited.accepted&&edited.state.revision==2,"single weight parameter update reaches canonical admission");
}
int main(){replay_faults();boundaries();expiry_before_returns();exact_numeric_replay();owner_observation();single_input_mixer_control();std::puts("canonical control replay/faults/TTL/limits/exact numeric oracles PASS");}
