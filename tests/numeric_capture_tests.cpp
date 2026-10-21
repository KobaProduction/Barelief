#include <pipeline/numeric_engine.h>
#include <pipeline/capture_buffer.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QThread>
#include <cmath>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <climits>

using namespace pipeline;
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
static bool wait(std::function<bool()> predicate,int timeout=2000) {
    QElapsedTimer time;time.start();
    while(!predicate() && time.elapsed()<timeout){QCoreApplication::processEvents();QThread::msleep(1);}
    return predicate();
}
struct Probe {
    IAudioSource::AudioHandler audio;
    IAudioSource::StatusHandler status;
    IAudioSource::LifecycleHandler lifecycle;
    QJsonObject configuration;
    int starts=0,stops=0;
    bool running=false;
};
class ProbeSource final:public IAudioSource {
    std::shared_ptr<Probe> probe;
public:
    explicit ProbeSource(std::shared_ptr<Probe> value):probe(std::move(value)){}
    void configure(const QJsonObject& value) override{probe->configuration=value;}
    void start() override{++probe->starts;probe->running=true;probe->status(true,{});}
    void stop() override{++probe->stops;probe->running=false;}
    bool is_running() const override{return probe->running;}
    AudioSourceKind source() const override{return AudioSourceKind::SystemOutput;}
    void set_audio_handler(AudioHandler handler) override{probe->audio=std::move(handler);}
    void set_status_handler(StatusHandler handler) override{probe->status=std::move(handler);}
    void set_lifecycle_handler(LifecycleHandler handler) override{probe->lifecycle=std::move(handler);}
};
static AudioFrame tones(std::uint64_t generation=1) {
    AudioFrame frame;frame.sample_rate=48000;frame.timestamp_ns=1000000;frame.generation=generation;
    static std::uint64_t sequence=0;frame.sequence=++sequence;
    frame.channel_count=1;frame.frame_count=256;frame.samples.resize(256);frame.sample_position=(frame.sequence-1)*256;
    for(std::size_t i=0;i<256;++i)for(int tone=0;tone<8;++tone) {
        const auto phase=6.283185307179586*(73+tone*113)*double(i)/48000;
        frame.samples[i]+=float(.02*std::sin(phase));
    }
    return frame;
}
static domain::NumericProcessingGraph graph_with_two_sources() {
    auto full=numeric_default_graph();auto source=full.nodes[0],sink=*std::find_if(full.nodes.begin(),full.nodes.end(),[](const auto& n){return n.id==9;});
    domain::NumericProcessingGraph graph;graph.graph_id="main";
    source.id=1;sink.id=3;graph.nodes={source,sink};source.id=2;sink.id=4;
    graph.nodes.push_back(source);graph.nodes.push_back(sink);graph.edges={{1,3,0,0},{2,4,0,0}};return graph;
}
static void selected_native_lane() {
    using namespace signal_contracts;
    auto graph=graph_with_two_sources();
    std::get<domain::EndpointProcessingParameters>(graph.nodes[0].parameters).values.push_back({"output_count",NumericLiteral{std::uint64_t{1}}});
    std::vector<std::shared_ptr<Probe>> probes;
    NumericEngine engine([&](auto){auto p=std::make_shared<Probe>();probes.push_back(p);return std::make_unique<ProbeSource>(p);});
    QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"limited native graph encode");
    bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& r){replied=true;accepted=r.accepted;});
    EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();engine.submit(command);
    require(wait([&]{return replied;})&&accepted&&probes.size()==2,"limited native graph accepted");
    require(probes[0]->configuration.value("channel_count").toInt()==0,"capture retains native geometry for selected lanes");
    unsigned received=0;bool exact=true;
    auto token=engine.subscribe_numeric_tap({},[&](const auto& e){if(!e.context || e.context->endpoint.node!=3 || !e.frame)return;
        const auto f=e.frame.view();if(!f.stamp.available || !f.stamp.has_sample_interval)return;
        const auto* data=std::get_if<NumericView>(&f.payload);if(!data)return;
        float value=0;exact=exact && load_element(*data,{0,0,0},value)==Status::ok && value==.25f;++received;
    },{});
    auto frame=tones();frame.channel_count=2;frame.samples.assign(512,-.75f);std::fill_n(frame.samples.begin(),256,.25f);
    probes[0]->audio(frame);require(wait([&]{return received>0;})&&exact,"mono output selects first native stereo lane without mixing");
    engine.unsubscribe_numeric_tap(token);
}
static void observe_downstream_error(const DeliveredNumericFrame& delivered,domain::NodeId source,
 domain::NodeId product,domain::NodeId window,unsigned& sourceFrames,unsigned& boundaries,unsigned& errors,std::size_t& windowPrefix) {
 using namespace signal_contracts;
        if(!delivered.context || !delivered.frame)return;const auto frame=delivered.frame.view();
        if(delivered.context->endpoint.node==source && delivered.context->endpoint.port==0){++sourceFrames;boundaries+=frame.stamp.discontinuity;}
        if(delivered.context->endpoint.node==product && !frame.stamp.available)++errors;
        if(delivered.context->endpoint.node==window && frame.stamp.available){
            const auto& values=std::get<NumericView>(frame.payload);windowPrefix=0;
            for(std::size_t i=0;i<values.actual[0];++i){float value=0;require(load_element(values,{0,i,0},value)==Status::ok,"window read");if(value!=0)break;++windowPrefix;}
        }
}
struct CaptureObservations {
 std::size_t available[2]{},unavailable[2]{},zero[2]{};
 void observe(const DeliveredNumericFrame& delivered) {
        if(!delivered.context)return;const auto node=delivered.context->endpoint.node;
        if(node!=3 && node!=4)return;const auto index=node-3;
        if(delivered.frame && delivered.frame.view().stamp.available) {
            ++available[index];const auto frame=delivered.frame.view();
            if(const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload)) {
                bool silent=true;for(std::size_t i=0;i<numeric->actual[0];++i) {
                    float value=0;require(signal_contracts::load_element(*numeric,{0,i,0},value)==signal_contracts::Status::ok,"capture value");silent&=value==0;
                }
                if(silent)++zero[index];
            }
        }else ++unavailable[index];
 }
};
template<class Submit>
static void verify_capture_retirement(NumericEngine& engine,const domain::NumericProcessingGraph& graph,
 EngineCommand& apply,const Submit& submit,const std::vector<std::shared_ptr<Probe>>& probes,const std::size_t* available) {
    auto detached=graph;for(auto& node:detached.nodes)if(node.kind=="AudioSource"){node.kind="NetworkInput";node.parameters=domain::EndpointProcessingParameters{};}
    QByteArray detached_bytes;require(bool(desktop::encode_numeric_graph(detached,detached_bytes)),"detached graph");
    EngineCommand detach=apply;detach.graph=QJsonDocument::fromJson(detached_bytes).object();submit(detach);
    require(engine.snapshot().statuses.value("capture").state==State::Stopped,"removed physical resources remain active");
    require(probes[0]->stops==1 && probes[1]->stops==1,"source replacement did not stop retired providers");
    const auto stale_audio=probes[0]->audio;const auto stale_status=probes[0]->status;
    const auto stopped_count=available[0];stale_audio(tones());stale_status(true,{});
    QCoreApplication::processEvents();require(available[0]==stopped_count && engine.snapshot().statuses.value("capture").state==State::Stopped,"retired source callback accepted");
    submit(apply);require(probes.size()==4,"restart did not create new providers");
    const auto restarted=available[0];stale_audio(tones());stale_status(false,"retired device error");probes[2]->audio(tones(2));
    require(wait([&]{return available[0]>restarted;}),"restarted source unavailable");
    require(engine.snapshot().statuses.value("capture").state==State::Running,"retired source error overwrote new source");
    submit(detach);
}
static void verify_multichannel_ring(unsigned lanes) {
        // Exercise both ring wrap copies and channel-major delivery independently
        // of OS callback chunk sizes. Capacity at 8kHz is 2000 frames.
        CaptureBuffer buffer;buffer.reset(8000,lanes);AudioFrame popped;
        std::vector<float> interleaved(1500*lanes);
        for(std::size_t frame=0;frame<1500;++frame)for(std::size_t lane=0;lane<lanes;++lane)
            interleaved[frame*lanes+lane]=float(lane*10000+frame);
        for(int iteration=0;iteration<3;++iteration) {
            require(buffer.push(interleaved.data(),1500,17,iteration*1500),"N-lane ring push");
            require(buffer.pop(popped) && popped.channel_count==lanes && popped.frame_count==1500,"N-lane ring pop");
            for(std::size_t frame=0;frame<1500;++frame)for(std::size_t lane=0;lane<lanes;++lane)
                require(popped.samples[lane*1500+frame]==interleaved[frame*lanes+lane],"N-lane wrap cross-contaminated lanes");
        }
}
static domain::NumericProcessingGraph multichannel_graph(unsigned lanes) {
 using namespace signal_contracts;
        auto defaults=numeric_default_graph();domain::NumericProcessingGraph graph;graph.graph_id="main";graph.execution_interval={1,120};
        auto source=defaults.nodes.front();source.outputs.clear();
        for(std::uint16_t lane=0;lane<lanes;++lane){auto port=defaults.nodes.front().outputs.front();port.id=lane;source.outputs.push_back(port);}
        graph.nodes.push_back(source);
        for(std::uint16_t lane=0;lane<lanes;++lane) {
            auto window=defaults.nodes[1];window.id=10+lane;
            auto contract=std::get<NumericContract>(window.outputs.front().contract.view());contract.shape.extents[0]={1024,1024};
            require(domain::own_processing_contract(contract,{8,32768,262144,16384,256,8192},16384,window.outputs.front().contract)==Status::ok,"N-lane window contract");
            auto& parameters=std::get<domain::TransformProcessingParameters>(window.parameters);parameters.values.front().value=NumericLiteral{std::uint64_t(1024)};
            graph.nodes.push_back(window);graph.edges.push_back({source.id,window.id,lane,0});
        }
        return graph;
}
static QJsonObject invalid_device_graph(QJsonObject invalid) {
    auto nodes=invalid["nodes"].toArray();
    for(const auto index:{0,1}){auto node=nodes[index].toObject();const auto key=index==0?"outputs":"inputs";auto ports=node[key].toArray();auto port=ports[0].toObject();auto contract=port["contract"].toObject();contract["dtype"]="Int64";port["contract"]=contract;ports[0]=port;node[key]=ports;nodes[index]=node;}
    invalid["nodes"]=nodes;domain::NumericProcessingGraph rejected_graph;
    require(bool(desktop::decode_numeric_graph(QJsonDocument(invalid).toJson(),rejected_graph)),"rejected device fixture must decode");
    return invalid;
}
static auto downstream_error_graph() {
using namespace signal_contracts;
    auto defaults=numeric_default_graph();auto source=defaults.nodes.front(),window=defaults.nodes[1];
    domain::ConfiguredProcessingNode product;product.id=50;product.kind="ArrayMultiply";
    product.parameters=domain::TransformProcessingParameters{};
    product.inputs={source.outputs.front(),source.outputs.front()};
    for(std::size_t i=0;i<product.inputs.size();++i){product.inputs[i].direction=domain::PortDirection::Input;product.inputs[i].id=domain::PortId(i);}
    product.outputs={source.outputs.front()};
    domain::NumericProcessingGraph graph;graph.graph_id="main";graph.nodes={source,window,product};
    graph.edges={{source.id,window.id,0,0},{source.id,product.id,0,0},{source.id,product.id,0,1}};
return graph;
}

static void capture_error_recovery(NumericEngine& engine,const auto& probes,CaptureObservations& observation) {
 auto& available=observation.available;auto& unavailable=observation.unavailable;auto& zero=observation.zero;
    probes[0]->audio(tones());probes[1]->audio(tones());
    require(wait([&]{return available[0] && available[1];}),"both sources delivered");
    probes[0]->status(false,"device disconnected");probes[1]->status(true,{});
    require(wait([&]{return engine.snapshot().statuses.value("capture").state==State::Failed;}),"healthy source hid failed source");
    require(engine.snapshot().statuses.value("capture").error.contains("device disconnected"),"device error lost");
    require(wait([&]{return zero[0]>0;}) && unavailable[0]==0,"known failed source did not advance with typed zeros");
    const auto unaffected=available[1];probes[1]->audio(tones());
    require(wait([&]{return available[1]>unaffected;}),"unrelated source stopped on failure");
    probes[0]->lifecycle(true,0,{});
    require(wait([&]{return engine.snapshot().statuses.value("capture").state==State::Running;}),"capture recovery");
    auto bad=tones();bad.sample_rate=44100;const auto prior_bad=zero[0];probes[0]->audio(bad);
    require(wait([&]{return zero[0]>prior_bad;}),"wrong rate did not enter zero fallback");
    require(wait([&]{return engine.snapshot().statuses.value("capture").state==State::Failed;}),"wrong rate missing failure status");
    require(engine.snapshot().statuses.value("capture").error.contains("expected 48000")&&engine.snapshot().statuses.value("capture").error.contains("received 44100"),"wrong rate diagnostic lost");
    probes[0]->audio(tones());
    require(wait([&]{return engine.snapshot().statuses.value("capture").state==State::Running;}),"valid capture metadata did not recover");
}

static void deliver_lanes(const std::shared_ptr<Probe>& probe,unsigned lanes,std::uint64_t generation,std::size_t begin,std::size_t count,float base) {

            AudioFrame frame;frame.sample_rate=48000;frame.channel_count=lanes;frame.frame_count=count;
            frame.generation=generation;frame.sample_position=begin;frame.discontinuity=begin==0;frame.timestamp_ns=1000000;
            frame.samples.resize(count*lanes);for(std::size_t lane=0;lane<lanes;++lane)
                std::fill_n(frame.samples.begin()+lane*count,count,base+float(lane));probe->audio(frame);
}

static void verify_resumed_lanes(unsigned lanes,const auto& observed) {
using namespace signal_contracts;
        for(std::uint16_t lane=0;lane<lanes;++lane) {
            const auto& frame=observed[lane];require(frame.status==Status::ok && frame.frame,"resumed lane Window unavailable");
            const auto view=std::get<NumericView>(frame.frame.view().payload);require(view.actual[0]==1024,"resumed Window size");
            for(std::size_t sample=0;sample<1024;++sample){float value=0;require(load_element(view,{0,sample,0},value)==Status::ok && value==10+float(lane),"later lane resume reset earlier lane Window");}
        }
}

static void downstream_error_preserves_capture_continuity() {
    using namespace signal_contracts;
    auto graph=downstream_error_graph();
    std::shared_ptr<Probe> probe;NumericEngine engine([&](auto){probe=std::make_shared<Probe>();return std::make_unique<ProbeSource>(probe);});
    QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"error isolation encode");
    bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& r){replied=true;accepted=r.accepted;});
    EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();engine.submit(command);
    require(wait([&]{return replied;}) && accepted && probe,"error isolation Apply");
    unsigned boundaries=0,sourceFrames=0,errors=0;std::size_t windowPrefix=4096;
    const auto token=engine.subscribe_numeric_tap({},[&](const DeliveredNumericFrame& delivered){
        observe_downstream_error(delivered,graph.nodes[0].id,graph.nodes[2].id,graph.nodes[1].id,sourceFrames,boundaries,errors,windowPrefix);
    },{});require(token!=0,"error isolation tap");
    for(unsigned packet=0;packet<32;++packet){auto frame=tones(17);frame.sample_position=packet*256;frame.discontinuity=packet==0;
        // Eight finite tones are accepted PCM; their product overflows Float32
        // only in the separate downstream branch.
        for(auto& value:frame.samples)value*=1e22f;
        probe->audio(frame);QCoreApplication::processEvents();
    }
    require(wait([&]{return sourceFrames==32 && errors>0;}),"failed downstream operation not observed");
    std::fprintf(stderr,"error isolation: source=%u boundaries=%u rejected=%u windowPrefix=%zu\n",sourceFrames,boundaries,errors,windowPrefix);
    require(boundaries==1 && windowPrefix<2,"downstream error fabricated capture gaps and cleared rolling window");
    engine.unsubscribe_numeric_tap(token);
}
static void capture_lifecycle() {
    std::vector<std::shared_ptr<Probe>> probes;
    NumericEngine engine([&](auto){auto probe=std::make_shared<Probe>();probes.push_back(probe);return std::make_unique<ProbeSource>(probe);});
    const auto graph=graph_with_two_sources();QByteArray bytes;
    require(bool(desktop::encode_numeric_graph(graph,bytes)),"encode graph");
    bool replied=false,accepted=false;
    engine.set_reply_handler([&](const auto& reply){replied=true;accepted=reply.accepted;});
    auto submit=[&](EngineCommand command){replied=false;engine.submit(std::move(command));require(wait([&]{return replied;})&&accepted,"command refused");};
    EngineCommand apply;apply.kind=EngineCommand::ApplyGraph;apply.graph=QJsonDocument::fromJson(bytes).object();submit(apply);
    require(probes.size()==2 && wait([&]{return engine.snapshot().statuses.value("capture").state==State::Running;}),"both sources running");
    require(probes[0]->configuration.value("sample_rate").toInt()==48000,"capture contract rate");
    CaptureObservations observation;auto& available=observation.available;auto& unavailable=observation.unavailable;auto& zero=observation.zero;
    const auto token=engine.subscribe_numeric_tap({},[&](auto delivered){
        observation.observe(delivered);
    },{});require(token!=0,"tap");
    capture_error_recovery(engine,probes,observation);
    verify_capture_retirement(engine,graph,apply,submit,probes,available);engine.unsubscribe_numeric_tap(token);
    std::puts("capture lifecycle/error/recovery/rate/zeros/retired callbacks PASS");
}
static void capture_declared_rate() {
    for(const auto rate:{44100u,96000u}){
        std::shared_ptr<Probe> probe;
        NumericEngine engine([&](auto){probe=std::make_shared<Probe>();return std::make_unique<ProbeSource>(probe);});
        auto graph=graph_with_two_sources();graph.nodes.resize(2);graph.edges.resize(1);
        for(auto& node:graph.nodes)for(auto* ports:{&node.inputs,&node.outputs})for(auto& port:*ports){
            auto contract=std::get<signal_contracts::NumericContract>(port.contract.view());contract.clock.sample_rate=rate;contract.clock.tick_seconds={1,rate};
            domain::OwnedProcessingContract owned;require(domain::own_processing_contract(contract,{8,32768,65536,8192,256,8192},65536,owned)==signal_contracts::Status::ok,"rate contract ownership");port.contract=std::move(owned);
        }
        auto& parameters=std::get<domain::EndpointProcessingParameters>(graph.nodes[0].parameters);
        const auto sample_rate=std::find_if(parameters.values.begin(),parameters.values.end(),[](const auto& field){return field.key=="sample_rate";});
        require(sample_rate!=parameters.values.end(),"factory declares source sample rate");
        sample_rate->value=signal_contracts::NumericLiteral{std::uint64_t(rate)};
        QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"rate graph encode");
        bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& r){replied=true;accepted=r.accepted;});
        EngineCommand apply;apply.kind=EngineCommand::ApplyGraph;apply.graph=QJsonDocument::fromJson(bytes).object();engine.submit(apply);
        require(wait([&]{return replied;}) && accepted,"non-default source rate refused");
        require(probe && probe->configuration.value("sample_rate").toInt()==int(rate),"declared rate not sent to capture");
    }
}
static void capture_multichannel() {
    using namespace signal_contracts;
    for(const auto lanes:{1u,2u,6u,8u}) {
        verify_multichannel_ring(lanes);
        auto graph=multichannel_graph(lanes);
        std::shared_ptr<Probe> probe;NumericEngine engine([&](auto){probe=std::make_shared<Probe>();return std::make_unique<ProbeSource>(probe);});
        QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"N-lane graph encode");
        bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& reply){replied=true;accepted=reply.accepted;});
        EngineCommand apply;apply.kind=EngineCommand::ApplyGraph;apply.graph=QJsonDocument::fromJson(bytes).object();engine.submit(apply);
        require(wait([&]{return replied;})&&accepted&&probe,"N-lane Apply");
        require(probe->configuration["channel_count"].toInt()==int(lanes),"admitted N not passed to capture owner");
        const auto deliver=[&](std::uint64_t generation,std::size_t begin,std::size_t count,float base) {deliver_lanes(probe,lanes,generation,begin,count,base);
        };
        std::vector<EndpointIdentity> selected;for(std::uint16_t lane=0;lane<lanes;++lane)selected.push_back({"main",10+lane,domain::PortDirection::Output,0});
        std::vector<DeliveredNumericFrame> observed(lanes);
        const auto tap=engine.subscribe_numeric_tap(selected,[&](DeliveredNumericFrame frame){const auto lane=frame.context->endpoint.node-10;if(lane>=0 && std::size_t(lane)<observed.size())observed[std::size_t(lane)]=std::move(frame);},{});
        require(tap!=0,"N-lane Window tap");
        deliver(1,0,1024,1);deliver(2,0,1024,10);deliver(2,1024,512,10);
        require(wait([&]{return std::all_of(observed.begin(),observed.end(),[](const auto& frame){return bool(frame.frame);});}),"resumed Window delivery");
        verify_resumed_lanes(lanes,observed);
        engine.unsubscribe_numeric_tap(tap);
    }
}
static void measured_processing_telemetry() {
    using namespace signal_contracts;
    std::vector<std::shared_ptr<Probe>> probes;
    NumericEngine engine([&](auto){auto probe=std::make_shared<Probe>();probes.push_back(probe);return std::make_unique<ProbeSource>(probe);});
    QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph_with_two_sources(),bytes)),"telemetry graph encode");
    bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& reply){replied=true;accepted=reply.accepted;});
    EngineCommand apply;apply.kind=EngineCommand::ApplyGraph;apply.graph=QJsonDocument::fromJson(bytes).object();engine.submit(apply);
    require(wait([&]{return replied;})&&accepted&&probes.size()==2,"telemetry Apply");
    require(wait([&]{return !engine.snapshot().diagnostics["node_stats"].toArray().isEmpty();}),"telemetry baseline absent");
    for(std::uint64_t i=0;i<12;++i){AudioFrame frame;frame.sample_rate=48000;frame.channel_count=1;frame.frame_count=256;
        frame.generation=1;frame.sample_position=i*256;frame.timestamp_ns=1000000;frame.samples.assign(256,.25f);probes[0]->audio(frame);}
    require(wait([&]{return engine.snapshot().diagnostics["processing_hz"].toDouble()>0;}),"measured source processing rate absent");
    const auto diagnostic=engine.snapshot().diagnostics;const double seconds=diagnostic["measurement_seconds"].toDouble();
    require(diagnostic["processing_measured"].toBool()&&seconds>0&&diagnostic["processing_rate_node"].toInt()==1&&diagnostic["processing_rate_basis"]=="source_outputs","processing rate has no declared measured origin");
    require(std::abs(diagnostic["processing_hz"].toDouble()*seconds-12)<1e-6,"processing Hz is not actual output count over monotonic interval");
    bool source_seen=false;
    const auto nodeStats=diagnostic["node_stats"].toArray();for(const auto entry:nodeStats){const auto stats=entry.toObject();if(stats["node"].toInt()!=1)continue;source_seen=true;
        require(stats["active"].toBool()&&std::abs(stats["calls_hz"].toDouble()*seconds-12)<1e-6&&std::abs(stats["produced_hz"].toDouble()*seconds-12)<1e-6,"per-node activity/cadence mismatches actual calls");
        require(stats["errors_delta"].toString()=="0"&&std::isfinite(stats["busy_fraction"].toDouble())&&stats["busy_fraction"].toDouble()>=0,"invalid measured node diagnostics");}
    require(source_seen,"measured source node omitted");
    require(wait([&]{const auto next=engine.snapshot().diagnostics;return next["processing_measured"].toBool()&&next["processing_hz"].toDouble()==0;}),"idle source retained stale measured activity");
}
static void test_signal_backend_identity() {
    std::vector<std::shared_ptr<Probe>> probes;std::vector<AudioSourceKind> kinds;
    NumericEngine engine([&](auto kind){kinds.push_back(kind);auto probe=std::make_shared<Probe>();probes.push_back(probe);return std::make_unique<ProbeSource>(probe);});
    bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& reply){replied=true;accepted=reply.accepted;});
    const auto apply=[&](const auto& graph,bool opaque_source=false){QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"backend identity encode");
        auto document=QJsonDocument::fromJson(bytes).object();
        if(opaque_source){auto nodes=document["nodes"].toArray();auto first=nodes[0].toObject();auto parameters=first["parameters"].toObject();auto values=parameters["values"].toArray();
            values.append(QJsonObject{{"key","source"},{"value",0}});parameters["values"]=values;first["parameters"]=parameters;nodes[0]=first;document["nodes"]=nodes;}
        EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=document;replied=false;engine.submit(command);
        require(wait([&]{return replied;})&&accepted,"backend identity Apply");};
    apply(numeric_default_graph());require(kinds.size()==1&&kinds.front()==AudioSourceKind::SystemOutput,"physical AudioSource mapping");
    auto fixture=numeric_test_graph();
    // Unknown JSON fields are opaque document data, not typed processing
    // parameters. A field belonging to AudioSource must not select the backend
    // of an explicit TestSignal after canonical decoding.
    apply(fixture,true);
    require(kinds.size()==2&&kinds.back()==AudioSourceKind::TestSignal&&probes.front()->stops==1,"TestSignal kind was overridden by physical source setting");
    require(probes.back()->configuration["tones"].toInt()==8&&probes.back()->configuration["sample_rate"].toInt()==48000,"TestSignal settings lost during backend replacement");
    apply(fixture);require(kinds.size()==2&&probes.back()->stops==0,"unchanged TestSignal unnecessarily reopened capture");
}
static void capture_partial_interval_gap() {
    using namespace signal_contracts;
    auto graph=graph_with_two_sources();graph.nodes.resize(2);graph.edges.resize(1);
    std::shared_ptr<Probe> probe;
    NumericEngine engine([&](auto){probe=std::make_shared<Probe>();return std::make_unique<ProbeSource>(probe);});
    QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"partial gap graph encode");
    bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& r){replied=true;accepted=r.accepted;});
    EngineCommand apply;apply.kind=EngineCommand::ApplyGraph;apply.graph=QJsonDocument::fromJson(bytes).object();engine.submit(apply);
    require(wait([&]{return replied;})&&accepted&&probe,"partial gap Apply");
    std::vector<DeliveredNumericFrame> observed;
    const auto tap=engine.subscribe_numeric_tap({{"main",3,domain::PortDirection::Input,0}},[&](auto frame){observed.push_back(std::move(frame));},{});
    require(tap!=0,"partial gap tap");
    auto deliver=[&](std::uint64_t begin,float value){
        AudioFrame frame;frame.sample_rate=48000;frame.channel_count=1;frame.frame_count=128;
        frame.generation=1;frame.sample_position=begin;frame.timestamp_ns=1000000;
        frame.samples.assign(128,value);probe->audio(frame);
    };
    deliver(0,1);deliver(384,9);deliver(512,9);
    require(wait([&]{return !observed.empty();})&&observed.size()==1,"partial gap fabricated an extra PCM block");
    require(observed.front().status==Status::ok && bool(observed.front().frame),"partial gap PCM unavailable");
    const auto frame=observed.front().frame.view();const auto& view=std::get<NumericView>(frame.payload);
    require(frame.stamp.discontinuity && frame.stamp.sample_begin==384 && frame.stamp.sample_count==256,"partial gap lost its true sample interval/boundary");
    for(std::size_t i=0;i<256;++i){float value=0;require(load_element(view,{0,i,0},value)==Status::ok&&value==9,"partial gap mixed old and new PCM");}
    engine.unsubscribe_numeric_tap(tap);
}
static void capture_resource_admission() {
    std::vector<std::shared_ptr<Probe>> probes;
    NumericEngine engine([&](auto){auto p=std::make_shared<Probe>();probes.push_back(p);return std::make_unique<ProbeSource>(p);});
    bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& r){replied=true;accepted=r.accepted;});
    auto apply=[&](const auto& graph){QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"budget graph encode");EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();replied=false;engine.submit(command);require(wait([&]{return replied;}),"budget Apply reply");return accepted;};
    const auto working=graph_with_two_sources();require(apply(working),"budget initial Apply");
    const auto revision=engine.snapshot().graph_revision;
    domain::NumericProcessingGraph oversized;oversized.graph_id="main";
    for(int index=0;index<40;++index) {
        auto source=working.nodes.front();source.id=100+index;
        auto contract=std::get<signal_contracts::NumericContract>(source.outputs.front().contract.view());contract.clock.sample_rate=192000;contract.clock.tick_seconds={1,192000};
        source.outputs.clear();for(std::uint16_t lane=0;lane<8;++lane){domain::ProcessingPort port;port.id=lane;port.direction=domain::PortDirection::Output;
            require(domain::own_processing_contract(contract,{8,32768,262144,16384,256,8192},16384,port.contract)==signal_contracts::Status::ok,"budget source contract");source.outputs.push_back(std::move(port));}
        source.parameters=domain::EndpointProcessingParameters{{{"sample_rate",signal_contracts::NumericLiteral{std::uint64_t(192000)}}}};
        oversized.nodes.push_back(std::move(source));
    }
    require(!apply(oversized),"aggregate native capture pool exceeded process budget");
    require(engine.snapshot().graph_revision==revision && probes.size()==2 && probes[0]->stops==0 && probes[1]->stops==0,"capture budget refusal changed working providers");
}
static void external_clock_exhaustion() {
    using namespace signal_contracts;
    auto graph=graph_with_two_sources();graph.nodes.resize(2);graph.edges.resize(1);
    graph.nodes.front().kind="NetworkInput";graph.nodes.front().parameters=domain::EndpointProcessingParameters{};
    NumericEngine engine;QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"external exhausted graph encode");
    bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& r){replied=true;accepted=r.accepted;});
    EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();engine.submit(command);
    require(wait([&]{return replied;})&&accepted,"external exhausted Apply");
    const auto contract=std::get<NumericContract>(graph.nodes.front().outputs.front().contract.view());
    std::array<float,256> values{};PlaneLayout plane{0,{sizeof(float),0}};
    FrameStamp stamp;stamp.producer_epoch=44;stamp.sequence=stamp.source_sequence=1;
    stamp.timestamp_ticks=INT64_MAX-1;stamp.sample_begin=INT64_MAX-256;stamp.sample_count=256;stamp.has_sample_interval=true;
    NumericView view{&contract,std::as_bytes(std::span(values)),std::span(&plane,1),{256,1}};
    require(engine.feed_numeric(1,{stamp,view})==Status::ok,"external near-boundary input rejected");
    stamp.sequence=2;stamp.available=false;view.storage={};
    const auto result=engine.feed_numeric(1,{stamp,view});require(result==Status::ok || result==Status::unavailable,"external disconnect rejected");
    require(wait([&]{return engine.snapshot().statuses.value("input:1").state==State::Failed;}),"external signed clock exhaustion not diagnosed");
    require(engine.snapshot().statuses.value("analyzer").state==State::Running,"exhausted source clock stopped executor");
}
static void physical_probe(bool require_nonzero=false,bool pinned=false) {
    auto source=default_audio_source_factory()(AudioSourceKind::SystemOutput);require(bool(source),"no capture provider");
    std::atomic<bool> observed=false,ready=false;std::atomic<std::size_t> frames=0;
    std::atomic<std::size_t> nonzero=0;std::atomic<bool> invalid=false;
    QString error;std::mutex mutex;
    QString endpoint;
    if(pinned){QString discovery_error;const auto devices=enumerate_audio_capture_devices(AudioSourceKind::SystemOutput,&discovery_error);require(discovery_error.isEmpty()&&!devices.empty(),"physical endpoint discovery");const auto selected=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.is_default;});require(selected!=devices.end(),"default physical endpoint absent from discovery");endpoint=selected->id;std::printf("discovery endpoints=%zu selected=%s name=%s\n",devices.size(),qPrintable(endpoint),qPrintable(selected->name));}
    source->configure({{"sample_rate",48000},{"device_id",endpoint}});
    source->set_audio_handler([&](const auto& frame){frames+=frame.frame_count;if(!frame.sample_rate||!frame.channel_count||frame.samples.size()!=size_t(frame.frame_count)*frame.channel_count)invalid=true;for(auto x:frame.samples){if(!std::isfinite(x))invalid=true;if(std::abs(x)>1e-8f)++nonzero;}});
    source->set_status_handler([&](bool running,QString message){std::lock_guard lock(mutex);ready=running;error=std::move(message);observed=true;});
    source->start();const auto result=wait([&]{return observed.load();},6000);
    if(ready)wait([&]{return require_nonzero?nonzero.load()>0:frames.load()>0;},2000);
    const auto diagnostics=source->diagnostics();source->stop();
    std::lock_guard lock(mutex);
    std::printf("physical SystemOutput observed=%d ready=%d backend=%s device=%s rate=%u frames=%zu error=%s\n",int(result),int(ready.load()),diagnostics.backend.toUtf8().constData(),diagnostics.device.toUtf8().constData(),diagnostics.sample_rate,frames.load(),error.toUtf8().constData());
    require(result,"capture lifecycle timeout");
    std::printf("endpoint=%s native_rate=%u native_channels=%u callbacks=%llu callback_frames=%llu silence_frames=%llu nonzero_samples=%zu\n",qPrintable(diagnostics.endpoint_id),diagnostics.native_sample_rate,diagnostics.native_channels,(unsigned long long)diagnostics.callback_count,(unsigned long long)diagnostics.callback_frames,(unsigned long long)diagnostics.silence_frames,nonzero.load());
    require(ready&&diagnostics.backend=="WASAPI"&&frames&&!invalid,"physical capture init/start/contract failure");
    if(pinned)require(diagnostics.endpoint_id==endpoint,"pinned endpoint silently replaced");
    if(require_nonzero)require(diagnostics.callback_frames&&nonzero,"no nonzero native audio observed during bounded probe");
    // Opening WASAPI and receiving silence does not prove nonzero audio capture.
}
static void unavailable_provider() {
    int attempts=0;
    NumericEngine engine([&](auto)->std::unique_ptr<IAudioSource>{++attempts;return {};});
    QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph_with_two_sources(),bytes)),"encode unavailable graph");
    EngineCommand apply;apply.kind=EngineCommand::ApplyGraph;apply.graph=QJsonDocument::fromJson(bytes).object();
    engine.submit(apply);
    require(wait([&]{return engine.snapshot().statuses.value("capture").state==State::Failed;}),"missing provider not reported");
    require(engine.snapshot().statuses.value("capture").error.contains("Capture source unavailable") && attempts>=2,"missing provider error lost");
    require(engine.snapshot().statuses.value("analyzer").state==State::Running,"missing capture device stopped core");
    std::puts("unavailable provider error/continuous core PASS");
}
static void capture_device_selection() {
    std::vector<std::shared_ptr<Probe>> probes;
    NumericEngine engine([&](auto){auto p=std::make_shared<Probe>();probes.push_back(p);return std::make_unique<ProbeSource>(p);});
    QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph_with_two_sources(),bytes)),"device graph encode");
    auto document=QJsonDocument::fromJson(bytes).object();
    auto selected=[&](const char* id){auto value=document;auto nodes=value["nodes"].toArray();auto node=nodes[0].toObject();node["adapter"]=QJsonObject{{"device_identity",id}};nodes[0]=node;value["nodes"]=nodes;return value;};
    bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& r){replied=true;accepted=r.accepted;});
    auto apply=[&](QJsonObject graph){EngineCommand c;c.kind=EngineCommand::ApplyGraph;c.graph=graph;replied=false;engine.submit(c);require(wait([&]{return replied;}),"device apply completion");return accepted;};
    require(apply(selected("endpoint-A")),"initial device Apply");
    require(wait([&]{return probes.size()==2&&engine.snapshot().statuses.value("capture").state==State::Running;}),"device start");
    require(probes[0]->configuration["device_id"]=="endpoint-A","owning adapter endpoint not forwarded");
    require(engine.snapshot().graph["nodes"].toArray()[0].toObject()["adapter"].toObject()["device_identity"]=="endpoint-A","accepted endpoint absent from authoritative snapshot");
    const auto unaffected=probes[1];
    require(apply(selected("endpoint-B")),"device switch Apply");
    require(wait([&]{return probes.size()==3;}),"device switch producer");
    require(probes[0]->stops==1&&probes[2]->configuration["device_id"]=="endpoint-B"&&unaffected->stops==0,"device switch affected unrelated source");
    auto invalid=invalid_device_graph(selected("endpoint-C"));
    require(!apply(invalid),"invalid device graph accepted");
    require(engine.snapshot().graph["nodes"].toArray()[0].toObject()["adapter"].toObject()["device_identity"]=="endpoint-B","rejected device leaked into authoritative snapshot");
    require(apply(selected("endpoint-B"))&&probes.size()==3&&probes[2]->stops==0,"refused graph changed accepted endpoint");
    require(apply(document)&&probes.size()==4&&probes[2]->stops==1&&!probes[3]->configuration.contains("device_id")&&unaffected->stops==0,"return to OS default changed unrelated source or retained pinned endpoint");
    require(engine.snapshot().statuses.value("analyzer").state==State::Running,"device selection stopped core");
    std::puts("owning capture device identity, isolated producer replacement and rejected Apply atomicity PASS");
}
// Opt-in inventory only. Discovery queries native formats without opening a
// capture stream, playing samples, or changing the user's device selection.
static void physical_discovery() {
    QJsonArray sources;
    for(const auto kind:{AudioSourceKind::SystemOutput,AudioSourceKind::Microphone}) {
        QString error;const auto devices=enumerate_audio_capture_devices(kind,&error);QJsonArray endpoints;
        for(const auto& device:devices) {
            const bool formatKnown=device.native_sample_rate!=0 && device.native_channels!=0;
            endpoints.append(QJsonObject{{"endpoint_id",device.id},{"name",device.name},{"is_default",device.is_default},
                {"availability","enumerated"},{"native_format_available",formatKnown},
                {"native_sample_rate",int(device.native_sample_rate)},{"native_channels",int(device.native_channels)},
                {"separate_output_count",int(device.native_channels)},{"capture_verified",false}});
        }
        sources.append(QJsonObject{{"source",kind==AudioSourceKind::SystemOutput?"loopback":"microphone"},
            {"discovery_available",error.isEmpty()},{"endpoint_count",int(devices.size())},{"error",error},{"endpoints",endpoints}});
    }
    const QJsonObject report{{"probe","physical-discovery"},{"version",1},{"capture_opened",false},{"playback_opened",false},{"sources",sources}};
    const auto json=QJsonDocument(report).toJson(QJsonDocument::Compact);std::printf("%s\n",json.constData());
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {if(argc>1 && QByteArray(argv[1])=="--physical-discovery")physical_discovery();else if(argc>1 && QByteArray(argv[1]).startsWith("--physical")){const auto mode=QByteArray(argv[1]);require(mode=="--physical"||mode=="--physical-nonzero"||mode=="--physical-pinned"||mode=="--physical-pinned-nonzero","unknown physical probe mode");physical_probe(mode.contains("nonzero"),mode.contains("pinned"));}else {downstream_error_preserves_capture_continuity();selected_native_lane();capture_lifecycle();capture_declared_rate();unavailable_provider();capture_device_selection();capture_multichannel();measured_processing_telemetry();test_signal_backend_identity();capture_partial_interval_gap();capture_resource_admission();external_clock_exhaustion();}return 0;}
    catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
