#include <core_app/numeric_default_graph.h>
#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>

namespace {
using namespace domain;using namespace signal_contracts;using namespace dataflow_runtime;
std::size_t allocations=0;
void check(bool value,const char* why) {if(!value){std::fprintf(stderr,"FAIL fixed window: %s\n",why);std::exit(1);}}
NumericRuntimeProfile profile() {
    constexpr FrameLimits f{1,65536,1048576,65536,256,65536};
    return {{32,64,8,16,256,131072,f},{{4194304,4194304,4194304,131072},0,16777216},
        {65536,1,1048576,1048576,1048576},{true,true,true}};
}
NumericProcessingGraph graph() {
    const auto example=numeric_basic_signal_graph(44100,2048);
    const auto found=std::find_if(example.nodes.begin(),example.nodes.end(),[](const auto& n){return n.id==2;});
    check(found!=example.nodes.end(),"basic preset owns its accumulator");
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.parameters=EndpointProcessingParameters{};
    source.outputs={{0,PortDirection::Output,found->inputs[0].contract}};
    NumericProcessingGraph g;g.graph_id="fixed-window";g.execution_interval=example.execution_interval;
    g.nodes={source,*found};g.edges={{1,2,0,0}};return g;
}
std::array<float,4096> signal() {
    std::array<float,4096> values{};
    for(std::size_t n=0;n<values.size();++n)for(unsigned tone=1;tone<=5;++tone){
        const float time=float(n)/44100.f;
        values[n]+=.03f*std::sin(2.f*std::numbers::pi_v<float>*(float(tone)*220.f*time+5.f*time*time));
    }
    return values;
}
struct Capture {
    std::array<float,2048> latest{};
    std::int64_t start=0;
    std::size_t count=0;
    static void receive(void* user,const NumericPortFrameView& event) {
        if(event.identity.node!=2 || event.direction!=PortDirection::Output || !event.frame.stamp.available)return;
        auto& self=*static_cast<Capture*>(user);
        const auto& v=std::get<NumericView>(event.frame.payload);const auto& stamp=event.frame.stamp;
        check(v.actual[0]==2048 && v.contract->clock.sample_rate==44100,"N and incoming clock metadata preserved");
        check(stamp.has_sample_interval && stamp.sample_count==2048,"retained source interval");
        self.start=stamp.sample_begin;
        for(std::size_t i=0;i<2048;++i)check(load_element(v,{0,i,0},self.latest[i])==Status::ok,"read retained value");
        ++self.count;
    }
};
struct Running {
    NumericProcessingGraph definition=graph();
    std::shared_ptr<const PreparedGraph> plan;
    std::unique_ptr<RuntimeContext> context;
    Capture capture;
    NumericContract input=std::get<NumericContract>(definition.nodes[0].outputs[0].contract.view());
    std::uint64_t sequence=0;
    Running() {check(PreparedGraph::prepare_numeric(definition,profile(),plan,context)==Status::ok,"prepare accumulation in offline fixture");}
    void send(std::span<const float> values,std::size_t offset,bool discontinuity=false) {
        const std::array<PlaneLayout,1> planes{{{0,{4,0}}}};
        std::array<float,512> packet{};
        std::array<double,512> coordinates{};
        check(values.size()<=coordinates.size(),"bounded source coordinates");
        std::copy(values.begin(),values.end(),packet.begin());
        for(std::size_t i=0;i<values.size();++i)coordinates[i]=double(offset+i)/44100.;
        FrameStamp stamp;stamp.sequence=++sequence;stamp.timestamp_ticks=std::int64_t(offset);stamp.sample_begin=std::int64_t(offset);
        stamp.has_sample_interval=true;stamp.sample_count=values.size();stamp.discontinuity=discontinuity;
        NumericView view{&input,std::as_bytes(std::span(packet)),planes,{values.size(),1}};
        if(input.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE)view.coordinates.axes[0]=std::as_bytes(std::span(coordinates).first(values.size()));
        const DataFrameView frame{stamp,view};
        const auto before=allocations;
        const auto status=plan->process_numeric_ports(1,frame,*context,{}, {Capture::receive,nullptr,nullptr,&capture});
        if(status!=Status::ok || allocations!=before)std::fprintf(stderr,"offset=%zu size=%zu status=%u allocations=%zu\n",offset,values.size(),unsigned(status),allocations-before);
        check(status==Status::ok && allocations==before,"bounded streaming framing has no hot allocations");
    }
    void feed(std::span<const float> values,unsigned partition,std::size_t origin=0) {
        std::size_t offset=0;
        while(offset<values.size()){
            const auto chunk=std::min({values.size()-offset,std::size_t(partition),std::size_t(input.shape.extents[0].maximum)});
            send(values.subspan(offset,chunk),origin+offset);offset+=chunk;
        }
    }
};
void partition_and_warmup() {
    const auto samples=signal();Running a,b;
    a.feed(samples,17);b.feed(samples,137);
    check(a.capture.count==(4096+16)/17 && b.capture.count==(4096+136)/137,"one result per packet, without warmup or hop");
    check(a.capture.start==2048 && b.capture.start==2048,"last N input positions retained");
    check(a.capture.latest==b.capture.latest,"final accumulated values independent of packet partitions");
    check(std::equal(a.capture.latest.begin(),a.capture.latest.end(),samples.begin()+2048),"complete retained tail");
}
void discontinuity() {
    const auto samples=signal();Running running;
    running.feed(std::span(samples).first(1000),17);check(running.capture.count>0,"partial history is available immediately");
    running.send(std::span(samples).subspan(1000,1),1000,true);
    running.feed(std::span(samples).subspan(1001,2047),137,1001);
    check(running.capture.start==1000,"runtime discontinuity resets incompatible prior history");
    check(std::equal(running.capture.latest.begin(),running.capture.latest.end(),samples.begin()+1000),"new source values retained after reset");
}
void cadence_and_reprepare() {
    const auto samples=signal();Running running;running.feed(std::span(samples).first(1000),17);
    auto changed=running.definition;changed.execution_interval={1,120};
    std::shared_ptr<const PreparedGraph> next;std::unique_ptr<RuntimeContext> context;
    check(PreparedGraph::prepare_numeric(changed,profile(),next,context,running.plan.get(),running.context.get())==Status::ok,"reprepare with a different GUI analysis cadence");
    running.plan=std::move(next);running.context=std::move(context);
    running.feed(std::span(samples).subspan(1000,37),37,1000);
    check(std::all_of(running.capture.latest.begin(),running.capture.latest.begin()+1011,[](float value){return value==0;}),"zero prefix preserved across cadence change");
    check(std::equal(running.capture.latest.begin()+1011,running.capture.latest.end(),samples.begin()),"partial retained history survives unrelated cadence change");
}
}
void* operator new(std::size_t n){++allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
int main(){partition_and_warmup();discontinuity();cadence_and_reprepare();std::puts("offline accumulator: partitions, immediate partial values, reset and cadence independence PASS");}
