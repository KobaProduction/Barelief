#include <embedded_support/session.h>
#include <embedded_support/synthetic_pipeline.h>
#include <array>
#include <bit>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace signal_contracts;
namespace nd=bdsp_protocol::numeric_data;
namespace es=embedded_support;
namespace {int failures=0,checks=0;bool tracking=false;std::size_t allocations=0;}
void* operator new(std::size_t n){if(tracking)++allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void operator delete(void* p) noexcept {std::free(p);}void operator delete(void* p,std::size_t) noexcept {std::free(p);}
namespace {
#define CHECK(x) do{++checks;if(!(x)){++failures;std::printf("FAIL %d: %s\n",__LINE__,#x);}}while(false)
struct Transport final:bdsp_protocol::ByteTransport {
    std::array<std::uint8_t,8192> queue{},trace{};std::size_t head=0,count=0,traced=0,quantum=7;
    Transport* peer=nullptr;bool stalled=false,connected=true;
    bdsp_protocol::TransportState state() const noexcept override {return connected?bdsp_protocol::TransportState::connected:bdsp_protocol::TransportState::disconnected;}
    bdsp_protocol::IoResult read(std::span<std::uint8_t> out) noexcept override {
        auto n=std::min({count,out.size(),quantum});for(std::size_t i=0;i<n;++i)out[i]=queue[(head+i)%queue.size()];
        head=(head+n)%queue.size();count-=n;return {n?Status::ok:Status::would_block,n};
    }
    bdsp_protocol::IoResult write(std::span<const std::uint8_t> in) noexcept override {
        auto n=stalled?0:std::min({in.size(),quantum,peer->queue.size()-peer->count});
        for(auto v:in.first(n)){peer->queue[(peer->head+peer->count++)%peer->queue.size()]=v;if(traced<trace.size())trace[traced++]=v;}
        return {n?Status::ok:Status::would_block,n};
    }
    void replay(std::span<const std::uint8_t> b){for(auto x:b){CHECK(count<queue.size());queue[(head+count++)%queue.size()]=x;}}
};
struct Storage {
    static constexpr auto message_bytes=nd::device_block_limits().message_bytes;
    std::array<std::uint8_t,4608> pi{},po{};std::array<std::uint8_t,128> read{};
    std::array<std::uint8_t,message_bytes> tx{},scratch{},assembly{};
    std::array<std::uint8_t,message_bytes+8> fragment{};
    alignas(8) std::array<std::byte,2048> bytes{};std::array<char,2048> text{};
    std::array<std::uint32_t,2> channels{};std::array<PlaneLayout,2> planes{};
    es::NumericSessionBuffers view(){return {pi,po,read,tx,scratch,fragment,assembly,{bytes,text,channels,planes}};}
};
struct Capture{int frames=0,markers=0;std::uint64_t exact=0;bool gap=false;};
void receive(void* p,const nd::Message& message) {
    auto& c=*static_cast<Capture*>(p);c.gap=message.stamp.discontinuity;
    if(!message.stamp.available){++c.markers;return;}++c.frames;
    CHECK(load_element(std::get<NumericView>(message.frame_view().payload),{0,0,0},c.exact)==Status::ok);
    CHECK(message.envelope.nonce==13 && message.envelope.session==17 && message.envelope.revision==19);
}
void transfer(es::NumericSession& p,es::NumericSession& s,Transport& a,Transport& b) {
    for(int i=0;i<1000;++i){CHECK(p.poll(a)==Status::ok);CHECK(s.poll(b)==Status::ok);}
}
void scenario() {
    constexpr std::array<std::uint32_t,1> channels{42};NumericContract contract;contract.dtype=DType::UInt64;contract.channel_order=channels;
    contract.representation.encoding=StorageEncoding::FixedLittleEndian;
    es::NumericSessionBinding binding;binding.binding={{1,1},{"graph",2,nd::Direction::Input,0},"exact64",contract,{},ValueDomain::FiniteOnly};
    binding.negotiated=nd::negotiate(5,5,1,1);binding.nonce=13;binding.session=17;binding.revision=19;binding.publisher_epoch=23;
    Storage ps,ss;Transport a,b;a.peer=&b;b.peer=&a;
    {
        auto passive=binding;auto tagged=contract;tagged.semantic=Semantic::History;passive.binding.contract=tagged;
        es::NumericSession ordinary(es::Role::publisher,passive,ps.view(),nd::device_block_limits());
        CHECK(ordinary.begin()==Status::ok); // A descriptive tag cannot turn a scalar into a ring History.
    }
    es::NumericSession publisher(es::Role::publisher,binding,ps.view(),nd::device_block_limits());
    es::NumericSession subscriber(es::Role::subscriber,binding,ss.view(),nd::device_block_limits());

    CHECK(publisher.begin()==Status::ok && subscriber.begin()==Status::ok);Capture captured;subscriber.set_frame_handler(receive,&captured);
    const std::uint64_t exact=UINT64_MAX;std::array<std::byte,8> bytes{};std::memcpy(bytes.data(),&exact,8);
    constexpr std::array<PlaneLayout,1> planes{{{0,{0,0}}}};
    nd::Message message;message.endpoint=binding.binding.endpoint;message.contract=contract;message.stamp.stream_id="exact64";
    message.stamp.sequence=1;message.stamp.producer_epoch=31;message.stamp.generation=1;message.numeric={bytes,planes,{1,1}};
    tracking=true;CHECK(publisher.publish(message)==Status::ok);transfer(publisher,subscriber,a,b);tracking=false;
    CHECK(allocations==0 && captured.frames==1 && captured.exact==UINT64_MAX && captured.gap);
    auto traced=a.traced;b.replay(std::span(a.trace).first(traced));transfer(publisher,subscriber,a,b);CHECK(captured.frames==1);
    auto wrong=message;wrong.endpoint.port=1;CHECK(publisher.publish(wrong)==Status::incompatible_contract);
    a.stalled=true;message.stamp.sequence=2;CHECK(publisher.publish(message)==Status::ok);
    CHECK(publisher.publish(message)==Status::overflow && publisher.dropped()==1);a.stalled=false;
    transfer(publisher,subscriber,a,b);CHECK(captured.frames==2);
    message.stamp.sequence=3;CHECK(publisher.publish(message)==Status::ok);transfer(publisher,subscriber,a,b);CHECK(captured.frames==3 && captured.gap);
    message.kind=nd::Kind::Unavailable;message.stamp.available=false;message.numeric={};
    CHECK(publisher.publish(message)==Status::ok);transfer(publisher,subscriber,a,b);CHECK(captured.markers==1);
    a.connected=false;CHECK(publisher.poll(a)==Status::disconnected && publisher.state()==es::SessionState::failed);
    auto invalid=binding;invalid.negotiated=nd::negotiate(5,4,1,1);es::NumericSession refused(es::Role::publisher,invalid,ps.view(),nd::device_block_limits());
    CHECK(refused.begin()!=Status::ok);invalid=binding;invalid.negotiated.features=0;
    es::NumericSession zero(es::Role::subscriber,invalid,ss.view(),nd::device_block_limits());CHECK(zero.begin()!=Status::ok);
    invalid=binding;std::get<NumericContract>(invalid.binding.contract).representation.encoding=StorageEncoding::Native;
    es::NumericSession native(es::Role::subscriber,invalid,ss.view(),nd::device_block_limits());CHECK(native.begin()!=Status::ok);
}
// Models the one shared CONTROL PacketStream enqueue boundary, without an OS
// reader in DATA. Backpressure must preserve the fragment until accepted once.
struct PacketQueue {
    std::array<std::uint8_t,64> bytes{};std::size_t size=0,accepted=0;
    std::uint8_t id=0;bool stalled=true;
    static Status send(void* user,std::uint8_t id,std::span<const std::uint8_t> payload) {
        auto& q=*static_cast<PacketQueue*>(user);
        if(q.stalled || q.size)return Status::would_block;
        CHECK(payload.size()<=q.bytes.size());if(payload.size()>q.bytes.size())return Status::buffer_too_small;
        std::copy(payload.begin(),payload.end(),q.bytes.begin());q.size=payload.size();q.id=id;++q.accepted;return Status::ok;
    }
    Status deliver(es::NumericSession& receiver) {
        if(!size)return Status::ok;
        const auto status=receiver.accept_packet({id,std::span(bytes).first(size)});size=0;return status;
    }
};
void external_session() {
    constexpr std::array<std::uint32_t,1> channels{42};NumericContract c;c.dtype=DType::UInt64;
    c.semantic=Semantic::Numeric;c.shape.rank=Rank::Vector;c.shape.axes={Axis::Declared,Axis::None};
    c.shape.extents={ExtentBounds{128,128},ExtentBounds{1,1}};c.channel_order=channels;c.representation.encoding=StorageEncoding::FixedLittleEndian;
    es::NumericSessionBinding binding;binding.binding={{1,1},{"graph",2,nd::Direction::Input,0},"exact64",c,{},ValueDomain::FiniteOnly};
    binding.negotiated=nd::negotiate(5,5,1,1);binding.nonce=13;binding.session=17;binding.revision=19;binding.publisher_epoch=23;
    Storage ps,ss;auto pb=ps.view(),sb=ss.view();pb.packet_in=pb.packet_out=pb.read={};sb.packet_in=sb.packet_out=sb.read={};pb.assembly={};
    PacketQueue queue;es::NumericSession publisher(es::Role::publisher,binding,pb,nd::device_block_limits(),PacketQueue::send,&queue,64);
    es::NumericSession subscriber(es::Role::subscriber,binding,sb,nd::device_block_limits(),PacketQueue::send,&queue,64);
    auto missing_assembly=sb;missing_assembly.assembly={};es::NumericSession refused_subscriber(es::Role::subscriber,binding,missing_assembly,nd::device_block_limits(),PacketQueue::send,&queue,64);CHECK(refused_subscriber.begin()==Status::buffer_too_small);
    CHECK(publisher.begin()==Status::ok && subscriber.begin()==Status::ok);Capture captured;subscriber.set_frame_handler(receive,&captured);
    std::array<std::byte,1024> values{};std::fill(values.begin(),values.begin()+8,std::byte{255});
    constexpr std::array<PlaneLayout,1> planes{{{0,{8,0}}}};
    nd::Message message;message.contract=c;message.endpoint=binding.binding.endpoint;message.stamp.stream_id="exact64";
    message.stamp.sequence=1;message.numeric={values,planes,{128,1},{},{},false};
    tracking=true;const auto before=allocations;
    CHECK(publisher.publish(message)==Status::would_block && publisher.pending_bytes()>0 && queue.accepted==0);
    CHECK(publisher.drain_packets()==Status::would_block && queue.accepted==0);
    queue.stalled=false;
    for(int i=0;i<100 && !captured.frames;++i){CHECK(publisher.drain_packets()==Status::ok);CHECK(queue.deliver(subscriber)==Status::ok);}
    tracking=false;
    CHECK(allocations==before && captured.frames==1 && captured.exact==UINT64_MAX && queue.accepted>1 && publisher.pending_bytes()==0);
    Transport unused;CHECK(publisher.poll(unused)==Status::unavailable);
    queue.stalled=true;message.stamp.sequence=2;CHECK(publisher.publish(message)==Status::would_block);
    message.kind=nd::Kind::Unavailable;message.stamp.available=false;message.numeric={};
    CHECK(publisher.publish(message)==Status::overflow && publisher.state()==es::SessionState::failed && publisher.pending_bytes()==0);
    es::NumericSession bad_capacity(es::Role::publisher,binding,pb,nd::device_block_limits(),PacketQueue::send,&queue,8);
    CHECK(bad_capacity.begin()==Status::invalid_argument);
    CHECK(subscriber.accept_packet({nd::FragmentId,std::span(ss.fragment).first(65)})==Status::buffer_too_small);
    CHECK(captured.frames==1 && subscriber.state()==es::SessionState::failed);
}
struct SourceFlow {
    dataflow_runtime::GraphExecutor* executor=nullptr;int source=0,sink=0;float first=0;std::int64_t timestamp=0;
};
void source_sink(void* user,const dataflow_runtime::NumericPortFrameView& output) {
    auto& flow=*static_cast<SourceFlow*>(user);++flow.sink;
    float actual=0;CHECK(load_element(std::get<NumericView>(output.frame.payload),{0,0,0},actual)==Status::ok);
    CHECK(actual==flow.first && output.frame.stamp.timestamp_ticks==flow.timestamp);
    CHECK(std::get<NumericContract>(output.contract).representation.encoding==StorageEncoding::FixedLittleEndian);
}
void feed_source(void* user,const nd::Message& input) {
    auto& flow=*static_cast<SourceFlow*>(user);++flow.source;flow.timestamp=input.stamp.timestamp_ticks;
    CHECK(input.endpoint.node==0 && input.endpoint.direction==nd::Direction::Output && input.stamp.source_sequence==std::uint64_t(flow.source));
    CHECK(input.stamp.timestamp_ticks==std::int64_t((flow.source-1)*256));
    auto frame=input.frame_view();CHECK(load_element(std::get<NumericView>(frame.payload),{0,0,0},flow.first)==Status::ok);
    CHECK(flow.executor->process_numeric_ports(0,frame,{1,1},{nullptr,source_sink,nullptr,&flow})==Status::ok);
}
void source_only() {
    es::Profile profile;es::SyntheticPipeline source;CHECK(source.prepare_source(profile,"source")==Status::ok);
    const auto device=es::device_profile(profile);dataflow_runtime::GraphExecutor executor(device.runtime);
    application::PreparedExecution candidate;const auto graph=es::default_processing_graph(profile,"source");
    CHECK(executor.prepare_numeric(graph,{device.runtime.admission,device.runtime.resources},candidate)==Status::ok);
    CHECK(executor.apply(std::move(candidate))==Status::ok);
    SourceFlow flow{&executor};tracking=true;const auto before=allocations;
    CHECK(source.tick_source(feed_source,&flow)==Status::ok);const auto first=flow.first;
    CHECK(source.tick_source(feed_source,&flow)==Status::ok);tracking=false;
    CHECK(allocations==before && flow.source==2 && flow.sink==2 && flow.first!=first);
    CHECK(source.process_pcm({},nullptr,nullptr)==Status::unavailable);
    CHECK(executor.shutdown()==Status::ok);CHECK(!executor.running());
}
}
int main(){scenario();external_session();source_only();std::printf("embedded numeric DATA: %d checks, %d failures\n",checks,failures);return failures?1:0;}
