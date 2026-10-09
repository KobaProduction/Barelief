#include <core_app/numeric_default_graph.h>
#include <dataflow_runtime/runtime.h>
#include <wled_protocol/wled_packet.h>
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
#define CHECK(x) do {if(!(x)){std::printf("FAIL line%d: %s\n",__LINE__,#x);++failures;}} while(false)
constexpr std::array<unsigned,8> bins{17,31,53,79,113,157,211,283};
constexpr FrameLimits frame_limits{8,32768,262144,16384,256,8192};
NumericRuntimeProfile runtime_profile() {
    return {{32,64,8,16,256,65536,frame_limits},
        {{1048576,1048576,1048576,65536},0,8388608},
        {32768,8,262144,262144,262144},{true,true,true}};
}
struct Metrics {
    std::array<std::size_t,16> deliveries{};
    std::array<double,16> energy{};
    std::array<float,8> fft{},mono{};
    std::uint64_t filled=0,ordinal=0,scalar=0;
    std::size_t rows=0,view_rows=0,packets=0;
    wled_protocol::DecodedPacket packet{};
    std::uint64_t last_source_sequence=0;
    unsigned bin_shift=0;
};
void observe(void* user,const NumericPortFrameView& frame) {
    auto& m=*static_cast<Metrics*>(user);
    CHECK(frame.status==Status::ok && frame.frame.stamp.available);
    if(frame.status!=Status::ok)return;
    CHECK(validate_view(frame.frame,frame_limits)==Status::ok);
    const auto id=std::size_t(frame.identity.node);
    CHECK(id<m.deliveries.size());if(id>=m.deliveries.size())return;
    ++m.deliveries[id];m.last_source_sequence=frame.frame.stamp.source_sequence;
    if(const auto* view=std::get_if<NumericView>(&frame.frame.payload)) {
        if(view->contract->dtype==DType::UInt64){CHECK(load_element(*view,{0,0,0},m.scalar)==Status::ok);return;}
        double energy=0;
        for(std::size_t plane=0;plane<view->planes.size();++plane)for(std::size_t i=0;i<view->actual[0];++i){
            float value=0;CHECK(load_element(*view,{plane,i,0},value)==Status::ok);CHECK(std::isfinite(value));energy+=double(value)*value;
        }
        m.energy[id]=energy;
        if(id==4 || id==5)for(std::size_t i=0;i<bins.size();++i){float value=0;
            CHECK(load_element(*view,{0,bins[i]+m.bin_shift,0},value)==Status::ok);
            (id==4?m.fft:m.mono)[i]=value;
        }
    }else {
        const auto& packet=std::get<StructuredView>(frame.frame.payload);
        CHECK(packet.bytes.size()==wled_protocol::packet_size);
        CHECK(wled_protocol::decode({reinterpret_cast<const std::uint8_t*>(packet.bytes.data()),packet.bytes.size()},
            m.packet,{50,wled_protocol::packet_size}).status==wled_protocol::Status::Ok);
        if(id==14)++m.packets;
    }
}
void history(void* user,const NumericHistoryRowView& event) {
    auto& m=*static_cast<Metrics*>(user);
    if(event.event!=NumericHistoryEventKind::Row)return;
    CHECK(event.capacity==400 && event.filled_count<=400);
    CHECK(event.compact.payload_bytes==50*sizeof(float));
    if(event.endpoint.direction==PortDirection::Output){++m.rows;m.filled=event.filled_count;m.ordinal=event.accepted_row_ordinal;}
    else {++m.view_rows;CHECK(event.endpoint.identity.node==8);}
}
void add_wled(NumericProcessingGraph& graph) {
    ConfiguredProcessingNode encoder;encoder.id=13;encoder.kind="WLEDEncode";encoder.parameters=TransformProcessingParameters{};
    const auto mel=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==6;});CHECK(mel!=graph.nodes.end());
    auto input=mel->outputs[0];input.direction=PortDirection::Input;encoder.inputs.push_back(input);
    StructuredContract wire;wire.schema="wled-audio-sync";wire.version=1;wire.encoding="audio-sync-44-le";
    wire.payload_bytes={44,44};
    OwnedProcessingContract owned;CHECK(own_processing_contract(DataContract{wire},frame_limits,16384,owned)==Status::ok);
    encoder.outputs.push_back({0,PortDirection::Output,owned});graph.nodes.push_back(std::move(encoder));
    ConfiguredProcessingNode sink;sink.id=14;sink.kind="WLEDOutput";sink.parameters=EndpointProcessingParameters{};
    sink.inputs.push_back({0,PortDirection::Input,owned});graph.nodes.push_back(std::move(sink));
    graph.edges.push_back({6,13,0,0});graph.edges.push_back({13,14,0,0});
}
struct Fixture {
    NumericRuntimeProfile profile=runtime_profile();
    GraphExecutor executor{profile};
    application::GraphLifecycle lifecycle{executor,executor,PreparedGraph::numeric_capabilities(profile),{profile.admission,profile.resources}};
    NumericProcessingGraph graph=numeric_default_graph();
    NumericContract source;
    std::array<float,512> samples{};
    std::array<PlaneLayout,2> planes{{{0,{4,0}},{256*sizeof(float),{4,0}}}};
    std::uint64_t sequence=0;
    Metrics metrics;
    Fixture(){graph.nodes[0].kind="TestSignal";source=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());}
    ConfiguredProcessingNode& node(NodeId id){const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[id](const auto& n){return n.id==id;});CHECK(found!=graph.nodes.end());return *found;}
    bool apply() {
        auto prepared=lifecycle.prepare_transactional(graph);CHECK(bool(prepared));if(!prepared){
            std::printf("prepare status=%d error=%d node=%d\n",int(prepared.status),int(prepared.numeric_validation.error),int(prepared.numeric_validation.node));return false;}
        auto applied=lifecycle.apply_transactional();CHECK(bool(applied));return bool(applied);
    }
    void feed(std::size_t blocks,unsigned shift=0) {
        metrics.bin_shift=shift;
        for(std::size_t block=0;block<blocks;++block){
            for(std::size_t i=0;i<256;++i){double value=0;
                for(auto bin:bins)value+=.025*std::sin(6.2831853071795864769*double(sequence*256+i)*double(bin+shift)/4096.);
                samples[i]=float(value);samples[256+i]=float(value*.75);
            }
            FrameStamp stamp;stamp.sequence=++sequence;stamp.source_sequence=sequence;stamp.producer_epoch=1;
            stamp.timestamp_ticks=std::int64_t((sequence-1)*256);stamp.stream_id="startup.synthetic";
            DataFrameView frame{stamp,NumericView{&source,std::as_bytes(std::span(samples)),planes,{256,1},{},{}}};
            auto before=allocations;
            CHECK(executor.process_numeric_ports(1,frame,{1,1},{observe,observe,history,&metrics})==Status::ok);
            CHECK(allocations==before);
        }
    }
    void zero_mel() const {CHECK(metrics.energy[6]==0 && metrics.energy[11]==0);}
    void history_row(bool zero=true) const {
        DataFrameView snapshot;std::uint64_t filled=0;CHECK(executor.plan()->history(7,0,*executor.context(),snapshot,&filled)==Status::ok);
        const auto& view=std::get<NumericView>(snapshot.payload);CHECK(view.actual[0]==view.contract->shape.extents[0].maximum&&filled>1&&filled<=view.actual[0]);float value=1;double first_energy=0,newest_energy=0;
        for(std::size_t band=0;band<50;++band){CHECK(load_element(view,{0,view.actual[0]-1,band},value)==Status::ok);newest_energy+=double(value)*value;if(zero)CHECK(value==0);
            CHECK(load_element(view,{0,view.actual[0]-std::size_t(filled),band},value)==Status::ok);first_energy+=double(value)*value;}
        CHECK(first_energy>0);
        if(!zero)CHECK(newest_energy>0);
    }
};
void startup_and_lifecycle() {
    Fixture f;CHECK(f.apply());f.feed(80);
    CHECK(f.metrics.deliveries[1]==80 && f.metrics.deliveries[9]==80);
    CHECK(f.metrics.rows==25 && f.metrics.view_rows==25 && f.metrics.filled==25 && f.metrics.scalar==25);
    CHECK(f.metrics.energy[9]>0 && f.metrics.energy[10]>0 && f.metrics.energy[11]>0);
    const float expected=float(.025*(4096.-1)/(4.*4096.));
    const float stereo_rms=std::sqrt((1.f+.75f*.75f)/2.f);
    for(std::size_t i=0;i<bins.size();++i){CHECK(std::abs(f.metrics.fft[i]-expected)<.000002);
        // Independent closed-form DFT above, followed by the declared Bark
        // coefficient and stereo RMS of the current default graph.
        const double bark=7*std::asinh((double(bins[i])*48000/4096)/650.);
        CHECK(std::abs(f.metrics.mono[i]-expected*stereo_rms*bark)<.000002*std::max(1.,bark));}
    std::printf("STARTUP PCM=%zu windows=%zu history=%zu count=%llu fft_bin17=%.9g expected=%.9g mel_energy=%.9g\n",
        f.metrics.deliveries[9],f.metrics.deliveries[4],f.metrics.rows,(unsigned long long)f.metrics.scalar,
        f.metrics.fft[0],expected,f.metrics.energy[11]);
    const auto first=f.metrics.energy[11];f.feed(80,3);CHECK(f.metrics.energy[11]!=first);
    CHECK(f.metrics.rows==51 && f.metrics.filled==51 && f.metrics.scalar==51);
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
void switches_and_disconnect() {
    Fixture f;CHECK(f.apply());f.feed(24);
    for(NodeId id:{1,2,3,4,5,6,7}){
        f.node(id).enabled=false;const auto count=f.metrics.filled;CHECK(f.apply());f.feed(24);
        CHECK(f.metrics.filled>count);
        if(id==3){
            // A disabled shape-preserving WindowFunction bypasses its DSP:
            // the unwindowed buffer passes through unchanged, including amplitude.
            CHECK(f.metrics.energy[2]>0 && f.metrics.energy[3]==f.metrics.energy[2]);
            CHECK(f.metrics.energy[6]>0 && f.metrics.energy[11]>0);
            for(const auto magnitude:f.metrics.fft)CHECK(std::abs(magnitude-.025f/2)<.000002);
        }else if(id!=7)f.zero_mel();
        f.history_row(id!=3);
        f.node(id).enabled=true;CHECK(f.apply());f.feed(24);CHECK(f.metrics.energy[11]>0);
        std::printf("BYPASS node%d %s; history retained; reenable positive Mel\n",int(id),id==3?"identity":"typed silence");
    }
    f.node(12).enabled=false;CHECK(f.apply());f.feed(4);CHECK(f.metrics.scalar==0 && f.metrics.filled>0);
    f.node(12).enabled=true;CHECK(f.apply());f.feed(4);CHECK(f.metrics.scalar==f.metrics.filled);
    for(NodeId id:{9,10,11}){
        const auto filled=f.metrics.filled;f.node(id).enabled=false;CHECK(f.apply());f.feed(4);
        CHECK(f.metrics.energy[std::size_t(id)]==0 && f.metrics.energy[6]>0 && f.metrics.filled>filled);
        f.node(id).enabled=true;CHECK(f.apply());f.feed(4);CHECK(f.metrics.energy[std::size_t(id)]>0);
    }
    const auto filled=f.metrics.filled;
    CHECK(f.lifecycle.disconnect(7,0));f.feed(24);f.history_row();CHECK(f.metrics.energy[11]>0 && f.metrics.filled>filled);
    CHECK(f.lifecycle.connect({6,7,0,0}));f.feed(24);CHECK(f.metrics.energy[11]>0);
}
void wled() {
    Fixture f;add_wled(f.graph);CHECK(f.apply());f.feed(40);
    CHECK(f.metrics.packets>0 && f.metrics.packet.magnitude>0 && f.metrics.packet.peak_hz>0);
    CHECK(f.metrics.deliveries[13]==f.metrics.deliveries[14]);
    std::printf("WLED packets=%zu bytes=44 magnitude=%.9g peak_hz=%.9g\n",f.metrics.packets,f.metrics.packet.magnitude,f.metrics.packet.peak_hz);
    f.node(13).enabled=false;CHECK(f.apply());f.feed(4);CHECK(f.metrics.packet.magnitude==0 && f.metrics.packet.peak_hz==0);
    CHECK(std::all_of(f.metrics.packet.bins.begin(),f.metrics.packet.bins.end(),[](auto value){return value==0;}));
    f.node(13).enabled=true;CHECK(f.apply());f.feed(4);CHECK(f.metrics.packet.magnitude>0);
}
}
int main(){startup_and_lifecycle();switches_and_disconnect();wled();std::printf("startup runtime failures=%d\n",failures);return failures?1:0;}
