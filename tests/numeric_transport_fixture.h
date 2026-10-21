#pragma once
#include <pipeline/numeric_engine.h>
#include <control/numeric_control_server.h>
#include <control/numeric_remote_endpoint.h>
#include <control/numeric_remote_wire.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QThread>
#include <map>
#include <mutex>
#include <stdexcept>
#include <cstdio>

namespace transport_fixture {
using namespace pipeline;
using namespace signal_contracts;
inline void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline bool wait(const std::function<bool()>& predicate,int ms=3000){
    QElapsedTimer timer;timer.start();while(!predicate()&&timer.elapsed()<ms){QCoreApplication::processEvents();QThread::msleep(1);}return predicate();
}
inline void settle(int ms=60){QElapsedTimer timer;timer.start();wait([&]{return timer.elapsed()>=ms;},ms+100);}
inline NumericContract contract(std::size_t elements=1){
    static constexpr std::array<std::uint32_t,1> channels{0};
    NumericContract c;c.dtype=DType::UInt64;c.channel_order=channels;
    c.semantic=elements==1?Semantic::Scalar:Semantic::Numeric;
    c.shape.rank=elements==1?Rank::Scalar:Rank::Vector;
    if(elements!=1){c.shape.axes[0]=Axis::Declared;c.shape.extents[0]={elements,elements};}
    c.representation.encoding=StorageEncoding::Native;c.representation.alignment=8;
    c.clock.domain="transport.fixture";return c;
}
inline domain::NumericProcessingGraph graph(std::string id="transport",std::size_t elements=1){
    domain::OwnedProcessingContract owner;
    require(domain::own_processing_contract(contract(elements),bdsp_protocol::numeric_control::desktop_limits().frame,16384,owner)==Status::ok,"own fixture contract");
    domain::ConfiguredProcessingNode input;input.id=0;input.kind="NetworkInput";
    input.outputs.push_back({0,domain::PortDirection::Output,owner});
    domain::ConfiguredProcessingNode output;output.id=1;output.kind="NetworkOutput";
    output.inputs.push_back({0,domain::PortDirection::Input,owner});
    domain::NumericProcessingGraph g;g.graph_id=std::move(id);g.nodes={input,output};g.edges={{0,1,0,0}};return g;
}
inline EndpointIdentity sink(const domain::NumericProcessingGraph& g){return {g.graph_id,1,domain::PortDirection::Input,0};}
inline EngineCommand apply(const domain::NumericProcessingGraph& graph){
    QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"canonical graph encoding");
    EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();return command;
}
struct Replies {
    EnginePort& port;std::map<quint64,EngineReply> values;
    explicit Replies(EnginePort& p):port(p){port.set_reply_handler([this](EngineReply r){values[r.request_id]=std::move(r);});}
    ~Replies(){port.set_reply_handler({});}
    EngineReply submit(EngineCommand command){const auto id=port.submit(std::move(command));require(wait([&]{return values.contains(id);}),"canonical command reply timeout");auto result=std::move(values.at(id));values.erase(id);return result;}
    void accepted(EngineCommand command){const auto result=submit(std::move(command));if(!result.accepted)std::fprintf(stderr,"canonical command rejected: %s\n",qPrintable(result.error));require(result.accepted,"canonical command rejected");}
};
inline EngineCommand command(EngineCommand::Kind kind){EngineCommand c;c.kind=kind;return c;}
inline void connect(NumericRemoteEndpoint& remote){remote.connect_remote();require(wait([&]{return remote.snapshot().epoch!=0;}),"CONTROL2 connection");}
template<class State> inline FrameStamp feed_stamp(const State& state,std::uint64_t sequence) {
    FrameStamp stamp;stamp.epoch=state.epoch;stamp.graph_revision=state.graph_revision;
    stamp.producer_epoch=state.epoch;stamp.generation=1;stamp.sequence=stamp.source_sequence=sequence;
    stamp.stream_id="fixture";stamp.timestamp_ticks=std::int64_t(sequence);return stamp;
}
template<class Engine> inline void feed(Engine& engine,std::size_t elements,std::uint64_t sequence,std::uint64_t value,int node=0){
    auto c=contract(elements);std::vector<std::uint64_t> values(elements,value);
    for(std::size_t i=0;i<elements;++i)values[i]+=i;
    PlaneLayout plane{0,{8,0}};
    const auto stamp=feed_stamp(engine.snapshot(),sequence);
    DataFrameView frame{stamp,NumericView{&c,std::as_bytes(std::span(values)),std::span(&plane,1),{elements,1}}};
    const auto status=engine.feed_numeric(node,frame);
    if(status!=Status::ok)std::fprintf(stderr,"canonical ingress node=%d sequence=%llu status=%d\n",node,(unsigned long long)sequence,int(status));
    require(status==Status::ok,"canonical ingress feed");
}

inline std::uint64_t value(const DeliveredNumericFrame& frame,std::size_t index=0){
    require(frame.frame&&frame.status==Status::ok&&frame.frame.view().stamp.available,"available numeric delivery");
    const auto view=std::get<NumericView>(frame.frame.view().payload);std::uint64_t result=0;
    require(view.contract->dtype==DType::UInt64&&load_element(view,{0,index,0},result)==Status::ok,"exact uint64 delivery");return result;
}
// Scalar/vector observation follows the owning event API. query_numeric is
// specifically a cold History snapshot API, not a scalar latest-value cache.
struct FrameProbe {
    struct Shared {std::mutex mutex;DeliveredNumericFrame frame;std::size_t count=0;};
    NumericEngine& engine;std::shared_ptr<Shared> shared=std::make_shared<Shared>();std::uint64_t token=0;
    FrameProbe(NumericEngine& e,EndpointIdentity endpoint):engine(e){
        token=engine.subscribe_numeric_tap({std::move(endpoint)},[state=shared](DeliveredNumericFrame frame){std::lock_guard lock(state->mutex);state->frame=std::move(frame);++state->count;},{});
        require(token!=0,"owning numeric probe subscription");
    }
    ~FrameProbe(){engine.unsubscribe_numeric_tap(token);}
    DeliveredNumericFrame latest()const{std::lock_guard lock(shared->mutex);return shared->frame;}
    std::size_t count()const{std::lock_guard lock(shared->mutex);return shared->count;}
    bool available()const{auto f=latest();return f.status==Status::ok&&f.frame&&f.frame.view().stamp.available;}
    bool equals(std::uint64_t expected)const{auto f=latest();return f.status==Status::ok&&f.frame&&f.frame.view().stamp.available&&value(f)==expected;}
};
template<class Engine> inline DeliveredNumericFrame query(Engine& engine,const EndpointIdentity& endpoint){DeliveredNumericFrame result;require(engine.query_numeric(endpoint,result)==Status::ok,"cold numeric query");return result;}
}
