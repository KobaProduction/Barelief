#include <application/graph_lifecycle.h>
#include <core_app/numeric_default_graph.h>
#include <dataflow_runtime/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace application;
static void check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
struct Plan final:PreparedPlan {
 std::shared_ptr<const domain::NumericProcessingGraph> graph;MemoryRequirements memory;
 Plan(domain::NumericProcessingGraph g,MemoryRequirements m):graph(std::make_shared<const domain::NumericProcessingGraph>(std::move(g))),memory(m){}
 const domain::NumericProcessingGraph* numeric_definition()const noexcept override{return graph.get();}
 std::shared_ptr<const domain::NumericProcessingGraph> numeric_snapshot()const noexcept override{return graph;}
 MemoryRequirements memory_requirements()const noexcept override{return memory;}
 std::optional<domain::ProcessingGraphAdmission> numeric_admission()const noexcept override{return {};}
};
struct Context final:RuntimeContext{};
struct Preparer final:NumericGraphPreparationPort {
 int calls=0;Status status=Status::ok;MemoryRequirements memory{4,8,16};bool incomplete=false,wrong_plan=false,resolve_bindings=false,wrong_bindings=false;
 Status prepare_numeric(const domain::NumericProcessingGraph& g,const NumericRuntimeLimits& limits,PreparedExecution& out)override{
  ++calls;if(status!=Status::ok)return status;auto copy=g;if(wrong_plan)copy.graph_id="wrong";
  if(resolve_bindings){domain::NumericProcessingGraph resolved;if(!domain::resolve_configuration_bindings(copy,limits.admission,resolved))return Status::invalid_argument;copy=std::move(resolved);}
  if(wrong_bindings && !copy.configuration_bindings.empty())copy.configuration_bindings.pop_back();
  out.plan=std::make_shared<Plan>(std::move(copy),memory);if(!incomplete)out.context=std::make_unique<Context>();return Status::ok;
 }
};
struct Runtime final:GraphRuntimePort {
 int applies=0,starts=0,stops=0;bool requested=false;Status status=Status::ok;PreparedExecution active;
 Status apply(PreparedExecution&& execution)override{++applies;if(status!=Status::ok)return status;requested=true;active=std::move(execution);return Status::ok;}
 Status shutdown()override{++stops;if(status==Status::ok){active={};requested=false;}return status;}
};
static void configuration_identity(const dataflow_runtime::NumericRuntimeProfile& profile){
 auto requested=numeric_generator_graph();for(auto& node:requested.nodes)node.clock_node=-1;
 auto resolved=requested;for(auto& node:resolved.nodes)if(node.kind=="SignalGenerator")node.clock_node=16;
 check(same_numeric_processing_graph(requested,resolved),"derived clock cache must not change graph identity");
 auto missing=requested;check(!missing.configuration_bindings.empty(),"identity fixture requires explicit configuration associations");missing.configuration_bindings.pop_back();
 check(!same_numeric_processing_graph(requested,missing),"explicit configuration association defines graph identity");
 auto changed=requested;changed.configuration_bindings.back().input=changed.configuration_bindings.back().input==domain::ConfigurationRole::FrequencyGrid?domain::ConfigurationRole::SampleClock:domain::ConfigurationRole::FrequencyGrid;
 check(!same_numeric_processing_graph(requested,changed),"configuration role defines graph identity");
 Preparer preparer;preparer.resolve_bindings=true;Runtime runtime;
 GraphLifecycle lifecycle(preparer,runtime,dataflow_runtime::PreparedGraph::numeric_capabilities(profile),{profile.admission,profile.resources});
 check(bool(lifecycle.prepare_transactional(requested)),"wire-like graph with absent cache accepts resolved preparation");
 check(bool(lifecycle.apply_transactional()) && runtime.applies==1,"resolved cache graph commits through lifecycle");
 const auto* working=lifecycle.working_numeric_graph();check(working && working->nodes.front().clock_node==16,"preparer actually derived generator clock cache");
 check(bool(lifecycle.prepare_transactional(requested)),"stage valid binding candidate");
 preparer.wrong_bindings=true;
 const auto rejected=lifecycle.prepare_transactional(requested);
 check(rejected.status==Status::invalid_argument && lifecycle.has_prepared() && runtime.applies==1,"different prepared associations refused atomically");
 check(bool(lifecycle.apply_transactional()) && runtime.applies==2,"binding mismatch preserves prior prepared candidate");
}
int main(){
 const signal_contracts::FrameLimits frames{8,32768,262144,16384,256,8192}; dataflow_runtime::NumericRuntimeProfile profile{{32,64,8,16,256,65536,frames},{{1048576,1048576,1048576,65536},0,8388608},{32768,8,262144,262144,262144},{true,true,true}};
 configuration_identity(profile);
 const auto caps=dataflow_runtime::PreparedGraph::numeric_capabilities(profile);NumericRuntimeLimits limits{profile.admission,profile.resources};
 Preparer p;Runtime r;GraphLifecycle life(p,r,caps,limits);auto g=numeric_test_graph();
 check(life.state()==LifecycleState::Empty&&!life.working_numeric_graph(),"constructor mutates runtime");
 check(!life.apply_transactional(),"empty Apply");
 check(bool(life.prepare_transactional(g))&&r.applies==0,"Prepare mutates runtime");
 check(bool(life.apply_transactional())&&r.requested,"initial apply running");
 auto candidate=g;candidate.graph_id="candidate";check(bool(life.prepare_transactional(candidate)),"stage candidate");
 auto bad=g;bad.versions.processing=65535;const auto calls=p.calls;
 check(!life.prepare_transactional(bad)&&p.calls==calls&&life.has_prepared(),"future version bypasses validation or discards pending");
 bad=g;bad.edges.push_back({6,5,0,0});check(!life.prepare_transactional(bad)&&life.has_prepared(),"invalid draft discards pending");
 p.status=Status::exhausted;check(!life.prepare_transactional(g)&&life.has_prepared(),"failed preparer discards pending");p.status=Status::ok;
 p.incomplete=true;check(life.prepare_transactional(g).status==Status::invalid_argument&&life.has_prepared(),"incomplete bundle accepted");p.incomplete=false;
 p.wrong_plan=true;check(life.prepare_transactional(g).status==Status::invalid_argument&&life.has_prepared(),"different plan accepted");p.wrong_plan=false;
 p.memory={std::numeric_limits<std::size_t>::max(),1,0};check(life.prepare_transactional(g).status==Status::exhausted&&life.has_prepared(),"overflowing preparer memory accepted");p.memory={4,8,16};
 r.status=Status::unavailable;check(!life.apply_transactional()&&life.has_prepared()&&life.working_numeric_graph()->graph_id==g.graph_id&&life.state()==LifecycleState::Running,"failed Apply loses working/pending/intent");
 r.status=Status::ok;check(bool(life.apply_transactional())&&!life.has_prepared()&&life.working_numeric_graph()->graph_id=="candidate","retry pending");
 r.status=Status::unavailable;check(life.shutdown_transactional()==Status::unavailable && life.state()==LifecycleState::Running && life.working_numeric_graph(),"failed shutdown retains working graph");r.status=Status::ok;
 check(bool(life.prepare_transactional(g)),"pending teardown fixture");
 check(life.shutdown_transactional()==Status::ok && !life.has_prepared() && !life.working_numeric_graph() && life.state()==LifecycleState::Empty,"owner shutdown clears pending and working");
 check(!life.apply_transactional(),"shutdown fences old pending");
 check(bool(life.prepare_transactional(g)) && bool(life.apply_transactional()) && r.requested,"re-admission automatically runs");
 check(bool(life.disconnect(6)) && bool(life.disconnect(6)) && r.requested,"disconnect retains continuous execution/idempotence");
 check(bool(life.connect({15,6,0,0})),"reconnect");const auto applies=r.applies;
 check(!life.connect({6,5,0,0})&&r.applies==applies,"invalid connect reaches runtime");
 check(bool(life.prepare_transactional(g)),"discard fixture");life.discard_prepared();check(!life.apply_transactional(),"discarded candidate applied");
 check(MemoryRequirements{1,2,3}.fits(6)&&!MemoryRequirements{1,2,3}.fits(5)&&!MemoryRequirements{SIZE_MAX,1,0}.fits(SIZE_MAX),"memory accounting overflow");
 std::puts("canonical application lifecycle fault/transaction oracles PASS");
}
