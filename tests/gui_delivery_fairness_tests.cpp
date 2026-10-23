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

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        input_event_between_slow_receivers();
        data_does_not_republish_control_state();
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"GUI delivery fairness FAIL: %s\n",error.what());return 1;
    }
}
