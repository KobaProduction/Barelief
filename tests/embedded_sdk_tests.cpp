#include <embedded_support/session.h>
#include <embedded_support/profile.h>
#include <embedded_support/synthetic_pipeline.h>
#include <cassert>
#include <algorithm>
#include <deque>
#include <vector>
#include <cmath>
#include <limits>
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
void transfer(embedded_support::Session &p, embedded_support::Session &s, Transport &a, Transport &b, int n=10000){
    for(int i=0;i<n;++i){assert(p.poll(a,1)==Status::ok);assert(s.poll(b,1)==Status::ok);}
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
int main(){
    embedded_support::Profile profile;profile.tcp_bytes_per_second=200000;profile.uart_baud=2000000;
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
}
