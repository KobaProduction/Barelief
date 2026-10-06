#include <embedded_support/session.h>
#include <embedded_support/profile.h>
#include <embedded_support/synthetic_pipeline.h>
#include <dsp_kernels/dsp.h>
#include <cassert>
#include <algorithm>
#include <deque>
#include <vector>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <new>
#include <cstring>
#include <cstdio>
static thread_local bool measuring_allocations=false;
static thread_local std::size_t allocation_count=0;
void *operator new(std::size_t size) {if(measuring_allocations)++allocation_count;if(auto *p=std::malloc(size))return p;std::abort();}
void operator delete(void *p) noexcept {std::free(p);}
void operator delete(void *p,std::size_t) noexcept {std::free(p);}
using signal_contracts::Status;
using signal_contracts::FrameType;
struct Transport : bdsp_protocol::ByteTransport {
    std::deque<std::uint8_t> bytes; Transport *peer=nullptr;
    std::size_t quantum=1; bool connected=true, stalled=false;
    bdsp_protocol::TransportState state() const noexcept override {return connected?bdsp_protocol::TransportState::connected:bdsp_protocol::TransportState::disconnected;}
    bdsp_protocol::IoResult read(std::span<std::uint8_t> out) noexcept override {
        auto n=std::min({bytes.size(),out.size(),quantum});
        for(std::size_t i=0;i<n;++i){out[i]=bytes.front();bytes.pop_front();}return {n?Status::ok:Status::would_block,n};
    }
    bdsp_protocol::IoResult write(std::span<const std::uint8_t> in) noexcept override {
        auto n=stalled?0:std::min(in.size(),quantum);
        for(auto byte:in.first(n))peer->bytes.push_back(byte);return {n?Status::ok:Status::would_block,n};
    }
};
struct Storage {
    std::array<std::uint8_t,4608> in{},out{};
    std::array<std::uint8_t,256> read{};
    std::array<std::uint8_t,2304> tx{},assembly{};
    std::array<std::uint8_t,2312> fragment{};
    std::array<float,512> samples{};std::array<char,256> id{};
    embedded_support::SessionBuffers buffers(){return {in,out,read,tx,fragment,assembly,samples,id};}
};
template<class T> void transfer(embedded_support::Session &p, embedded_support::Session &s, T &a, T &b, int n=10000){
    for(int i=0;i<n;++i){assert(p.poll(a,1)==Status::ok);assert(s.poll(b,1)==Status::ok);}
}
struct FixedTransport : bdsp_protocol::ByteTransport {
    std::array<std::uint8_t,4096> bytes{};FixedTransport *peer=nullptr;
    std::size_t head=0,count=0,quantum=1;
    bdsp_protocol::TransportState state() const noexcept override{return bdsp_protocol::TransportState::connected;}
    bdsp_protocol::IoResult read(std::span<std::uint8_t> out) noexcept override {
        const auto n=std::min({count,out.size(),quantum});
        for(std::size_t i=0;i<n;++i){out[i]=bytes[head];head=(head+1)%bytes.size();}count-=n;
        return {n?Status::ok:Status::would_block,n};
    }
    bdsp_protocol::IoResult write(std::span<const std::uint8_t> in) noexcept override {
        const auto n=std::min({in.size(),quantum,peer->bytes.size()-peer->count});
        for(auto value:in.first(n)){peer->bytes[(peer->head+peer->count)%peer->bytes.size()]=value;++peer->count;}
        return {n?Status::ok:Status::would_block,n};
    }
};
void producer_fences(bool replay_only=false) {
    Storage pb,sb;FixedTransport a,b;a.peer=&b;b.peer=&a;
    embedded_support::Session p{embedded_support::Role::publisher,FrameType::PCM,"raw",pb.buffers()};
    embedded_support::Session s{embedded_support::Role::subscriber,FrameType::PCM,"raw",sb.buffers()};
    assert(p.begin(0,123)==Status::ok && s.begin(0,0)==Status::ok);transfer(p,s,a,b,1000);
    struct Received {int data=0,markers=0;std::uint64_t producer=0,generation=0;bool gap=false;} received;
    s.set_frame_handler([](void *ctx,const signal_contracts::FrameView &frame) {
        assert(frame.metadata.timestamp_ns==-456 && frame.metadata.source_sequence==91 && frame.metadata.stream_id=="raw");
        auto &r=*static_cast<Received *>(ctx);if(frame.metadata.available)++r.data;else ++r.markers;
        r.producer=frame.metadata.producer_epoch;r.generation=frame.metadata.generation;r.gap=frame.metadata.discontinuity;
    },&received);
    std::array<float,4> pcm{1,2,3,4};signal_contracts::FrameView frame;
    frame.metadata.stream_id="raw";frame.metadata.sample_rate=8000;frame.metadata.producer_epoch=100;
    frame.metadata.generation=1;frame.metadata.sequence=1;frame.left=pcm;frame.right=pcm;
    frame.metadata.timestamp_ns=-456;frame.metadata.source_sequence=91;
    auto send=[&](const signal_contracts::FrameView &value){assert(p.publish(value)==Status::ok);transfer(p,s,a,b,1000);};
    allocation_count=0;measuring_allocations=true;send(frame);assert(received.data==1);
    if(!replay_only) {
        auto marker=frame;marker.metadata.available=false;marker.metadata.sequence=0;marker.metadata.generation=2;marker.left={};marker.right={};
        send(marker);assert(received.markers==1);
        frame.metadata.sequence=2;send(frame);assert(received.data==1); // older generation after accepted marker
        frame.metadata.generation=2;frame.metadata.sequence=1;send(frame);assert(received.data==2);
        marker.metadata.generation=1;send(marker);assert(received.markers==1); // stale control cannot clear current data
        marker.metadata.producer_epoch=0;marker.metadata.generation=0;send(marker);assert(received.markers==2);
        send(frame);assert(received.data==2);frame.metadata.sequence=2;send(frame);assert(received.data==3 && received.gap);
    }
    const auto before=received.data;
    frame.metadata.producer_epoch=101;frame.metadata.generation=1;frame.metadata.sequence=3;send(frame);assert(received.data==before+1);
    frame.metadata.producer_epoch=100;frame.metadata.sequence=4;frame.metadata.discontinuity=true;send(frame);
    assert(received.data==before+1); // retired identity must not become a new producer again
    auto old_marker=frame;old_marker.metadata.available=false;old_marker.metadata.sequence=0;old_marker.left={};old_marker.right={};
    const auto controls=received.markers;send(old_marker);assert(received.markers==controls);
    frame.metadata.producer_epoch=102;frame.metadata.sequence=1;frame.metadata.discontinuity=false;send(frame);assert(received.data==before+2);
    // Retention is explicitly bounded to the same sixteen identities as runtime.
    for(std::uint64_t producer=200;producer<217;++producer){frame.metadata.producer_epoch=producer;send(frame);}
    const auto latest=received.data;frame.metadata.producer_epoch=215;frame.metadata.sequence=2;send(frame);assert(received.data==latest);
    frame.metadata.producer_epoch=100;frame.metadata.sequence=1;send(frame);assert(received.data==latest+1); // evicted, beyond retention window
    measuring_allocations=false;assert(allocation_count==0);
}
void sessions(std::size_t quantum) {
    Storage pb,sb; Transport a,b;a.peer=&b;b.peer=&a;a.quantum=b.quantum=quantum;
    embedded_support::Session p{embedded_support::Role::publisher,FrameType::PCM,"raw",pb.buffers()};
    embedded_support::Session s{embedded_support::Role::subscriber,FrameType::PCM,"raw",sb.buffers()};
    assert(p.begin(0,123)==Status::ok);assert(s.begin(0,0)==Status::ok);
    transfer(p,s,a,b,1000);assert(p.state()==embedded_support::SessionState::ready);assert(s.state()==embedded_support::SessionState::ready);
    std::array<float,256> left{},right{};
    for(std::size_t i=0;i<left.size();++i){left[i]=std::sin(float(i));right[i]=-left[i];}
    signal_contracts::FrameView f;f.type=FrameType::PCM;f.metadata.stream_id="raw";f.metadata.sequence=1;f.metadata.sample_rate=8000;f.left=left;f.right=right;
    struct Received {int count=0;bool gap=false;} received;
    s.set_frame_handler([](void *ctx,const signal_contracts::FrameView &x){auto &r=*static_cast<Received *>(ctx);++r.count;r.gap=x.metadata.discontinuity;assert(x.metadata.epoch==123 || x.metadata.epoch==124);assert(x.left.size()==256);assert(x.left[1]==-x.right[1]);},&received);
    assert(p.publish(f)==Status::ok);assert(p.publish(f)==Status::overflow);
    transfer(p,s,a,b);assert(received.count==1 && received.gap);
    f.metadata.sequence=2;assert(p.publish(f)==Status::ok);transfer(p,s,a,b);assert(received.count==2 && received.gap);
    f.metadata.sequence=3;assert(p.publish(f)==Status::ok);a.stalled=true;
    auto marker=f;marker.metadata.available=false;marker.metadata.sequence=0;marker.left={};marker.right={};
    assert(p.publish(marker)==Status::overflow);assert(p.state()==embedded_support::SessionState::failed);assert(p.pending_bytes()==0);
    // Host must reset its transport too; reconnect is explicit, never replays old bytes.
    a.bytes.clear();b.bytes.clear();a.stalled=false;s.stop();
    assert(p.begin(0,124)==Status::ok);assert(s.begin(0,0)==Status::ok);transfer(p,s,a,b,1000);
    f.metadata.sequence=1;assert(p.publish(f)==Status::ok);transfer(p,s,a,b);assert(received.count==3 && received.gap);
}
void rejection_and_marker() {
    Storage pb,sb;Transport a,b;a.peer=&b;b.peer=&a;a.quantum=b.quantum=127;
    embedded_support::Session p{embedded_support::Role::publisher,FrameType::PCM,"raw",pb.buffers()};
    embedded_support::Session s{embedded_support::Role::subscriber,FrameType::Mel,"raw",sb.buffers()};
    assert(p.begin(0,1)==Status::ok);assert(s.begin(0,0)==Status::ok);
    for(int i=0;i<10;++i){p.poll(a,1);s.poll(b,1);}
    assert(p.state()==embedded_support::SessionState::failed && s.state()==embedded_support::SessionState::failed);
    assert(s.last_status()==Status::incompatible_contract); // ID14 received before close.
    assert(p.begin(0,1)==Status::ok);assert(p.poll(a,5000)==Status::unavailable);
    a.bytes.clear();b.bytes.clear();
    embedded_support::Session r{embedded_support::Role::subscriber,FrameType::PCM,"raw",sb.buffers()};
    assert(p.begin(0,1)==Status::ok);assert(r.begin(0,0)==Status::ok);transfer(p,r,a,b,10);
    int markers=0;
    r.set_frame_handler([](void *ctx,const signal_contracts::FrameView &f){assert(!f.metadata.available && f.metadata.sequence==0 && f.left.empty());++*static_cast<int *>(ctx);},&markers);
    signal_contracts::FrameView f;f.metadata.stream_id="raw";f.metadata.available=false;
    assert(p.publish(f)==Status::ok);transfer(p,r,a,b,100);assert(markers==1);
    assert(p.begin(0,0)==Status::invalid_argument);
    embedded_support::Session invalid{embedded_support::Role::subscriber,FrameType::PCM,std::string_view("\xc0\xaf",2),sb.buffers()};
    assert(invalid.begin(0,0)==Status::invalid_argument);
}
void multiple_fragments() {
    std::vector<std::uint8_t> pin(61000),pout(122000),ptx(100000),pfrag(60008),pread(256);
    std::vector<std::uint8_t> sin(61000),sout(122000),assembly(100000),sread(256);
    std::vector<float> samples(20000),values(16000,.25f);std::array<char,256> id{};
    embedded_support::Session p{embedded_support::Role::publisher,FrameType::Spectrum,"spectrum",{pin,pout,pread,ptx,pfrag,{},{},{}}};
    embedded_support::Session s{embedded_support::Role::subscriber,FrameType::Spectrum,"spectrum",{sin,sout,sread,{},{},assembly,samples,id}};
    Transport a,b;a.peer=&b;b.peer=&a;a.quantum=b.quantum=127;
    assert(p.begin(0,1)==Status::ok && s.begin(0,0)==Status::ok);transfer(p,s,a,b,100);
    signal_contracts::FrameView f;f.type=FrameType::Spectrum;f.metadata.stream_id="spectrum";f.metadata.sample_rate=48000;f.metadata.sequence=1;f.values=values;
    int count=0;s.set_frame_handler([](void *ctx,const signal_contracts::FrameView &frame){++*static_cast<int *>(ctx);assert(frame.values.size()==16000 && frame.values.back()==.25f);},&count);
    assert(p.publish(f)==Status::ok);transfer(p,s,a,b,5000);assert(count==1);
}
void sequence_gap() {
    Storage pb,sb;Transport a,b;a.peer=&b;b.peer=&a;a.quantum=b.quantum=127;
    embedded_support::Session p{embedded_support::Role::publisher,FrameType::PCM,"raw",pb.buffers()};
    embedded_support::Session s{embedded_support::Role::subscriber,FrameType::PCM,"raw",sb.buffers()};
    assert(p.begin(0,1)==Status::ok && s.begin(0,0)==Status::ok);transfer(p,s,a,b,100);
    std::array<float,4> pcm{};
    signal_contracts::FrameView f;f.metadata.stream_id="raw";f.metadata.sample_rate=8000;f.metadata.sequence=1;f.left=pcm;f.right=pcm;
    struct Received {int count=0;bool gap=false;} r;
    s.set_frame_handler([](void *ctx,const signal_contracts::FrameView &f){auto &r=*static_cast<Received *>(ctx);++r.count;r.gap=f.metadata.discontinuity;},&r);
    assert(p.publish(f)==Status::ok);transfer(p,s,a,b,100);assert(r.count==1);
    f.metadata.sequence=3;assert(p.publish(f)==Status::ok);transfer(p,s,a,b,100);assert(r.count==2 && r.gap);
    f.metadata.sequence=4;assert(p.publish(f)==Status::ok);transfer(p,s,a,b,100);assert(r.count==3 && !r.gap);
    f.metadata.sequence=6;f.metadata.discontinuity=true;assert(p.publish(f)==Status::ok);transfer(p,s,a,b,100);assert(r.count==4 && r.gap);
    f.metadata.sequence=5;assert(p.publish(f)==Status::ok);transfer(p,s,a,b,100);assert(r.count==4);
}
void independent_fft_windows() {
    if(dsp_kernels::max_fft_samples<2048)return; // Explicit compact kernel profile.
    using namespace domain;
    GraphDefinition graph;graph.graph_id="long-and-short-stereo";
    auto add=[&](int id,NodeKind kind,NodeParameters params){graph.nodes.push_back({id,kind,std::move(params),true});};
    add(0,NodeKind::AudioSource,AudioSourceParameters{});
    add(1,NodeKind::Window,WindowParameters{2048});
    add(2,NodeKind::WindowFunction,WindowFunctionParameters{});
    add(3,NodeKind::StereoFFT,NoParameters{});
    add(4,NodeKind::FFTView,FFTViewParameters{Channel::Merged});
    add(10,NodeKind::Window,WindowParameters{512});
    add(11,NodeKind::WindowFunction,WindowFunctionParameters{});
    add(12,NodeKind::StereoFFT,NoParameters{});
    add(13,NodeKind::FFTView,FFTViewParameters{Channel::Left});
    add(14,NodeKind::FFTView,FFTViewParameters{Channel::Right});
    graph.edges={{0,1},{1,2},{2,3},{3,4},{0,10},{10,11},{11,12},{12,13},{12,14}};
    const std::array<dataflow_runtime::IngressSpec,1> ingress{{{0,{FrameType::PCM,8192,0,0,0,256},256}}};
    application::RuntimeLimits limits;limits.max_frame_samples=2048;limits.max_history_frames=0;
    limits.max_memory_bytes=256*1024;
    std::shared_ptr<const dataflow_runtime::PreparedGraph> plan;
    std::unique_ptr<dataflow_runtime::RuntimeContext> context;
    assert(dataflow_runtime::PreparedGraph::prepare(graph,ingress,limits,plan,context)==Status::ok);
    std::array<float,256> left{},right{};
    struct Result {int long_frames=0,left_frames=0,right_frames=0;std::size_t long_peak=0,left_peak=0,right_peak=0;
        std::uint64_t left_source=0,right_source=0;std::int64_t left_time=0,right_time=0;} result;
    allocation_count=0;measuring_allocations=true;
    for(int tick=0;tick<40;++tick) {
        for(std::size_t i=0;i<left.size();++i) {
            const auto sample=tick*left.size()+i;
            left[i]=.8f*std::sin(float(sample)*6.28318530718f*512/8192);
            right[i]=.4f*std::sin(float(sample)*6.28318530718f*1024/8192);
        }
        signal_contracts::FrameView pcm;pcm.metadata.sample_rate=8192;
        pcm.metadata.sequence=pcm.metadata.source_sequence=tick+1;
        pcm.metadata.producer_epoch=pcm.metadata.generation=1;
        pcm.metadata.timestamp_ns=std::int64_t(tick)*256*1000000000LL/8192;
        pcm.left=left;pcm.right=right;
        assert(plan->process(0,pcm,*context,[](void *ctx,const dataflow_runtime::OutputView &out){
            if(out.kind!=NodeKind::FFTView)return;
            auto &r=*static_cast<Result *>(ctx);
            const auto peak=std::max_element(out.display_values.begin(),out.display_values.end())-out.display_values.begin();
            if(out.node==4){++r.long_frames;assert(out.frame.metadata.fft_size==2048 && out.display_values.size()==1024);r.long_peak=peak;}
            if(out.node==13){++r.left_frames;assert(out.frame.metadata.fft_size==512 && out.display_values.size()==256);
                r.left_peak=peak;r.left_source=out.frame.metadata.source_sequence;r.left_time=out.frame.metadata.timestamp_ns;}
            if(out.node==14){++r.right_frames;r.right_peak=peak;r.right_source=out.frame.metadata.source_sequence;r.right_time=out.frame.metadata.timestamp_ns;}
        },&result)==Status::ok);
    }
    measuring_allocations=false;
    assert(allocation_count==0 && result.long_frames>0 && result.left_frames==result.right_frames);
    assert(result.long_peak==128 && result.left_peak==32 && result.right_peak==64);
    assert(result.left_source==result.right_source && result.left_time==result.right_time);
}
int main(int argc,char **argv){
    producer_fences(argc>1 && std::strcmp(argv[1],"--retired-producer-only")==0);
    independent_fft_windows();
    embedded_support::Profile profile;profile.tcp_bytes_per_second=200000;profile.uart_baud=2000000;
    assert(profile.fft_size==2048); // Samples per channel, not interleaved stereo count.
    profile.fft_size=std::min(profile.fft_size,std::uint32_t(dsp_kernels::max_fft_samples));
    assert(embedded_support::admit(profile,3).status==Status::ok);
    profile.link=embedded_support::Link::uart_8n1;assert(embedded_support::admit(profile,3).status==Status::ok);
    profile.uart_baud=115200;assert(embedded_support::admit(profile,3).status==Status::exhausted);
    profile.spectral=true;assert(embedded_support::admit(profile,3).status==Status::exhausted);
    profile.uart_baud=921600;assert(embedded_support::admit(profile,3).status==Status::ok);
    profile.memory_bytes=1;assert(embedded_support::admit(profile,3).status==Status::exhausted);
    profile.memory_bytes=128*1024;profile.fft_size=513;assert(embedded_support::admit(profile,3).status==Status::invalid_argument);
    auto huge=profile;huge.adapter_bytes=std::numeric_limits<std::size_t>::max();
    assert(embedded_support::runtime_limits(huge).max_memory_bytes==0);
    sessions(1);sessions(127);rejection_and_marker();multiple_fragments();sequence_gap();
    embedded_support::SyntheticPipeline pipeline;
    profile.fft_size=512;profile.spectral=true;profile.link=embedded_support::Link::tcp;profile.adapter_bytes=64*1024;
    assert(pipeline.prepare(profile,"mel")==Status::ok);
    int outputs=0;
    for(int i=0;i<10;++i)assert(pipeline.tick([](void *ctx,const signal_contracts::FrameView &f){++*static_cast<int *>(ctx);assert(f.type==FrameType::Mel && f.values.size()==8);for(float x:f.values)assert(std::isfinite(x));},&outputs)==Status::ok);
    assert(outputs>0);
    profile.spectral=false;assert(pipeline.prepare(profile,"raw")==Status::ok);outputs=0;
    assert(pipeline.tick([](void *ctx,const signal_contracts::FrameView &f){++*static_cast<int *>(ctx);assert(f.type==FrameType::PCM && f.left.size()==256 && f.right.size()==256);},&outputs)==Status::ok);assert(outputs==1);
    for(const auto size:{512u,1024u,2048u,4096u}) {
        embedded_support::Profile spectral;spectral.spectral=true;spectral.fft_size=size;
        spectral.memory_bytes=(size==4096?384:256)*1024;spectral.adapter_bytes=64*1024;
        if(size>dsp_kernels::max_fft_samples) {
            assert(embedded_support::admit(spectral,3).status==Status::invalid_argument);
            continue;
        }
        const auto prepared=pipeline.prepare(spectral,"mel");
        if(prepared!=Status::ok)std::fprintf(stderr,"FFT %u preparation status %d\n",size,int(prepared));
        assert(prepared==Status::ok);
        const auto admitted=embedded_support::admit(spectral,3,pipeline.memory_requirements());
        assert(admitted.status==Status::ok);
        std::fprintf(stdout,"FFT %u: reserved %zu bytes plus source PCM\n",size,admitted.reserved_bytes);
        outputs=0;allocation_count=0;measuring_allocations=true;
        for(int i=0;i<20;++i)assert(pipeline.tick([](void *ctx,const signal_contracts::FrameView &f){
            ++*static_cast<int *>(ctx);assert(f.type==FrameType::Mel && f.values.size()==8);
            for(float x:f.values)assert(std::isfinite(x));
        },&outputs)==Status::ok);
        measuring_allocations=false;assert(outputs>0 && allocation_count==0);
        spectral.memory_bytes=64*1024;
        assert(pipeline.prepare(spectral,"mel")==Status::exhausted);
    }
}
