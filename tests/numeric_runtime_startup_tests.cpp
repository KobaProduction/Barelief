#include <core_app/numeric_default_graph.h>
#include <dataflow_runtime/runtime.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace signal_contracts;
using namespace domain;
using namespace dataflow_runtime;
namespace { std::size_t allocations=0; }
void* operator new(std::size_t size) {++allocations;auto* p=std::malloc(size?size:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
int failures=0;
void expect(bool valid,int line,const char* expression) {
    if(valid)return;
    std::printf("FAIL line%d: %s\n",line,expression);
    ++failures;
}
#define CHECK(x) expect(bool(x),__LINE__,#x)
constexpr std::array<unsigned,8> bins{17,31,53,79,113,157,211,283};
constexpr FrameLimits frame_limits{8,32768,262144,16384,256,8192};
NumericRuntimeProfile runtime_profile() {
    return {{32,64,8,16,256,65536,frame_limits},
        {{1048576,4*1048576,1048576,65536},0,16*1048576},
        {32768,8,262144,262144,262144},{true,true,true}};
}
struct Metrics {
    std::array<std::size_t,2048> deliveries{};
    std::array<double,2048> energy{};
    std::array<float,8> fft{},mono{};
    std::uint64_t filled=0,ordinal=0,scalar=0;
    std::size_t rows=0,view_rows=0;
    std::uint64_t last_source_sequence=0;
    unsigned bin_shift=0;
};
double numeric_energy(const NumericView& view) {
    double energy=0;
    for(std::size_t plane=0;plane<view.planes.size();++plane)
        for(std::size_t i=0;i<view.actual[0];++i){
            float value=0;
            CHECK(load_element(view,{plane,i,0},value)==Status::ok);
            CHECK(std::isfinite(value));
            energy+=double(value)*value;
        }
    return energy;
}
void observe_bins(Metrics& metrics,const NumericView& view,std::size_t id) {
    if(id!=4 && id!=15)return;
    for(std::size_t i=0;i<bins.size();++i){
        float value=0;
        CHECK(load_element(view,{0,bins[i]+metrics.bin_shift,0},value)==Status::ok);
        (id==4?metrics.fft:metrics.mono)[i]=value;
    }
}
void observe_numeric(Metrics& metrics,const NumericView& view,std::size_t id) {
    if(view.contract->dtype==DType::UInt32 || view.contract->dtype==DType::Float64)return;
    if(view.contract->dtype==DType::UInt64){
        CHECK(load_element(view,{0,0,0},metrics.scalar)==Status::ok);
        return;
    }
    metrics.energy[id]=numeric_energy(view);
    observe_bins(metrics,view,id);
}
void observe(void* user,const NumericPortFrameView& frame) {
    auto& metrics=*static_cast<Metrics*>(user);
    CHECK(frame.status==Status::ok && frame.frame.stamp.available);
    if(frame.status!=Status::ok)return;
    CHECK(validate_view(frame.frame,frame_limits)==Status::ok);
    const auto id=std::size_t(frame.identity.node);
    CHECK(id<metrics.deliveries.size());
    if(id>=metrics.deliveries.size())return;
    ++metrics.deliveries[id];
    metrics.last_source_sequence=frame.frame.stamp.source_sequence;
    if(const auto* view=std::get_if<NumericView>(&frame.frame.payload))observe_numeric(metrics,*view,id);
}
void history(void* user,const NumericHistoryRowView& event) {
    auto& m=*static_cast<Metrics*>(user);
    if(event.event!=NumericHistoryEventKind::Row)return;
    CHECK(event.capacity==400 && event.filled_count<=400);
    CHECK(event.compact.payload_bytes==50*sizeof(float));
    if(event.endpoint.direction==PortDirection::Output){++m.rows;m.filled=event.filled_count;m.ordinal=event.accepted_row_ordinal;}
    else {++m.view_rows;CHECK(event.endpoint.identity.node==8);}
}
struct Fixture {
    NumericRuntimeProfile profile=runtime_profile();
    GraphExecutor executor{profile};
    application::GraphLifecycle lifecycle{executor,executor,PreparedGraph::numeric_capabilities(profile),{profile.admission,profile.resources}};
    NumericProcessingGraph graph=numeric_default_graph();
    NumericContract source;
    std::array<float,512> samples{};
    std::array<PlaneLayout,1> planes{{{0,{4,0}}}};
    std::uint64_t sequence=0;
    Metrics metrics;
    Fixture(){CHECK((graph.execution_interval==Ratio{1,120}));graph.execution_interval={1,60};graph.nodes[0].kind="TestSignal";source=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());}
    ConfiguredProcessingNode& node(NodeId id){const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[id](const auto& n){return n.id==id;});CHECK(found!=graph.nodes.end());return *found;}
    bool apply() {
        auto prepared=lifecycle.prepare_transactional(graph);CHECK(bool(prepared));if(!prepared){
            std::printf("prepare status=%d error=%d node=%d\n",int(prepared.status),int(prepared.numeric_validation.error),int(prepared.numeric_validation.node));return false;}
        auto applied=lifecycle.apply_transactional();CHECK(bool(applied));return bool(applied);
    }
    void prepare_samples(unsigned shift) {
        for(std::size_t i=0;i<256;++i){
            double value=0;
            for(auto bin:bins)
                value+=.025*std::sin(6.2831853071795864769*double(sequence*256+i)*double(bin+shift)/4096.);
            samples[i]=float(value);
            samples[256+i]=float(value*.75);
        }
    }
    void feed_frame() {
        if(!executor.plan()){CHECK(executor.plan()!=nullptr);return;}
        FrameStamp stamp;
        stamp.sequence=++sequence;
        stamp.source_sequence=sequence;
        stamp.producer_epoch=1;
        stamp.timestamp_ticks=std::int64_t((sequence-1)*256);
        stamp.stream_id="startup.synthetic";
        DataFrameView frame{stamp,NumericView{&source,std::as_bytes(std::span(samples)),planes,{256,1},{},{}}};
        const auto before=allocations;
        CHECK(executor.process_numeric_ports(1,frame,{1,1},{observe,observe,history,&metrics})==Status::ok);
        CHECK(allocations==before);
    }
    void feed(std::size_t blocks,unsigned shift=0) {
        metrics.bin_shift=shift;
        for(std::size_t block=0;block<blocks;++block){
            prepare_samples(shift);
            feed_frame();
        }
    }
    void zero_mel() const {CHECK(metrics.energy[6]==0 && metrics.energy[11]==0);}
    double row_energy(const NumericView& view,std::size_t row,bool must_be_zero) const {
        double energy=0;
        for(std::size_t band=0;band<50;++band){
            float value=1;
            CHECK(load_element(view,{0,row,band},value)==Status::ok);
            if(must_be_zero)CHECK(value==0);
            energy+=double(value)*value;
        }
        return energy;
    }
    void history_row(bool zero=true) const {
        if(!executor.plan()){CHECK(executor.plan()!=nullptr);return;}
        DataFrameView snapshot;
        std::uint64_t filled=0;
        const auto status=executor.plan()->history(7,0,*executor.context(),snapshot,&filled);
        CHECK(status==Status::ok);if(status!=Status::ok)return;
        const auto& view=std::get<NumericView>(snapshot.payload);
        CHECK(view.actual[0]==view.contract->shape.extents[0].maximum&&filled>1&&filled<=view.actual[0]);
        const auto newest=row_energy(view,view.actual[0]-1,zero);
        const auto first=row_energy(view,view.actual[0]-std::size_t(filled),false);
        CHECK(first>0);
        if(!zero)CHECK(newest>0);
    }
};
void assert_default_spectrum(const Fixture& f) {
    const float expected=float(.025*(4096.-1)/(4.*4096.));
    const float channel_gain=1.f;
    for(std::size_t i=0;i<bins.size();++i){
        CHECK(std::abs(f.metrics.fft[i]-expected)<.000002);
        const double bark=7*std::asinh((double(bins[i])*48000/4096)/650.);
        CHECK(std::abs(f.metrics.mono[i]-expected*channel_gain*bark)<.000002*std::max(1.,bark));
    }
}
void assert_default_delivery(Fixture& f) {
    CHECK(f.metrics.deliveries[1]==80 && f.metrics.deliveries[9]==80);
    CHECK(f.metrics.rows==25 && f.metrics.view_rows==25 && f.metrics.filled==25 && f.metrics.scalar==25);
    CHECK(f.metrics.energy[9]>0 && f.metrics.energy[10]>0 && f.metrics.energy[11]>0);
    assert_default_spectrum(f);
    const float expected=float(.025*(4096.-1)/(4.*4096.));
    std::printf("STARTUP PCM=%zu windows=%zu history=%zu count=%llu fft_bin17=%.9g expected=%.9g mel_energy=%.9g\n",
        f.metrics.deliveries[9],f.metrics.deliveries[4],f.metrics.rows,(unsigned long long)f.metrics.scalar,
        f.metrics.fft[0],expected,f.metrics.energy[11]);
    const auto first=f.metrics.energy[11];f.feed(80,3);CHECK(f.metrics.energy[11]!=first);
    CHECK(f.metrics.rows==51 && f.metrics.filled==51 && f.metrics.scalar==51);
}
void assert_shutdown_and_restart(Fixture& f) {
    CHECK(f.lifecycle.shutdown_transactional()==Status::ok);const auto delivered=f.metrics.deliveries;
    FrameStamp stamp;stamp.sequence=f.sequence+1;stamp.producer_epoch=1;
    CHECK(f.executor.process_numeric_ports(1,{stamp,NumericView{&f.source,std::as_bytes(std::span(f.samples)),f.planes,{256,1},{},{}}},{},{observe,observe,history,&f.metrics})==Status::would_block);
    CHECK(f.metrics.deliveries==delivered);
    CHECK(f.apply() && f.executor.running());f.feed(4);
    CHECK(f.metrics.filled==1 && f.metrics.ordinal==1 && f.metrics.scalar==1);
    const auto* working=f.executor.plan();auto malformed=f.graph;malformed.nodes[3].kind="Unknown";
    CHECK(!f.lifecycle.prepare_transactional(malformed));CHECK(f.executor.plan()==working && f.executor.running());f.feed(4);
    CHECK(f.metrics.filled==2);
    std::printf("LIFECYCLE owner shutdown=no delivery; re-admission=running; fresh count=1; rejected prepare retains working\n");
}
void startup_and_lifecycle() {
    Fixture fixture;
    if(!fixture.apply())return;
    fixture.feed(80);
    assert_default_delivery(fixture);
    assert_shutdown_and_restart(fixture);
}
void assert_disabled_window(const Fixture& f) {
    // Shape-preserving WindowFunction bypasses DSP but retains amplitude.
    CHECK(f.metrics.energy[2]>0 && f.metrics.energy[3]==4096 && f.metrics.energy[1001]==f.metrics.energy[2]);
    CHECK(f.metrics.energy[6]>0 && f.metrics.energy[11]>0);
    for(const auto magnitude:f.metrics.fft)CHECK(std::abs(magnitude-.025f/2)<.000002);
}
void switch_core_node(Fixture& f,NodeId id) {
    f.node(id).enabled=false;
    const auto count=f.metrics.filled;
    CHECK(f.apply());
    f.feed(24);
    CHECK(f.metrics.filled>count);
    if(id==3)assert_disabled_window(f);
    else if(id==15)CHECK(f.metrics.energy[6]>0 && f.metrics.energy[11]>0);
    else if(id!=7)f.zero_mel();
    f.history_row(id!=3 && id!=15);
    f.node(id).enabled=true;
    CHECK(f.apply());
    f.feed(24);
    CHECK(f.metrics.energy[11]>0);
    std::printf("BYPASS node%d %s; history retained; reenable positive Mel\n",int(id),id==3?"identity":"typed silence");
}
void switch_scalar_and_endpoints(Fixture& f) {
    f.node(12).enabled=false;CHECK(f.apply());f.feed(4);CHECK(f.metrics.scalar==0 && f.metrics.filled>0);
    f.node(12).enabled=true;CHECK(f.apply());f.feed(4);CHECK(f.metrics.scalar==f.metrics.filled);
    for(NodeId id:{9,10,11}){
        const auto filled=f.metrics.filled;f.node(id).enabled=false;CHECK(f.apply());f.feed(4);
        CHECK(f.metrics.energy[std::size_t(id)]==0 && f.metrics.energy[6]>0 && f.metrics.filled>filled);
        f.node(id).enabled=true;CHECK(f.apply());f.feed(4);CHECK(f.metrics.energy[std::size_t(id)]>0);
    }
}
void disconnect_history(Fixture& f) {
    const auto filled=f.metrics.filled;
    CHECK(f.lifecycle.disconnect(7,0));f.feed(24);f.history_row();CHECK(f.metrics.energy[11]>0 && f.metrics.filled>filled);
    CHECK(f.lifecycle.connect({6,7,0,0}));f.feed(24);CHECK(f.metrics.energy[11]>0);
}
void switches_and_disconnect() {
    Fixture f;if(!f.apply())return;f.feed(24);
    for(NodeId id:{1,2,3,4,15,6,7})switch_core_node(f,id);
    switch_scalar_and_endpoints(f);
    disconnect_history(f);
}
}
int main(){startup_and_lifecycle();switches_and_disconnect();std::printf("startup runtime failures=%d\n",failures);return failures?1:0;}
