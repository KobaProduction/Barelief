#include <pipeline/numeric_engine.h>
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
    frame.left.resize(256);frame.right.resize(256);
    for(std::size_t i=0;i<256;++i)for(int tone=0;tone<8;++tone) {
        const auto phase=6.283185307179586*(73+tone*113)*double(i)/48000;
        frame.left[i]+=float(.02*std::sin(phase));frame.right[i]+=float(.02*std::cos(phase));
    }
    return frame;
}
static domain::NumericProcessingGraph graph_with_two_sources() {
    auto full=numeric_default_graph();auto source=full.nodes[0],sink=full.nodes[8];
    domain::NumericProcessingGraph graph;graph.graph_id="main";
    source.id=1;sink.id=3;graph.nodes={source,sink};source.id=2;sink.id=4;
    graph.nodes.push_back(source);graph.nodes.push_back(sink);graph.edges={{1,3,0,0},{2,4,0,0}};return graph;
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
    std::size_t available[2]{},unavailable[2]{};
    const auto token=engine.subscribe_numeric_tap({},[&](auto delivered){
        if(!delivered.context)return;const auto node=delivered.context->endpoint.node;
        if(node!=3 && node!=4)return;const auto index=node-3;
        if(delivered.frame && delivered.frame.view().stamp.available)++available[index];else ++unavailable[index];
    },{});require(token!=0,"tap");
    probes[0]->audio(tones());probes[1]->audio(tones());
    require(wait([&]{return available[0] && available[1];}),"both sources delivered");
    probes[0]->status(false,"device disconnected");probes[1]->status(true,{});
    require(wait([&]{return engine.snapshot().statuses.value("capture").state==State::Failed;}),"healthy source hid failed source");
    require(engine.snapshot().statuses.value("capture").error.contains("device disconnected"),"device error lost");
    require(wait([&]{return unavailable[0]>0;}),"failed source did not deliver unavailable");
    const auto unaffected=available[1];probes[1]->audio(tones());
    require(wait([&]{return available[1]>unaffected;}),"unrelated source stopped on failure");
    probes[0]->lifecycle(true,0,{});
    require(wait([&]{return engine.snapshot().statuses.value("capture").state==State::Running;}),"capture recovery");
    auto bad=tones();bad.sample_rate=44100;const auto prior_bad=unavailable[0];probes[0]->audio(bad);
    require(wait([&]{return unavailable[0]>prior_bad;}),"wrong rate silently accepted");
    require(wait([&]{return engine.snapshot().statuses.value("capture").state==State::Failed;}),"wrong rate missing failure status");
    require(engine.snapshot().statuses.value("capture").error.contains("expected 48000")&&engine.snapshot().statuses.value("capture").error.contains("received 44100"),"wrong rate diagnostic lost");
    probes[0]->audio(tones());
    require(wait([&]{return engine.snapshot().statuses.value("capture").state==State::Running;}),"valid capture metadata did not recover");
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
    submit(detach);engine.unsubscribe_numeric_tap(token);
    std::puts("capture lifecycle/error/recovery/rate/Stop/retired callbacks PASS");
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
        parameters.values.push_back({"sample_rate",signal_contracts::NumericLiteral{std::uint64_t(rate)}});
        QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"rate graph encode");
        bool replied=false,accepted=false;engine.set_reply_handler([&](const auto& r){replied=true;accepted=r.accepted;});
        EngineCommand apply;apply.kind=EngineCommand::ApplyGraph;apply.graph=QJsonDocument::fromJson(bytes).object();engine.submit(apply);
        require(wait([&]{return replied;}) && accepted,"non-default source rate refused");
        require(probe && probe->configuration.value("sample_rate").toInt()==int(rate),"declared rate not sent to capture");
    }
}
static void physical_probe(bool require_nonzero=false,bool pinned=false) {
    auto source=default_audio_source_factory()(AudioSourceKind::SystemOutput);require(bool(source),"no capture provider");
    std::atomic<bool> observed=false,ready=false;std::atomic<std::size_t> frames=0;
    std::atomic<std::size_t> nonzero=0;std::atomic<bool> invalid=false;
    QString error;std::mutex mutex;
    QString endpoint;
    if(pinned){QString discovery_error;const auto devices=enumerate_audio_capture_devices(AudioSourceKind::SystemOutput,&discovery_error);require(discovery_error.isEmpty()&&!devices.empty(),"physical endpoint discovery");const auto selected=std::find_if(devices.begin(),devices.end(),[](const auto& d){return d.is_default;});require(selected!=devices.end(),"default physical endpoint absent from discovery");endpoint=selected->id;std::printf("discovery endpoints=%zu selected=%s name=%s\n",devices.size(),qPrintable(endpoint),qPrintable(selected->name));}
    source->configure({{"sample_rate",48000},{"device_id",endpoint}});
    source->set_audio_handler([&](auto frame){frames+=frame.left.size();if(frame.sample_rate!=48000||frame.left.size()!=frame.right.size())invalid=true;for(auto x:frame.left){if(!std::isfinite(x))invalid=true;if(std::abs(x)>1e-8f)++nonzero;}});
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
    std::puts("unavailable provider error/Stop PASS");
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
    auto invalid=selected("endpoint-C");auto nodes=invalid["nodes"].toArray();
    for(const auto index:{0,1}){auto node=nodes[index].toObject();const auto key=index==0?"outputs":"inputs";auto ports=node[key].toArray();auto port=ports[0].toObject();auto contract=port["contract"].toObject();contract["dtype"]="Int64";port["contract"]=contract;ports[0]=port;node[key]=ports;nodes[index]=node;}
    invalid["nodes"]=nodes;domain::NumericProcessingGraph rejected_graph;
    require(bool(desktop::decode_numeric_graph(QJsonDocument(invalid).toJson(),rejected_graph)),"rejected device fixture must decode");
    require(!apply(invalid),"invalid device graph accepted");
    require(engine.snapshot().graph["nodes"].toArray()[0].toObject()["adapter"].toObject()["device_identity"]=="endpoint-B","rejected device leaked into authoritative snapshot");
    require(apply(selected("endpoint-B"))&&probes.size()==3&&probes[2]->stops==0,"refused graph changed accepted endpoint");
    require(apply(document)&&probes.size()==4&&probes[2]->stops==1&&!probes[3]->configuration.contains("device_id")&&unaffected->stops==0,"return to OS default changed unrelated source or retained pinned endpoint");
    require(engine.snapshot().statuses.value("analyzer").state==State::Running,"device selection stopped core");
    std::puts("owning capture device identity, isolated producer replacement and rejected Apply atomicity PASS");
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {if(argc>1 && QByteArray(argv[1]).startsWith("--physical")){const auto mode=QByteArray(argv[1]);require(mode=="--physical"||mode=="--physical-nonzero"||mode=="--physical-pinned"||mode=="--physical-pinned-nonzero","unknown physical probe mode");physical_probe(mode.contains("nonzero"),mode.contains("pinned"));}else {capture_lifecycle();capture_declared_rate();unavailable_provider();capture_device_selection();}return 0;}
    catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
