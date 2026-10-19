#include "numeric_transport_fixture.h"
#include <control/numeric_routing_adapter.h>
#include <QJsonArray>
#include <core_app/numeric_default_graph.h>
#include <cmath>
#include <bit>
#include <cstdio>
using namespace transport_fixture;
static void pcm_delivery_gap(){
    auto recipient=numeric_default_graph();recipient.graph_id="pcm-gap-target";
    const auto retained=[](domain::NodeId id){return id<=7||id==15||id==1000||id==1001;};
    std::erase_if(recipient.nodes,[&](const auto& n){return !retained(n.id);});
    std::erase_if(recipient.edges,[&](const auto& e){return !retained(e.from)||!retained(e.to);});
    // Configuration wires are graph topology too: remove the view bindings
    // together with the views, while retaining the processing-chain bindings.
    std::erase_if(recipient.configuration_bindings,[&](const auto& binding){return !retained(binding.from)||!retained(binding.to);});
    recipient.nodes.front().kind="NetworkInput";
    recipient.nodes.front().parameters=domain::EndpointProcessingParameters{};
    require(recipient.nodes.front().outputs.size()==2&&domain::data_outputs(recipient.nodes.front()).size()==1&&recipient.nodes.front().outputs.back().id==domain::sample_rate_output,"PCM routing preserves one data stream and explicit Fs auxiliary");
    const auto pcm_owner=recipient.nodes.front().outputs[0].contract;
    auto producer=graph("pcm-gap-source");
    producer.nodes[0].outputs[0].contract=pcm_owner;producer.nodes[1].inputs[0].contract=pcm_owner;
    auto independent=contract(2);independent.semantic=Semantic::History;independent.history_element=Semantic::Numeric;
    independent.shape={Rank::Matrix,{Axis::Time,Axis::Declared},{ExtentBounds{0,4},ExtentBounds{2,2}}};
    const auto limits=bdsp_protocol::numeric_control::desktop_limits().frame;
    domain::OwnedProcessingContract independent_owner;
    require(domain::own_processing_contract(independent,limits,16384,independent_owner)==Status::ok,"independent History contract");
    auto branch=graph("unused");branch.nodes[0].id=20;branch.nodes[1].id=21;
    branch.nodes[0].outputs[0].contract=independent_owner;branch.nodes[1].inputs[0].contract=independent_owner;
    recipient.nodes.push_back(branch.nodes[0]);recipient.nodes.push_back(branch.nodes[1]);recipient.edges.push_back({20,21,0,0});
    NumericEngine source,target;Replies sr(source),tr(target);
    sr.accepted(apply(producer));tr.accepted(apply(recipient));

    auto independent_arena=std::make_shared<remote_numeric::Arena>(limits);
    std::array<std::uint64_t,8> independent_values{9007199254740993ULL,UINT64_MAX-1,9007199254740994ULL,UINT64_MAX-2};
    PlaneLayout independent_plane{0,{16,8}};FrameStamp independent_stamp;
    independent_stamp.producer_epoch=31;independent_stamp.generation=9;independent_stamp.sequence=independent_stamp.source_sequence=2;
    independent_stamp.stream_id="independent";independent_stamp.timestamp_ticks=2;
    OwnedDataFrame independent_frame;
    require(retain_frame({independent_stamp,NumericView{&independent,std::as_bytes(std::span(independent_values)),
        std::span(&independent_plane,1),{4,2},{4,2,{}}}},limits,independent_arena->payload_pool,independent_arena->metadata_pool,independent_frame)==Status::ok,"own independent History");
    require(target.feed_numeric_owned(20,independent_frame,2,independent_arena,2)==Status::ok,"independent History ingress");
    const EndpointIdentity independent_endpoint{recipient.graph_id,21,domain::PortDirection::Input,0};
    auto independent_retained=[&]{DeliveredNumericFrame f;std::uint64_t ordinal=0,filled=0;
        if(target.query_numeric(independent_endpoint,f,&ordinal,&filled)!=Status::ok||!f.frame||ordinal!=2||filled!=2)return false;
        const auto view=std::get<NumericView>(f.frame.view().payload);std::uint64_t value=0,first=0,padding=1;
        return view.actual[0]==4&&load_element(view,{0,0,0},padding)==Status::ok&&padding==0&&
            load_element(view,{0,2,0},first)==Status::ok&&first==9007199254740993ULL&&
            load_element(view,{0,3,1},value)==Status::ok&&value==UINT64_MAX-2&&
            f.frame.view().stamp.producer_epoch==31&&f.frame.view().stamp.generation==9;
    };
    require(independent_retained(),"independent History seed");
    NumericControlServer server(source);require(server.listen(QHostAddress::LocalHost,0,true),"PCM gap server");
    NumericRoutingAdapter routing(target);
    require(routing.configure({{"network",QJsonObject{{"routes",QJsonArray{QJsonObject{{"node_id",1},{"host","127.0.0.1"},{"port",int(server.port())},
        {"remote_endpoint",QJsonObject{{"graph_id",producer.graph_id.c_str()},{"node_id",1},{"direction","input"},{"port_index",0}}}}}}}}}),"PCM gap route");
    FrameProbe incoming(target,{recipient.graph_id,1,domain::PortDirection::Output,0});
    auto pcm=std::get<NumericContract>(pcm_owner.view());std::array<float,256> samples{};
    std::array<PlaneLayout,1> planes{{{0,{4,0}}}};std::uint64_t sequence=0,sent=0;
    auto feed_pcm=[&]{for(std::size_t i=0;i<256;++i){
        double left=.15*std::sin(6.283185307179586*(sent+i)*512/48000);
        for(std::size_t tone=0;tone<6;++tone){const auto phase=6.283185307179586*(sent+i)*(64*(tone+1))/48000;
            const auto amplitude=.003+.001*std::sin(double(sequence)*.07+tone);left+=amplitude*std::sin(phase);}
        samples[i]=float(left);
    }
        FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=17;stamp.generation=4;
        stamp.timestamp_ticks=std::int64_t(sent);stamp.stream_id="canonical.audit";sent+=256;
        require(source.feed_numeric(0,{stamp,NumericView{&pcm,std::as_bytes(std::span(samples)),planes,{256,1}}})==Status::ok,"PCM source ingress");
    };
    auto arrives=[&]{auto f=incoming.latest();return f.frame&&f.frame.view().stamp.source_sequence==sequence;};
    require(wait([&]{feed_pcm();return incoming.available();}),"PCM route DATA binding ready");
    for(int i=0;i<4;++i){feed_pcm();require(wait(arrives),"paced PCM route warmup");}
    const EndpointIdentity history_endpoint{recipient.graph_id,7,domain::PortDirection::Output,0};
    auto history_rows=[&]{DeliveredNumericFrame f;std::uint64_t filled=0,ordinal=0;
        require(target.query_numeric(history_endpoint,f,&ordinal,&filled)==Status::ok,"recipient History occupancy query");
        const auto view=std::get<NumericView>(f.frame.view().payload);
        require(view.actual[0]==400 && filled<=400 && filled<=ordinal,"recipient fixed capacity/real occupancy metadata");
        return filled;};
    require(history_rows()>0,"PCM recipient warm History");
    DeliveredNumericFrame warm_history;std::uint64_t warm_ordinal=0,warm_filled=0;
    require(target.query_numeric(history_endpoint,warm_history,&warm_ordinal,&warm_filled)==Status::ok,"retain pre-gap History oracle");
    // No event pumping: real bounded FIFO overflow rejects the tail.
    for(int i=0;i<200;++i)feed_pcm();
    settle(1500);feed_pcm();require(wait(arrives),"FIFO recovery PCM did not cross actual wire/route");
    const auto gap=incoming.latest().frame.view().stamp;
    require(gap.discontinuity&&gap.producer_epoch==17&&gap.generation==4&&gap.source_sequence==sequence,"wire/route lost producer identity or delivery gap");
    require(independent_retained(),"PCM gap reset unrelated History branch");
    for(int i=0;i<3;++i){feed_pcm();require(wait(arrives),"fresh PCM after delivery gap");}
    // A delivery gap resets partial DSP windows, while compatible completed
    // History rows remain. Compare retained rows by absolute accepted ordinal.
    DeliveredNumericFrame resumed;std::uint64_t resumed_ordinal=0,resumed_filled=0;
    require(target.query_numeric(history_endpoint,resumed,&resumed_ordinal,&resumed_filled)==Status::ok,"resumed History query");
    require(resumed_ordinal>warm_ordinal && resumed_filled>=warm_filled,"delivery gap reset compatible completed History");
    const auto warm_view=std::get<NumericView>(warm_history.frame.view().payload);
    const auto resumed_view=std::get<NumericView>(resumed.frame.view().payload);
    std::size_t compared=0;
    for(std::uint64_t index=warm_ordinal-warm_filled;index<warm_ordinal;++index){
        if(resumed_ordinal-index>400)continue;
        const auto before_row=400-std::size_t(warm_ordinal-index);
        const auto after_row=400-std::size_t(resumed_ordinal-index);
        for(std::size_t band=0;band<50;++band){float before=0,after=0;
            require(load_element(warm_view,{0,before_row,band},before)==Status::ok &&
                load_element(resumed_view,{0,after_row,band},after)==Status::ok &&
                std::bit_cast<std::uint32_t>(before)==std::bit_cast<std::uint32_t>(after),"delivery gap changed retained completed History row");}
        ++compared;
    }
    require(compared>0,"delivery-gap oracle retained no comparable rows");
    require(independent_retained(),"resumed PCM reset unrelated History branch");
}
static void history_route(){
    auto h=contract(2);h.semantic=Semantic::History;h.history_element=Semantic::Numeric;
    h.shape.rank=Rank::Matrix;h.shape.axes={Axis::Time,Axis::Declared};h.shape.extents={ExtentBounds{0,4},ExtentBounds{2,2}};
    domain::OwnedProcessingContract owner;const auto limits=bdsp_protocol::numeric_control::desktop_limits().frame;
    require(domain::own_processing_contract(h,limits,16384,owner)==Status::ok,"History route contract");
    auto sourceGraph=graph("history-source"),targetGraph=graph("history-target");
    for(auto* g:{&sourceGraph,&targetGraph}){g->nodes[0].outputs[0].contract=owner;g->nodes[1].inputs[0].contract=owner;}
    NumericEngine source,target;Replies sr(source),tr(target);sr.accepted(apply(sourceGraph));tr.accepted(apply(targetGraph));

    NumericControlServer server(source);require(server.listen(QHostAddress::LocalHost,0,true),"History server");const auto port=server.port();
    NumericRoutingAdapter routing(target);require(routing.configure({{"network",QJsonObject{{"routes",QJsonArray{QJsonObject{{"node_id",0},{"host","127.0.0.1"},{"port",int(port)},
        {"remote_endpoint",QJsonObject{{"graph_id","history-source"},{"node_id",1},{"direction","input"},{"port_index",0}}}}}}}}}),"History route config");
    auto arena=std::make_shared<remote_numeric::Arena>(limits);std::array<std::uint64_t,8> bytes{};PlaneLayout plane{0,{16,8}};
    auto feedHistory=[&](std::uint64_t ordinal){const auto filled=std::min(ordinal,std::uint64_t(4));
        bytes[2*((ordinal-1)%4)]=9007199254740993ULL+ordinal;bytes[2*((ordinal-1)%4)+1]=UINT64_MAX-ordinal;
        FrameStamp stamp;stamp.producer_epoch=source.snapshot().epoch;stamp.generation=1;stamp.sequence=ordinal;stamp.stream_id="history";
        DataFrameView frame{stamp,NumericView{&h,std::as_bytes(std::span(bytes)),std::span(&plane,1),{4,2},{4,std::size_t(ordinal%4),{}}}};
        OwnedDataFrame owned;require(retain_frame(frame,limits,arena->payload_pool,arena->metadata_pool,owned)==Status::ok,"owned History fixture");
        require(source.feed_numeric_owned(0,owned,ordinal,arena,filled)==Status::ok,"owned History source ingress");};
    auto received=[&](std::uint64_t expected){DeliveredNumericFrame f;std::uint64_t ordinal=0,filled=0;
        if(target.query_numeric(sink(targetGraph),f,&ordinal,&filled)!=Status::ok||!f.frame||!f.frame.view().stamp.available||ordinal!=expected)return false;
        const auto view=std::get<NumericView>(f.frame.view().payload);std::uint64_t newest=0;
        require(view.actual[0]==4 && filled==std::min(expected,std::uint64_t(4))&&load_element(view,{0,view.actual[0]-1,0},newest)==Status::ok,"History routed matrix dimensions");
        return newest==9007199254740993ULL+expected;};
    feedHistory(1);require(wait([&]{return received(1);},5000),"cold owned History routing");
    feedHistory(2);require(wait([&]{return received(2);}),"History ordinal advances");
    auto extra=targetGraph.nodes[0];extra.id=2;extra.enabled=false;targetGraph.nodes.push_back(extra);tr.accepted(apply(targetGraph));
    require(received(2),"compatible local Apply erased routed History");feedHistory(3);require(wait([&]{return received(3);}),"History stalls after compatible Apply");
    server.close();require(wait([&]{DeliveredNumericFrame f;return target.query_numeric(sink(targetGraph),f)==Status::unavailable&&f.frame&&!f.frame.view().stamp.available;}),"History outage unavailable");
    require(server.listen(QHostAddress::LocalHost,port,true),"History reconnect listen");
    feedHistory(4);require(wait([&]{return received(4);},5000),"History resume retains previous rows and ordinal");
    auto final=query(target,sink(targetGraph));auto view=std::get<NumericView>(final.frame.view().payload);std::uint64_t oldest=0;
    require(load_element(view,{0,0,0},oldest)==Status::ok&&oldest==9007199254740994ULL,"History outage erased oldest row");
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    NumericEngine source,target;Replies sr(source),tr(target);auto sg=graph("direct-source"),tg=graph("direct-target");
    sr.accepted(apply(sg));tr.accepted(apply(tg));
    NumericControlServer server(source);
    FrameProbe observation(target,sink(tg));
    require(server.listen(QHostAddress::LocalHost,0,true),"source listen");NumericRoutingAdapter routing(target);
    const auto remote=QJsonObject{{"graph_id","direct-source"},{"node_id",1},{"direction","input"},{"port_index",0}};
    QJsonObject route{{"node_id",0},{"host","127.0.0.1"},{"port",int(server.port())},{"remote_endpoint",remote}};
    const auto config=QJsonObject{{"network",QJsonObject{{"routes",QJsonArray{route}}}}};QString error;
    require(routing.configure(config,&error),"direct route configuration");std::uint64_t sequence=0;
    auto arrives=[&](std::uint64_t expected){feed(source,1,++sequence,expected);return observation.equals(expected);};
    require(wait([&]{return arrives(9007199254740993ULL);}),"automatic direct DATA4 routing");
    require(routing.diagnostics()["peer_count"].toInt()==1,"one shared peer");
    auto malformed=route;malformed["port"]="invalid";
    require(!routing.configure({{"network",QJsonObject{{"routes",QJsonArray{malformed}}}}},&error),"invalid routing accepted");
    require(wait([&]{return arrives(17);}),"invalid configuration changed active route");
    // Compatible replacement remains active and keeps the declared route.
    tr.accepted(apply(tg));require(target.snapshot().statuses.value("analyzer").state==State::Running,"Apply suspended continuous core");
    require(wait([&]{return arrives(UINT64_MAX);}),"replacement route exact uint64");
    auto metadata=apply(tg).graph;auto nodes=metadata["nodes"].toArray();auto input=nodes[0].toObject();
    input["adapter"]=QJsonObject{{"host","127.0.0.1"}};nodes[0]=input;metadata["nodes"]=nodes;
    require(routing.validate_graph_routes(metadata,&error)&&routing.configure_graph_routes(metadata,&error),"partial draft metadata rejected");
    require(wait([&]{return arrives(23);}),"graph metadata overrode explicit routes");
    require(routing.configure({}),"clear explicit configuration");routing.refresh();
    require(routing.diagnostics()["peer_count"].toInt()==0,"incomplete route kept prior direct peer");
    require(wait([&]{return !observation.available();}),"removed/replaced route retained stale ingress");
    require(!routing.diagnostics()["error"].toString().isEmpty(),"missing binding diagnostic");
    input["adapter"]=QJsonObject{};nodes[0]=input;metadata["nodes"]=nodes;
    require(routing.configure_graph_routes(metadata),"empty adapter clear");require(routing.diagnostics()["routes"].toArray().isEmpty(),"empty metadata retained route");
    history_route();pcm_delivery_gap();std::puts("canonical direct/owned History/PCM gap DSP routing, atomic config and Stop/Apply/Start passed");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
