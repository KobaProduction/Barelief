#include "basic_signal_probe.h"
#include "numeric_transport_fixture.h"
#include "synthetic_capture.h"
#include <core_app/numeric_default_graph.h>
#include <QCoreApplication>
#include <cstdio>
#include <atomic>

static void verify_basic_window(std::size_t size) {
    using namespace transport_fixture;
    const auto graph=numeric_basic_signal_graph(48000,size);
    require(basic_signal_probe::graph_is_basic(graph,size),"exact three-node basic graph");
    NumericEngine engine;Replies replies(engine);replies.accepted(apply(graph));
    const auto state=engine.snapshot();
    const auto observation=std::make_shared<basic_signal_probe::Observation>(size,pipeline::DeliveryStamp{state.graph_revision,state.epoch});
    const auto tap=engine.subscribe_numeric_tap(basic_signal_probe::endpoints(),[observation](const auto& frame){observation->observe(frame);},{});
    require(tap!=0,"basic source/Window/view tap");
    const bool ready=wait([&]{return observation->ready();},5000);
    engine.unsubscribe_numeric_tap(tap);observation->report("headless");
    require(ready&&observation->good(),"basic Window samples, output count and delivery progression");
}
static void verify_live_reference() {
    using namespace transport_fixture;
    NumericEngine engine(test_fixture::startup_capture_factory());Replies replies(engine);
    const auto graph=numeric_reference_beat_example().graph;replies.accepted(apply(graph));
    std::atomic<unsigned> received{0};
    const auto tap=engine.subscribe_numeric_tap({{graph.graph_id,1,domain::PortDirection::Output,0},
        {graph.graph_id,3,domain::PortDirection::Output,0},{graph.graph_id,4,domain::PortDirection::Output,0},
        {graph.graph_id,35,domain::PortDirection::Output,0},{graph.graph_id,30,domain::PortDirection::Output,0}},[&](const auto& delivered){
        if(!delivered.frame || !delivered.context || !delivered.frame.view().stamp.available)return;
        const auto& endpoint=delivered.context->endpoint;const auto frame=delivered.frame.view();const auto& value=std::get<NumericView>(frame.payload);
        if(endpoint.node==1 && value.actual[0]==512)received.fetch_or(1u);
        if(endpoint.node==3 && value.contract->shape.rank==signal_contracts::Rank::Scalar)received.fetch_or(2u);
        if(endpoint.node==4)received.fetch_or(4u);
        if(endpoint.node==35 && value.contract->shape.rank==signal_contracts::Rank::Scalar)received.fetch_or(8u);
        if(value.actual[0]==128 && endpoint.node==30)received.fetch_or(16u);
    },{});
    require(tap!=0,"streaming rhythm subscriptions");
    const bool ready=wait([&]{return received.load()==31;},6000);engine.unsubscribe_numeric_tap(tap);
    require(ready,"direct source, scalar onset/BPM, beat grid and one difference frame");
    std::puts("Headless desktop streaming without Mel display history PASS");
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {
        const auto examples=numeric_example_graphs();
        transport_fixture::require(examples.size()==2&&std::string_view(examples.front().id)=="reference-beat"&&std::string_view(examples.back().id)=="compact-spectrogram","only the rhythm and compact spectrogram presets");
        transport_fixture::require(basic_signal_probe::graph_is_basic(numeric_startup_example().graph,500),"basic default is three nodes and N500");
        for(const auto size:{1,10,200,500,1024})verify_basic_window(size);
        verify_live_reference();
        std::puts("Basic default plus one reference preset; arbitrary Window sizes, exact samples and progress PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"basic headless: %s\n",error.what());return 1;}
}
