#include "numeric_transport_fixture.h"
#include <control/numeric_desktop_host.h>
#include <QMetaObject>
#include <set>
using namespace transport_fixture;

static auto cohort() {
    auto g=graph("gui-delivery-fairness");
    const auto prototype=g.nodes.back();
    g.nodes.resize(1);g.edges.clear();
    std::vector<EndpointIdentity> selected;
    for(int i=0;i<24;++i) {
        auto output=prototype;output.id=100+i;g.nodes.push_back(output);
        g.edges.push_back({0,output.id,0,0});
        selected.push_back({g.graph_id,output.id,domain::PortDirection::Input,0});
    }
    return std::pair{std::move(g),std::move(selected)};
}

static void input_event_between_slow_receivers() {
    auto [g,selected]=cohort();NumericDesktopHost host;
    {Replies replies(host);replies.accepted(apply(g));}
    require(host.subscribe_numeric_frames(selected),"subscribe fairness cohort");
    std::set<domain::NodeId> received;
    int observed=-1;
    host.set_numeric_frame_handler([&](const DeliveredNumericFrame& frame) {
        if(!frame.frame || !frame.frame.view().stamp.available || value(frame)!=101)return;
        received.insert(frame.context->endpoint.node);
        if(received.size()==1)QMetaObject::invokeMethod(&host,[&]{observed=int(received.size());},Qt::QueuedConnection);
        QThread::msleep(3); // A reproducibly expensive UI receiver, without a scheduler-dependent threshold.
    });
    feed(*host.local_engine(),1,1,101);
    require(wait([&]{return received.size()==selected.size() && observed>=0;}),"delivery continuation stranded endpoints");
    require(observed<24,"GUI event waited for the complete slow delivery cohort");
    host.set_numeric_frame_handler({});
    std::printf("fairness: queued input ran after %d of 24 receivers\n",observed);
}

static void data_does_not_republish_control_state() {
    NumericDesktopHost host;const auto g=graph("control-state-isolation");
    {Replies replies(host);replies.accepted(apply(g));}
    require(host.subscribe_numeric_frames({sink(g)}),"subscribe control-state fixture");
    settle();
    std::optional<EngineSnapshot> previous;
    unsigned duplicates=0,frames=0;
    host.set_snapshot_handler([&](EngineSnapshot state) {
        if(previous && previous->epoch==state.epoch && previous->graph_revision==state.graph_revision &&
           previous->diagnostics==state.diagnostics && previous->graph==state.graph)++duplicates;
        previous=std::move(state);
    });
    host.set_numeric_frame_handler([&](const DeliveredNumericFrame& frame) {
        if(frame.frame && frame.frame.view().stamp.available && value(frame)>=101)++frames;
    });
    settle(5);
    for(std::uint64_t i=1;i<=8;++i) {
        feed(*host.local_engine(),1,i,100+i);
        require(wait([&]{return frames>=i;}),"control-state fixture missed data");
    }
    host.set_snapshot_handler({});host.set_numeric_frame_handler({});
    require(!duplicates,"data delivery republished unchanged control state");
    std::puts("control state: no duplicate snapshots during data delivery");
}

static void fast_ingress_reserves_fifo_space_for_late_endpoint() {
    NumericEngine engine;Replies replies(engine);auto g=graph("tap-fifo-reservation");
    auto second=graph("unused");
    second.nodes[0].id=2;second.nodes[1].id=3;
    g.nodes.insert(g.nodes.end(),second.nodes.begin(),second.nodes.end());g.edges.push_back({2,3,0,0});
    replies.accepted(apply(g));unsigned fast=0,late=0;bool gap=false;
    const auto token=engine.subscribe_numeric_tap({sink(g),{g.graph_id,3,domain::PortDirection::Input,0}},[&](auto frame){
        if(!frame.frame)return;
        if(frame.context->endpoint.node==3)late+=value(frame)==2001;
        else {++fast;if(value(frame)==130)gap=frame.frame.view().stamp.discontinuity;}
    },{});
    require(token!=0,"subscribe bounded two-endpoint FIFO");
    for(std::uint64_t sequence=1;sequence<=129;++sequence)feed(engine,1,sequence,sequence);
    feed(engine,1,1,2001,2);
    require(wait([&]{return late==1&&fast==32;}),"fast ingress starved the late endpoint");
    feed(engine,1,130,130);
    require(wait([&]{return fast==33;})&&gap,"fair FIFO overflow must retain discontinuity");
    engine.unsubscribe_numeric_tap(token);
}

static void changed_tap_has_retention_space() {
    NumericEngine engine;Replies replies(engine);auto g=graph("tap-retention-reservation");
    auto second=graph("unused");second.nodes[0].id=2;second.nodes[1].id=3;
    g.nodes.insert(g.nodes.end(),second.nodes.begin(),second.nodes.end());g.edges.push_back({2,3,0,0});
    replies.accepted(apply(g));unsigned selected=0;bool retention_error=false;
    const auto token=engine.subscribe_numeric_tap({sink(g)},[&](auto frame){
        retention_error|=frame.status==signal_contracts::Status::exhausted;
        if(frame.frame && frame.context->endpoint.node==3)selected+=value(frame)==2001;
    },{});
    require(token!=0,"subscribe retention pressure fixture");
    for(std::uint64_t sequence=1;sequence<=129;++sequence)feed(engine,1,sequence,sequence);
    engine.update_numeric_tap(token,{{g.graph_id,3,domain::PortDirection::Input,0}});
    feed(engine,1,1,2001,2);
    require(wait([&]{return selected==1;})&&!retention_error,"tap change exhausted the entire shared delivery pool");
    engine.unsubscribe_numeric_tap(token);
}

static void queried_matrix_survives_full_fifo() {
    auto g=graph("tap-query-pressure",4);auto c=contract(4);
    c.shape={Rank::Matrix,{Axis::Declared,Axis::Declared},{ExtentBounds{2,2},ExtentBounds{2,2}}};
    domain::OwnedProcessingContract owner;
    require(domain::own_processing_contract(c,bdsp_protocol::numeric_control::desktop_limits().frame,16384,owner)==Status::ok,"own matrix pressure contract");
    g.nodes[0].outputs[0].contract=owner;g.nodes[1].inputs[0].contract=owner;
    NumericEngine engine;Replies replies(engine);replies.accepted(apply(g));
    const EndpointIdentity endpoint{g.graph_id,0,domain::PortDirection::Output,0};
    const auto token=engine.subscribe_numeric_tap({endpoint},[](auto){},{});
    require(token!=0,"subscribe matrix query pressure fixture");
    std::array<std::uint64_t,4> data{};PlaneLayout plane{0,{16,8}};DeliveredNumericFrame latest;
    for(std::uint64_t sequence=1;sequence<=129;++sequence){
        data.fill(sequence);const auto stamp=feed_stamp(engine.snapshot(),sequence);
        const NumericView input{&c,std::as_bytes(std::span(data)),std::span(&plane,1),{2,2}};
        require(engine.feed_numeric(0,{stamp,input})==Status::ok,"feed matrix without draining observation FIFO");
        require(engine.query_numeric(endpoint,latest)==Status::ok && latest.frame && latest.frame.view().stamp.source_sequence==sequence,"backpressured FIFO erased the latest matrix snapshot");
        std::uint64_t sample=0;
        require(load_element(std::get<NumericView>(latest.frame.view().payload),{0,1,1},sample)==Status::ok && sample==sequence,"matrix query returned stale samples under FIFO pressure");
    }
    engine.unsubscribe_numeric_tap(token);
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        input_event_between_slow_receivers();
        data_does_not_republish_control_state();
        fast_ingress_reserves_fifo_space_for_late_endpoint();
        changed_tap_has_retention_space();
        queried_matrix_survives_full_fifo();
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"GUI delivery fairness FAIL: %s\n",error.what());return 1;
    }
}
