#include "numeric_transport_fixture.h"
#include <cstdio>
#include <core_app/numeric_default_graph.h>
#include <control/numeric_remote_support.h>
using namespace transport_fixture;
static void fifo_and_cancellation_regressions() {
    NumericEngine engine;Replies replies(engine);const auto g=graph("fifo");replies.accepted(apply(g));
    std::vector<std::uint64_t> received;bool recovery_gap=false;
    const auto token=engine.subscribe_numeric_tap({sink(g)},[&](auto frame){if(frame.frame){received.push_back(frame.frame.view().stamp.source_sequence);if(received.back()==130)recovery_gap=frame.frame.view().stamp.discontinuity;}},{});
    for(std::uint64_t sequence=1;sequence<=129;++sequence)feed(engine,1,sequence,sequence);
    require(wait([&]{return received.size()==128;}),"FIFO lost admitted frame batch");
    for(std::size_t index=0;index<128;++index)require(received[index]==index+1,"FIFO processing frames reordered/coalesced");
    feed(engine,1,130,130);require(wait([&]{return received.size()==129;}),"FIFO recovery frame");require(recovery_gap,"real FIFO overflow was silent");engine.unsubscribe_numeric_tap(token);
    unsigned cancelled_calls=0,first_calls=0;std::uint64_t cancelled=0;
    const auto first=engine.subscribe_numeric_tap({sink(g)},[&](auto){++first_calls;engine.unsubscribe_numeric_tap(cancelled);},{});
    cancelled=engine.subscribe_numeric_tap({sink(g)},[&](auto){++cancelled_calls;},{});feed(engine,1,131,131);
    require(wait([&]{return first_calls==1;}),"cancellation first callback");require(cancelled_calls==0,"copied tap invoked after unsubscribe");engine.unsubscribe_numeric_tap(first);
}
static void ratio_json_regression() {
    auto g=graph("ratio");g.execution_interval={9007199254740993LL,9007199254740994ULL};QByteArray encoded;
    require(bool(desktop::encode_numeric_graph(g,encoded)),"exact rational encoding");domain::NumericProcessingGraph restored;
    require(bool(desktop::decode_numeric_graph(encoded,restored))&&restored.execution_interval.numerator==g.execution_interval.numerator&&restored.execution_interval.denominator==g.execution_interval.denominator,"exact rational JSON changed");
    auto document=QJsonDocument::fromJson(encoded).object();document.remove("execution_interval");
    require(bool(desktop::decode_numeric_graph(QJsonDocument(document).toJson(),restored))&&restored.execution_interval.numerator==1&&restored.execution_interval.denominator==60,"missing policy default");
    for(const Ratio bad:std::array{Ratio{0,1},Ratio{1,0},Ratio{2,120},Ratio{61,1}}){g.execution_interval=bad;require(!desktop::encode_numeric_graph(g,encoded),"invalid JSON policy accepted");}
}
static void queue_endpoint_loss_regression() {
    NumericEngine engine;Replies replies(engine);const auto g=graph("queue");replies.accepted(apply(g));DeliveredNumericFrame retained;
    const auto token=engine.subscribe_numeric_tap({sink(g)},[&](auto frame){retained=std::move(frame);},{});feed(engine,1,1,1);require(wait([&]{return bool(retained.frame);}),"queue ownership fixture");feed(engine,1,2,2);require(wait([&]{return retained.frame.view().stamp.source_sequence==2;}),"queue steady frame");engine.unsubscribe_numeric_tap(token);
    auto second=retained;auto context=std::make_shared<NumericDeliveryContext>(*retained.context);context->endpoint.node=2;second.context=std::move(context);
    std::deque<remote_numeric::Queued> queue;remote_numeric::DeliveryLosses losses;
    for(unsigned i=0;i<remote_numeric::OrdinaryQueueSize;++i)require(remote_numeric::enqueue_delivery(queue,retained,losses)==remote_numeric::EnqueueResult::Appended,"queue FIFO capacity");
    require(remote_numeric::enqueue_delivery(queue,retained,losses)==remote_numeric::EnqueueResult::Overflow&&queue.size()==remote_numeric::OrdinaryQueueSize,"queue overflow changed admitted owners");
    queue.pop_front();require(remote_numeric::enqueue_delivery(queue,second,losses)==remote_numeric::EnqueueResult::Appended&&!std::get<DeliveredNumericFrame>(queue.back()).frame.view().stamp.discontinuity,"overflow leaked to unrelated endpoint");
    queue.clear();require(remote_numeric::enqueue_delivery(queue,retained,losses)==remote_numeric::EnqueueResult::Appended&&std::get<DeliveredNumericFrame>(queue.back()).frame.view().stamp.discontinuity,"endpoint recovery missing gap");
    require(!retained.frame.view().stamp.discontinuity,"gap mutated shared immutable producer frame");
}
static void remote_terminal_cancellation() {
    NumericEngine engine;Replies replies(engine);const auto g=graph("terminal");replies.accepted(apply(g));NumericControlServer server(engine);require(server.listen(QHostAddress::LocalHost,0),"terminal server");
    NumericRemoteEndpoint remote("127.0.0.1",server.port());connect(remote);unsigned live=0,terminal=0;
    require(remote.subscribe_numeric_frames({sink(g),{g.graph_id,0,domain::PortDirection::Output,0}}),"two terminal bindings");
    remote.set_numeric_frame_handler([&](auto frame){if(frame.frame&&frame.frame.view().stamp.available)++live;else {++terminal;remote.set_numeric_frame_handler({});}});
    std::uint64_t sequence=0;require(wait([&]{feed(engine,1,++sequence,1);return live>=2;}),"two live terminal bindings");remote.disconnect_remote();require(terminal==1,"retired batch invoked cleared observer");
}
static void demanded_sink_burst() {
    auto template_graph=numeric_default_graph();auto source=template_graph.nodes.front();source.id=0;source.kind="NetworkInput";source.parameters=domain::EndpointProcessingParameters{};
    source.outputs={std::find_if(template_graph.nodes.begin(),template_graph.nodes.end(),[](const auto& node){return node.kind=="FFTView";})->inputs.front()};source.outputs[0].direction=domain::PortDirection::Output;
    domain::NumericProcessingGraph g;g.graph_id="demand";g.nodes={source};domain::NodeId prior=0;
    for(int i=0;i<127;++i){domain::ConfiguredProcessingNode gain;gain.id=100+i;gain.kind="SpectrumGain";gain.parameters=domain::TransformProcessingParameters{{{"gain",NumericLiteral{1.0f}}}};auto input=source.outputs.front();input.direction=domain::PortDirection::Input;gain.inputs={input};gain.outputs=source.outputs;g.edges.push_back({prior,gain.id,0,0});prior=gain.id;g.nodes.push_back(std::move(gain));}
    domain::ConfiguredProcessingNode output;output.id=300;output.kind="NetworkOutput";auto input=source.outputs.front();input.direction=domain::PortDirection::Input;output.inputs={input};g.nodes.push_back(output);g.edges.push_back({prior,300,0,0});
    NumericEngine engine;Replies replies(engine);replies.accepted(apply(g));NumericControlServer server(engine);require(server.listen(QHostAddress::LocalHost,0),"demand server");NumericRemoteEndpoint remote("127.0.0.1",server.port());connect(remote);
    unsigned frames=0,local_frames=0;int final_status=-1;bool final_available=false;const auto final_tap=engine.subscribe_numeric_tap({{g.graph_id,300,domain::PortDirection::Input,0}},[&](auto frame){final_status=int(frame.status);final_available=frame.frame&&frame.frame.view().stamp.available;if(final_available)++local_frames;},{});remote.set_numeric_frame_handler([&](auto frame){if(frame.frame&&frame.frame.view().stamp.available)++frames;});require(remote.subscribe_numeric_frames({{g.graph_id,300,domain::PortDirection::Input,0}}),"final sink demand");
    const auto contract_data=source.outputs[0].contract.view();const auto& c=std::get<NumericContract>(contract_data);std::array<float,2048> pcm{};pcm.fill(.25f);std::array<PlaneLayout,1> planes{{{0,{4,0}}}};std::uint64_t sequence=0;
    const auto arrived=wait([&]{FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.producer_epoch=stamp.generation=1;stamp.stream_id="demand";stamp.timestamp_ticks=std::int64_t(sequence*256);require(engine.feed_numeric(0,{stamp,NumericView{&c,std::as_bytes(std::span(pcm)),planes,{2048,1}}})==Status::ok,"demand PCM feed");return frames>0;});if(!arrived)std::fprintf(stderr,"Demanded sink frames=%u local=%u status=%d available=%d sends=%llu remote epoch=%llu revision=%llu warning=%s server=%s\n",frames,local_frames,final_status,int(final_available),(unsigned long long)sequence,(unsigned long long)remote.snapshot().epoch,(unsigned long long)remote.snapshot().graph_revision,qPrintable(remote.network_warning()),qPrintable(server.error_string()));engine.unsubscribe_numeric_tap(final_tap);require(arrived,"129-port burst starved demanded final NetworkOutput");
    remote.disconnect_remote();
}
static void pcm_transport_window_continuity() {
    auto defaults=numeric_default_graph();auto source=defaults.nodes.front();source.id=0;source.kind="NetworkInput";source.parameters=domain::EndpointProcessingParameters{};
    auto send_graph=graph("pcm");send_graph.nodes[0]=source;auto input=source.outputs.front();input.direction=domain::PortDirection::Input;send_graph.nodes[1].inputs={input};
    auto receive_graph=defaults;receive_graph.graph_id="receiver";receive_graph.nodes.front().kind="NetworkInput";receive_graph.nodes.front().parameters=domain::EndpointProcessingParameters{};
    NumericEngine sender,receiver;Replies sr(sender),rr(receiver);sr.accepted(apply(send_graph));rr.accepted(apply(receive_graph));NumericControlServer server(sender);require(server.listen(QHostAddress::LocalHost,0),"PCM server");NumericRemoteEndpoint remote("127.0.0.1",server.port());connect(remote);require(remote.subscribe_numeric_frames({sink(send_graph)}),"PCM sink subscription");
    std::uint64_t received=0;bool gap=false;remote.set_numeric_frame_handler([&](auto frame){if(frame.frame&&frame.frame.view().stamp.available){++received;if(received>1)gap|=frame.frame.view().stamp.discontinuity;require(receiver.feed_numeric(1,frame.frame.view())==Status::ok,"received PCM processing");}});
    settle(100);const auto data=source.outputs[0].contract.view();const auto& c=std::get<NumericContract>(data);std::array<float,512> pcm{};pcm.fill(.125f);std::array<PlaneLayout,2> planes{{{0,{4,0}},{1024,{4,0}}}};
    std::uint64_t sequence=0;auto send_pcm=[&]{FrameStamp stamp;stamp.sequence=stamp.source_sequence=++sequence;stamp.generation=stamp.producer_epoch=1;stamp.timestamp_ticks=std::int64_t((sequence-1)*256);stamp.stream_id="pcm";require(sender.feed_numeric(0,{stamp,NumericView{&c,std::as_bytes(std::span(pcm)),planes,{256,1}}})==Status::ok,"sender PCM burst");};
    // CONTROL readiness precedes ACKed DATA demand; establish a real delivery first.
    require(wait([&]{send_pcm();return received>0;}),"PCM DATA binding readiness");settle(200);const auto baseline=received;gap=false;
    for(unsigned block=0;block<24;++block)send_pcm();
    const auto complete=wait([&]{return received==baseline+24;});if(!complete)std::fprintf(stderr,"PCM transport received=%llu baseline=%llu sent=%llu warning=%s\n",(unsigned long long)received,(unsigned long long)baseline,(unsigned long long)sequence,qPrintable(remote.network_warning()));
    require(complete,"ordered PCM frames coalesced at sender/receiver");require(!gap,"ordinary PCM delivery invented discontinuity");DeliveredNumericFrame history;std::uint64_t history_filled=0;require(receiver.query_numeric({receive_graph.graph_id,7,domain::PortDirection::Output,0},history,nullptr,&history_filled)==Status::ok&&history_filled>0&&std::get<NumericView>(history.frame.view().payload).actual[0]==400,"continuous PCM never completed Window/FFT/History");remote.disconnect_remote();
}
struct BatchIo final:bdsp_protocol::ByteTransport {
    std::deque<std::uint8_t> incoming;BatchIo* peer=nullptr;
    bdsp_protocol::TransportState state() const noexcept override{return bdsp_protocol::TransportState::connected;}
    bdsp_protocol::IoResult read(std::span<std::uint8_t> out) noexcept override {const auto count=std::min(out.size(),incoming.size());for(std::size_t i=0;i<count;++i){out[i]=incoming.front();incoming.pop_front();}return {Status::ok,count};}
    bdsp_protocol::IoResult write(std::span<const std::uint8_t> bytes) noexcept override {peer->incoming.insert(peer->incoming.end(),bytes.begin(),bytes.end());return {Status::ok,bytes.size()};}
};
static void multi_peer_commit_batch() {
    NumericEngine engine;Replies local(engine);auto g=graph("batch");local.accepted(apply(g));auto profile=engine.capabilities();remote_numeric::CapabilitySnapshot advertised;require(advertised.assign(g,profile)==Status::ok,"batch capabilities");remote_numeric::Clock clock;
    control::numeric::SessionConfig config;config.endpoint_authorized=true;config.device_identity="batch";config.codec_limits=remote_numeric::control_limits();config.runtime_preflights_reply_envelope=true;config.application_capabilities=&profile;config.advertised_capabilities=&advertised.value;
    std::unique_ptr<control::numeric::ControlSession> one,two;
    engine.with_control_service([&](auto& service){config.client_identity=81;one=std::make_unique<control::numeric::ControlSession>(config,clock,&service);config.client_identity=82;two=std::make_unique<control::numeric::ControlSession>(config,clock,&service);});
    for(auto* session:{one.get(),two.get()})session->set_command_commit_handler([](const application::ProcessingReply& reply,void* owner){static_cast<NumericEngine*>(owner)->synchronize_control_state(reply);},&engine);
    config.application_capabilities=nullptr;config.advertised_capabilities=nullptr;config.client_identity=81;control::numeric::ControlSession a(config,clock);config.client_identity=82;control::numeric::ControlSession b(config,clock);BatchIo ai,as,bi,bs;ai.peer=&as;as.peer=&ai;bi.peer=&bs;bs.peer=&bi;
    struct Receipt {unsigned calls=0;std::uint64_t token=0,revision=0;bdsp_protocol::numeric_control::Error error;};Receipt ar,br;
    auto receive=[](const bdsp_protocol::numeric_control::Message& message,void* out){auto& receipt=*static_cast<Receipt*>(out);const auto& reply=std::get<bdsp_protocol::numeric_control::Reply>(message.body);++receipt.calls;receipt.token=reply.state.token;receipt.revision=reply.state.revision;receipt.error=reply.error;};a.set_reply_handler(receive,&ar);b.set_reply_handler(receive,&br);
    auto drain=[&]{for(unsigned i=0;i<1000;++i){a.poll(ai);b.poll(bi);engine.with_control_service([&](auto&){one->poll(as);two->poll(bs);});if(!a.pending_bytes()&&!b.pending_bytes()&&!one->pending_bytes()&&!two->pending_bytes()&&ai.incoming.empty()&&as.incoming.empty()&&bi.incoming.empty()&&bs.incoming.empty())return;}require(false,"multi-peer CONTROL batch timeout");};
    require(a.open(81)==Status::ok&&b.open(82)==Status::ok,"batch open");drain();control::numeric::GraphProjection projected;require(bool(projected.assign(g,remote_numeric::control_limits())),"batch graph projection");namespace nc=bdsp_protocol::numeric_control;nc::Command prepare;prepare.id=nc::CommandId::Prepare;prepare.graph=projected.wire();require(a.send(prepare,1,engine.snapshot().revision)==Status::ok,"batch Prepare");drain();require(ar.calls==1&&ar.error==nc::Error::None,"batch prepared");
    unsigned retained_checks=0;std::uint64_t retained_check=0;engine.with_control_service([&](auto&){retained_check=engine.register_control_reply_check([&](const auto& candidate){++retained_checks;return bool(control::numeric::reply_envelope_fits(candidate,remote_numeric::control_limits(),advertised.value));});});require(retained_check!=0,"retained capability check admission");
    nc::Command apply;apply.id=nc::CommandId::Apply;apply.graph_id=g.graph_id;apply.token=ar.token;const auto revision=ar.revision;require(a.send(apply,2,revision)==Status::ok&&b.send(prepare,1,revision+1)==Status::ok,"same-pump Apply/Prepare");drain();require(ar.calls==2&&br.calls==1&&ar.error==nc::Error::None&&br.error==nc::Error::None,"same-pump committed revision not published before next peer");
    require(retained_checks>=3,"prepared Apply or next Prepare omitted retained capability envelope");engine.with_control_service([&](auto&){engine.unregister_control_reply_check(retained_check);one.reset();two.reset();});
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    fifo_and_cancellation_regressions();ratio_json_regression();queue_endpoint_loss_regression();remote_terminal_cancellation();demanded_sink_burst();pcm_transport_window_continuity();multi_peer_commit_batch();
    {
        auto limits=bdsp_protocol::numeric_data::desktop_limits().frame;
        limits.max_payload_bytes=64;limits.max_metadata_bytes=64;
        auto budget=std::make_shared<remote_numeric::ArenaBudget>(24000);
        auto generation=remote_numeric::Arena::prepare(limits,24,budget);
        require(bool(generation),"cold remote arena admission");
        require(generation->payload_slots.size()==121,"selected ports/reserve slot count");
        const auto charged=budget->used.load();
        require(charged==121*(128+2*sizeof(signal_contracts::BufferSlot)),"arena storage charge");
        auto context=std::make_shared<NumericDeliveryContext>();context->retention_owner=generation;
        auto reused=remote_numeric::Arena::prepare_rebind(limits,24,budget,generation);
        require(reused==generation&&budget->used.load()==charged,"same-selection rebind double charged active arena");
        require(!remote_numeric::Arena::prepare_rebind(limits,128,budget,generation),"growth exceeded live arena budget");
        require(generation->payload_slots.size()==121&&budget->used.load()==charged,"growth refusal changed active arena");
        reused.reset();
        generation.reset();
        require(!remote_numeric::Arena::prepare(limits,24,budget),"retained old generation escaped budget");
        require(budget->used.load()==charged,"refusal changed retained generation charge");
        context.reset();require(budget->used.load()==0,"retired arena charge leaked");
        generation=remote_numeric::Arena::prepare(limits,24,budget);
        require(bool(generation),"cold rebind did not recover released budget");
    }
    NumericEngine engine;Replies local(engine);const auto graph=transport_fixture::graph("large",10000);local.accepted(apply(graph));
    NumericControlServer server(engine);require(server.listen(QHostAddress::LocalHost,0,true),"actual canonical server");
    NumericRemoteEndpoint first("127.0.0.1",server.port()),second("127.0.0.1",server.port());Replies a(first),b(second);
    connect(first);connect(second);std::size_t ac=0,bc=0,other_count=0;DeliveredNumericFrame af,bf;
    const EndpointIdentity source_endpoint{graph.graph_id,0,domain::PortDirection::Output,0};
    require(first.subscribe_numeric_frames({source_endpoint,sink(graph)})&&second.subscribe_numeric_frames({sink(graph)}),"TCP numeric subscription refused");
    first.set_numeric_frame_handler([&](auto f){if(f.context&&f.frame&&f.frame.view().stamp.available){
        if(f.context->endpoint==source_endpoint)++other_count;
        if(f.context->endpoint==sink(graph)){af=std::move(f);++ac;}
    }});
    second.set_numeric_frame_handler([&](auto f){if(f.context&&f.context->endpoint==sink(graph)&&f.frame&&f.frame.view().stamp.available){bf=std::move(f);++bc;}});
    std::uint64_t sequence=0;
    const auto both_clients=wait([&]{feed(engine,10000,++sequence,9007199254740993ULL);return ac&&bc&&other_count;});
    if(!both_clients)std::fprintf(stderr,"fragmented diagnostics: A=%zu B=%zu other=%zu Aepoch=%llu Brevision=%llu Awarning=%s Bwarning=%s\n",
        ac,bc,other_count,static_cast<unsigned long long>(first.snapshot().epoch),
        static_cast<unsigned long long>(second.snapshot().graph_revision),qPrintable(first.network_warning()),qPrintable(second.network_warning()));
    require(both_clients,"two clients fragmented DATA4");
    require(other_count>0,"TCP source/sink prewarm did not select both ports");
    require(first.subscribe_numeric_frames({sink(graph)}),"TCP selected subset rebind refused");
    const auto prior_a=ac,prior_b=bc;
    require(wait([&]{feed(engine,10000,++sequence,9007199254740993ULL);return ac>prior_a&&bc>prior_b;}),"removed TCP subscription starved retained selected port");
    require(value(af)==9007199254740993ULL&&value(bf,9999)==9007199254750992ULL,"large exact uint64 payload changed");
    const auto retained=af;first.disconnect_remote();require(wait([&]{return first.snapshot().epoch==0;}),"disconnect retirement");
    require(value(retained)==9007199254740993ULL,"retained arena after endpoint retirement");
    const auto before=bc;require(wait([&]{feed(engine,10000,++sequence,UINT64_MAX-10000);return bc>before;}),"independent client stopped");
    connect(first);const auto prior=ac;require(wait([&]{feed(engine,10000,++sequence,123);return ac>prior&&value(af)==123;}),"fresh reconnect binding");
    const auto revision=second.snapshot().graph_revision;auto changed=graph;changed.nodes[1].enabled=false;
    auto mutation=apply(changed);mutation.expected_revision=revision;b.accepted(mutation);
    mutation.expected_revision=revision;require(!b.submit(mutation).accepted,"stale graph revision accepted");
    auto malformed=command(EngineCommand::ApplyGraph);malformed.graph={{"version",99}};require(!b.submit(malformed).accepted,"future graph accepted");
    require(engine.snapshot().graph_revision==revision+1,"rejected mutation changed graph");
    require(engine.snapshot().statuses.value("analyzer").state==State::Running,"Apply is continuously active");
    auto adapter_edit=apply(changed);auto nodes=adapter_edit.graph["nodes"].toArray();auto node=nodes[0].toObject();node["adapter"]=QJsonObject{{"host","127.0.0.1"}};nodes[0]=node;adapter_edit.graph["nodes"]=nodes;
    require(!b.submit(adapter_edit).accepted,"unsupported remote adapter metadata silently lost");
    NumericControlServer readonly(engine);require(readonly.listen(QHostAddress::LocalHost,0,false),"read-only endpoint");NumericRemoteEndpoint observer("127.0.0.1",readonly.port());Replies read(observer);connect(observer);
    require(!read.submit(apply(graph)).accepted,"read-only mutation accepted");
    second.disconnect_remote();observer.disconnect_remote();std::puts("actual CONTROL2/DATA4 TCP exact64/fragmentation/two clients/reconnect/rejected Apply/Stop/read-only passed");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
