#include <application/graph_lifecycle.h>
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace application;
static void check(bool ok,const char* why) { if(!ok) {std::cerr<<why<<'\n';std::exit(1);} }
struct Plan final : PreparedPlan {
    domain::GraphDefinition graph;
    MemoryRequirements memory;
    Plan(domain::GraphDefinition g, MemoryRequirements m):graph(std::move(g)),memory(m) {}
    const domain::GraphDefinition& definition() const noexcept override {return graph;}
    MemoryRequirements memory_requirements() const noexcept override {return memory;}
};
struct Context final : RuntimeContext {};
struct Preparer final : GraphPreparationPort {
    int calls=0; Status status=Status::ok; MemoryRequirements memory{4,8,16}; bool incomplete=false, wrong_plan=false;
    Status prepare(const domain::GraphDefinition& graph,const std::vector<domain::NodeId>& order,
                   const RuntimeLimits&,PreparedExecution& out) override {
        ++calls; check(!order.empty(),"validated order missing");
        if(status!=Status::ok) return status;
        auto copy=graph; if(wrong_plan) copy.graph_id="wrong";
        out.plan=std::make_shared<Plan>(copy,memory);
        if(!incomplete) out.context=std::make_unique<Context>();
        return Status::ok;
    }
};
struct Runtime final : GraphRuntimePort {
    int applies=0,starts=0,stops=0; bool requested=false; Status status=Status::ok;
    PreparedExecution active;
    Status apply(PreparedExecution&& execution,bool run) override {
        ++applies;if(status!=Status::ok) return status;
        requested=run;active=std::move(execution);return Status::ok;
    }
    Status start() override {++starts;return status;}
    Status stop() override {++stops;return status;}
};
static domain::GraphDefinition graph() {
    return {"main",{{0,domain::NodeKind::SpectrumGain,domain::GainParameters{}},
                    {1,domain::NodeKind::Mel,domain::MelParameters{}}},{{0,1}}};
}
int main() {
    Preparer factory;Runtime runtime;
    GraphLifecycle lifecycle(factory,runtime,domain::desktop_capabilities());
    check(lifecycle.state()==LifecycleState::Empty && !lifecycle.working_graph(),"constructor runs resources");
    check(lifecycle.start()==Status::invalid_argument,"start without graph");
    check(bool(lifecycle.prepare(graph())) && runtime.applies==0 && lifecycle.has_prepared(),"prepare mutates runtime");
    check(bool(lifecycle.apply()) && runtime.requested && lifecycle.state()==LifecycleState::Running,"first apply starts");
    const auto original=*lifecycle.working_graph();
    auto bad=graph();bad.edges={{1,0}};
    check(!lifecycle.prepare(bad) && factory.calls==1 && *lifecycle.working_graph()==original,"invalid draft replaced working");
    check(!lifecycle.apply() && runtime.applies==1,"invalid draft leaves stale pending");
    factory.status=Status::exhausted;
    check(!lifecycle.prepare(graph()) && *lifecycle.working_graph()==original,"allocation failure replaced working");
    factory.status=Status::ok;
    check(lifecycle.stop()==Status::ok && lifecycle.state()==LifecycleState::Stopped,"stop state");
    check(bool(lifecycle.prepare(graph())) && bool(lifecycle.apply()) && !runtime.requested,"apply overrides explicit stop");
    check(lifecycle.start()==Status::ok && lifecycle.state()==LifecycleState::Running,"explicit restart");
    check(bool(lifecycle.prepare(graph())) && lifecycle.stop()==Status::ok && bool(lifecycle.apply()) && !runtime.requested,"late prepared ACK overrides stop");
    runtime.status=Status::disconnected;
    check(bool(lifecycle.prepare(graph())) && !lifecycle.apply() && *lifecycle.working_graph()==original && lifecycle.state()==LifecycleState::Stopped,"failed commit replaces working");
    runtime.status=Status::ok;
    check(bool(lifecycle.disconnect(1)) && lifecycle.working_graph()->edges.empty() && !runtime.requested,"disconnect must preserve stop intent");
    check(bool(lifecycle.connect({0,1})) && lifecycle.working_graph()->edges.size()==1,"typed reconnect");
    const auto before=runtime.applies;
    check(!lifecycle.connect({1,0}) && runtime.applies==before,"incompatible connect reaches runtime");
    check(bool(lifecycle.disconnect(1)) && bool(lifecycle.disconnect(1)),"disconnect idempotence");
    factory.memory={std::numeric_limits<std::size_t>::max(),1,0};
    check(lifecycle.prepare(graph()).status==Status::exhausted,"memory overflow accepted");
    factory.memory={4,8,16};factory.incomplete=true;
    check(lifecycle.prepare(graph()).status==Status::invalid_argument,"incomplete prepared execution");
    factory.incomplete=false;factory.wrong_plan=true;
    check(lifecycle.prepare(graph()).status==Status::invalid_argument,"wrong plan accepted");
    factory.wrong_plan=false;
    auto large=graph();large.nodes.push_back({2,domain::NodeKind::Window,domain::WindowParameters{4096}});
    GraphLifecycle small(factory,runtime,domain::desktop_capabilities(),{{128,256},1024,512,2});
    check(small.prepare(large).status==Status::exhausted,"runtime frame limit");
    large=graph();large.nodes.push_back({2,domain::NodeKind::Spectrogram,domain::HistoryParameters{400}});
    check(small.prepare(large).status==Status::exhausted,"runtime history limit");
    runtime.status=Status::disconnected;
    check(lifecycle.start()==Status::disconnected && lifecycle.state()==LifecycleState::Stopped,"failed start changes state");
    check(lifecycle.stop()==Status::disconnected,"failed stop status lost");
    runtime.status=Status::ok;
    check(bool(lifecycle.prepare(graph())) && bool(lifecycle.apply()) && !runtime.requested,"failed stop loses stop intent");
    check(!lifecycle.disconnect(99) && !lifecycle.disconnect(1,1),"invalid disconnect accepted");
    check(bool(lifecycle.prepare(graph())),"limits pending fixture");
    const auto before_limits=*lifecycle.working_graph();
    RuntimeLimits reduced;reduced.max_memory_bytes=1;
    lifecycle.runtime_limits(reduced);
    check(!lifecycle.has_prepared() && !lifecycle.apply(),"limits change leaves old pending applicable");
    check(lifecycle.state()==LifecycleState::Stopped && *lifecycle.working_graph()==before_limits,
          "limits change mutates working state");
    check(lifecycle.prepare(graph()).status==Status::exhausted && *lifecycle.working_graph()==before_limits,
          "reduced budget destroys working graph");
    lifecycle.runtime_limits({});
    check(bool(lifecycle.prepare(graph())) && bool(lifecycle.apply()) && !runtime.requested,
          "limits change loses explicit stop intent");
    check(MemoryRequirements{1,2,3}.fits(6) && !MemoryRequirements{1,2,3}.fits(5),"memory accounting");
    std::cout<<"application lifecycle passed\n";
}
