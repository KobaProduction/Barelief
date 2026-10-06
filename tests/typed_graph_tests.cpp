#include "pipeline/audio_pipeline.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QDebug>
#include <cmath>
#include <thread>
#include <stdexcept>
#include <condition_variable>
#include <QUdpSocket>
#include <cstring>
#include <source_location>
using namespace pipeline;
static void check(bool ok,const char *why) { if(!ok) throw std::runtime_error(why); }
static void check_apply(AudioPipeline &runtime, const QJsonObject &document, QString &error,
                        std::source_location location = std::source_location::current()) {
    const bool applied = runtime.apply_graph(document, error);
    if (!applied) throw std::runtime_error((QStringLiteral("Apply at line %1 failed: %2")
        .arg(location.line()).arg(error)).toStdString());
}
static QJsonObject node(int id,const char *kind,QJsonObject p={}) {
    if(QString(kind).startsWith("Network") && !p.contains("stream_id")) p["stream_id"]=QString::number(id);
    p["model-name"]=kind;return {{"id",id},{"internal-data",p}};
}
static QJsonObject edge(int a,int b) { return {{"outNodeId",a},{"outPortIndex",0},{"inNodeId",b},{"inPortIndex",0}}; }
static QJsonObject graph(QJsonArray n,QJsonArray e) { return {{"version",2},{"nodes",n},{"connections",e}}; }
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        QString error;
        auto doc=GraphProgram::defaults();check(doc["version"]==2,"Default is not explicit v2");
        auto p=GraphProgram::compile(doc,error);check(bool(p),qPrintable(error));
        // Gate each category without bypassing type checks or disturbing another ingress.
        auto gates=graph({node(0,"NetworkInput",{{"data_type","mel"}}),node(1,"Spectrogram"),node(2,"SpectrogramView"),
            node(3,"NetworkOutput",{{"data_type","history"}}),node(4,"WLED"),
            node(10,"NetworkInput",{{"data_type","mel"}}),node(11,"MelView")},
            {edge(0,1),edge(1,2),edge(1,3),edge(0,4),edge(10,11)});
        for(const int gated:{0,1,2,3,4}) {
            auto disabled=gates;auto nodes=disabled["nodes"].toArray();
            for(int i=0;i<nodes.size();++i) {auto n=nodes[i].toObject();if(n["id"].toInt()!=gated) continue;
                auto params=n["internal-data"].toObject();params["enabled"]=false;n["internal-data"]=params;nodes[i]=n;}
            disabled["nodes"]=nodes;auto plan=GraphProgram::compile(disabled,error);check(bool(plan),qPrintable(error));
            GraphEvaluationContext context;TypedFrame input;input.type=FrameType::Mel;input.sample_rate=48000;input.sequence=1;input.values={1,2,3};
            auto out=plan->evaluate(0,input,context);const auto markers=plan->disabled_output();
            check(markers.previews.contains(gated) && markers.previews[gated].values==0,"Disabled preview retains values");
            for(auto it=markers.views.begin();it!=markers.views.end();++it) {
                const auto frame=out.views[it.key()];check(frame.available && std::all_of(frame.values.begin(),frame.values.end(),[](float v){return v==0;}),"Disabled descendant did not receive silence");
                if(!frame.history.empty()) check(std::all_of(frame.history.back().begin(),frame.history.back().end(),[](float v){return v==0;}),"Disabled history row is nonzero");
            }
            for(const auto &[id,event]:markers.output_events) check(out.ports[id].available && std::all_of(out.ports[id].values.begin(),out.ports[id].values.end(),[](float v){return v==0;}),"Disabled output did not receive silence");
            check(plan->evaluate(10,input,context).views[11].available,"Disabled branch blocked independent ingress");
        }
        auto invalid=gates;auto invalid_nodes=invalid["nodes"].toArray();auto invalid_node=invalid_nodes[0].toObject();
        auto invalid_params=invalid_node["internal-data"].toObject();invalid_params["enabled"]="false";invalid_node["internal-data"]=invalid_params;invalid_nodes[0]=invalid_node;invalid["nodes"]=invalid_nodes;
        check(!GraphProgram::compile(invalid,error),"String enabled accepted");
        auto wrong_type=graph({node(0,"NetworkInput",{{"data_type","mel"},{"enabled",false}}),node(1,"Window",{{"enabled",false}})},{edge(0,1)});
        check(!GraphProgram::compile(wrong_type,error),"Disabled nodes bypassed type validation");
        check(GraphProgram::compile(gates,error)->disabled_output().views.isEmpty(),"Legacy missing enabled is not true");
        for(const auto &definition:node_definitions()) check(definition.defaults["enabled"].toBool() && definition.constraints["enabled"].toObject()["type"]=="bool","Enabled metadata missing");
        for(const char *name:{"hann","hamming","blackman","rectangular"}) {
            auto window_graph=graph({node(0,"NetworkInput",{{"data_type","window"}}),node(1,"WindowFunction",{{"function",name}}),node(2,"WindowedView")},{edge(0,1),edge(1,2)});
            auto plan=GraphProgram::compile(window_graph,error);check(bool(plan),qPrintable(error));GraphEvaluationContext context;
            TypedFrame input;input.type=FrameType::Window;input.sample_rate=48000;input.sequence=1;input.left.assign(513,1);input.right=input.left;
            const auto values=plan->evaluate(0,input,context).views[2].left;
            check(values.size()==513 && std::abs(values[256]-1)<1e-6,"Window selection did not produce coefficients");
            check(std::abs(values[0]-(QString(name)=="hamming"?0.08f:QString(name)=="rectangular"?1.f:0.f))<1e-6,"Window selection ignored");
        }
        for(int scenario=0;scenario<3;++scenario) {
            TypedFrame f;f.type=FrameType::Window;f.sample_rate=48000;f.sequence=1;f.generation=1;f.timestamp_ns=123456;
            f.left.resize(4096);f.right.resize(4096);
            for(size_t i=0;i<4096;++i) {
                // Eight changing carriers, independently present in both channels.
                for(int t=0;t<8;++t) {
                    const float s=std::sin(2*3.141592653589793*(17+t*37+scenario)*i/4096)/16;
                    if(scenario!=1) f.left[i]+=s;
                    if(scenario!=0) f.right[i]+=scenario==2?-s:s;
                }
            }
            auto l=f.left,r=f.right;std::vector<float> hann(4096),weights(2048),sl(2048),sr(2048);
            dsp_core::hann_window(hann);dsp_core::bark_weights(weights,48000.f/4096);
            dsp_core::StereoProcessor legacy;check(legacy.prepare(4096,48000),"Legacy prepare failed");
            check(legacy.process(l,r,hann,weights,sl,sr),"Legacy process failed");
            GraphEvaluationContext c;const auto out=p->evaluate(0,f,c);
            auto spectrum=out.views[5];check(spectrum.timestamp_ns==123456,"Origin timestamp lost");
            check(spectrum.left==sl && spectrum.right==sr,"Split stages changed legacy numbers");
            check(out.mel.size()==50 && !out.history.empty(),"Explicit DSP route failed");
            check(*std::max_element(out.mel.begin(),out.mel.end())>0,"Stereo/antiphase frequencies lost");
        }
        auto branches=graph({node(0,"NetworkInput",{{"data_type","mel"}}),node(1,"Spectrogram",{{"history_size",2}}),node(2,"SpectrogramView"),
            node(10,"NetworkInput",{{"data_type","mel"}}),node(11,"Spectrogram",{{"history_size",3}}),node(12,"SpectrogramView"),node(13,"MelView"),node(14,"NetworkOutput",{{"data_type","history"}})},
            {edge(0,1),edge(1,2),edge(10,11),edge(11,12),edge(10,13),edge(11,14)});
        auto b=GraphProgram::compile(branches,error);check(bool(b),qPrintable(error));check(!b->contains("StereoFFT"),"External Mel requires FFT");
        {
            auto fenced_graph=branches;auto fenced_nodes=fenced_graph["nodes"].toArray();fenced_nodes.append(node(3,"NetworkOutput",{{"data_type","history"}}));
            auto fenced_edges=fenced_graph["connections"].toArray();fenced_edges.append(edge(1,3));
            fenced_graph["nodes"]=fenced_nodes;fenced_graph["connections"]=fenced_edges;
            AudioPipeline fenced([](AudioSourceKind){return std::unique_ptr<IAudioSource>{};});check_apply(fenced,fenced_graph,error);
            int controls=0;
            fenced.subscribe_typed([&](int,TypedFrame output) {
                if(!output.available) {
                    ++controls;check(output.generation==2 && output.producer_epoch==100,"Forwarded marker lost producer/generation fence");
                }
            },std::vector<int>{3});
            TypedFrame data;data.type=FrameType::Mel;data.sample_rate=48000;data.epoch=100;data.generation=1;
            data.sequence=1;data.values={1,2,3};fenced.feed_typed(0,data);fenced.feed_typed(10,data);
            auto marker=data;marker.available=false;marker.sequence=0;marker.values.clear();marker.generation=2;
            fenced.feed_typed(0,marker);
            check(!fenced.latest_views()[2].available,"New generation marker did not make dependent view unavailable");
            data.sequence=2;fenced.feed_typed(0,data);
            check(!fenced.latest_views()[2].available,"Older generation revived history after accepted marker");
            check(fenced.latest_views()[12].available && fenced.latest_views()[12].history.size()==1,"Marker cleared an independent branch");
            data.generation=2;data.sequence=3;fenced.feed_typed(0,data);
            check(fenced.latest_views()[2].available && fenced.latest_views()[2].history.size()==1,"Current generation did not recover");
            marker.generation=1;fenced.feed_typed(0,marker);
            check(fenced.latest_views()[2].available,"Stale marker cleared current generation data");
            data.epoch=101;data.generation=1;data.sequence=1;fenced.feed_typed(0,data);
            check(fenced.latest_views()[2].history.back()==data.values,"New producer/geometry was rejected");
            marker.epoch=100;marker.generation=99;fenced.feed_typed(0,marker);
            check(fenced.latest_views()[2].available,"Retired producer marker cleared the new producer");
            check(controls==1,"Rejected or accepted marker emitted duplicate controls");
            data.sequence=2;data.values={4,5,6,7};fenced.feed_typed(0,data);
            check(fenced.latest_views()[2].history.back()==data.values,"Recovery geometry renegotiation was rejected");
        }
        GraphEvaluationContext c;TypedFrame f;f.type=FrameType::Mel;f.sample_rate=48000;f.generation=2;f.sequence=1;f.values={1,2,3};
        b->evaluate(0,f,c);f.sequence=2;auto a=b->evaluate(0,f,c);check(a.views[2].history.size()==2,"First history failed");
        f.sequence=1;f.values={4,5,6};auto second=b->evaluate(10,f,c);check(second.views[12].history.size()==1,"Histories share state");
        f.sequence=3;f.values={1,2,3};check(b->evaluate(0,f,c).views[2].history.size()==2,"Independent ingress reset another history");
        f.generation=1;f.sequence=10;check(b->evaluate(0,f,c).views.isEmpty(),"Old generation accepted");
        f.generation=2;f.sequence=4;f.discontinuity=true;check(b->evaluate(0,f,c).views[2].history.size()==1,"Discontinuity did not reset history");
        f.epoch=100;f.generation=1;f.sequence=1;f.discontinuity=true;
        check(b->evaluate(0,f,c).views[2].history.size()==1,"New producer epoch with sequence1 rejected");
        f.epoch=101;f.discontinuity=false;
        check(b->evaluate(0,f,c).views[2].history.size()==1,"Producer restart did not reset history");
        f.epoch=100;f.sequence=2;check(b->evaluate(0,f,c).views.isEmpty(),"Retired producer epoch accepted");
        f.epoch=0;f.generation=2;f.sequence=1;
        AudioPipeline runtime;check_apply(runtime,branches,error);
        const auto epoch=runtime.epoch();std::atomic<int> observer1{0},observer2{0};
        auto one=runtime.subscribe_typed([&](int,TypedFrame){++observer1;});auto two=runtime.subscribe_typed([&](int,TypedFrame){++observer2;});
        f.discontinuity=false;f.sequence=1;runtime.feed_typed(0,f,epoch);runtime.feed_typed(10,f,epoch);
        check(runtime.latest_views()[2].history.size()==1 && runtime.latest_views()[12].history.size()==1,"Per-node runtime views missing");
        check(observer1>0 && observer1==observer2,"Multiple observers failed");runtime.unsubscribe_typed(one);int prior=observer1;
        f.sequence=2;runtime.feed_typed(0,f,epoch);check(observer1==prior && observer2>prior,"Unsubscribe failed");
        auto moved=branches;auto moved_nodes=moved["nodes"].toArray();auto moved_node=moved_nodes[0].toObject();
        moved_node["position"]=QJsonObject{{"x",99},{"y",123}};moved_nodes[0]=moved_node;moved["nodes"]=moved_nodes;
        check(runtime.apply_graph(moved,error) && runtime.epoch()==epoch,"Layout apply changed epoch");
        f.sequence=3;runtime.feed_typed(0,f,epoch);check(runtime.latest_views()[2].history.size()==2,"Layout apply reset node history");
        std::atomic<int> unavailable{0};auto marker_token=runtime.subscribe_typed([&](int id,TypedFrame frame){
            if(id==14 && !frame.available) {check(frame.history.empty() && frame.values.empty(),"Unavailable marker has data");++unavailable;}
        });
        runtime.clear_ingress(10,epoch);
        check(!runtime.latest_views()[12].available && runtime.latest_views()[12].history.empty() && runtime.latest_views()[2].history.size()==2,
            "Disconnect clear affected independent branch or retained stale history");
        check(unavailable==1,"Disconnect did not propagate to output observer");
        f.sequence=2;runtime.feed_typed(10,f,epoch);check(runtime.latest_views()[12].available && runtime.latest_views()[12].history.size()==1,
            "Ingress did not resume with fresh history");
        TypedFrame missing;missing.available=false;runtime.feed_typed(10,missing,epoch);
        check(!runtime.latest_views()[12].available && unavailable==2,"Unavailable input did not propagate clear");
        runtime.unsubscribe_typed(marker_token);
        std::mutex gate_mutex;std::condition_variable gate;bool entered=false,released=false;
        std::atomic<bool> cleared{false};
        const auto old_serial=runtime.ingress_serial(10);
        auto blocking=runtime.subscribe_typed([&](int id,TypedFrame frame){
            if(id!=14 || !frame.available) return;
            std::unique_lock lock(gate_mutex);entered=true;gate.notify_all();gate.wait(lock,[&]{return released;});
        });
        f.sequence=10;
        std::thread in_flight([&]{runtime.feed_typed(10,f,epoch,old_serial);});
        {std::unique_lock lock(gate_mutex);check(gate.wait_for(lock,std::chrono::seconds(1),[&]{return entered;}),"No in-flight output");}
        std::thread clear([&]{runtime.clear_ingress(10,epoch);cleared=true;});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));check(!cleared,"Clear returned while old callback was in flight");
        {std::lock_guard lock(gate_mutex);released=true;}gate.notify_all();in_flight.join();clear.join();runtime.unsubscribe_typed(blocking);
        check(!runtime.latest_views()[12].available,"In-flight data revived cleared view");
        f.sequence=11;runtime.feed_typed(10,f,epoch,old_serial);
        check(!runtime.latest_views()[12].available,"Queued old serial revived cleared view");
        runtime.feed_typed(10,f,epoch,runtime.ingress_serial(10));
        check(runtime.latest_views()[12].available && runtime.latest_views()[12].history.size()==1,"Fresh ingress serial did not resume");
        auto disconnected=branches;auto edges=disconnected["connections"].toArray();edges.removeAt(0);disconnected["connections"]=edges;
        const auto retained_history=runtime.latest_views()[2].history;
        check(runtime.apply_graph(disconnected,error),"Disconnected graph rejected");
        check(runtime.latest_views()[2].history==retained_history,"Disconnect Apply reset compatible history");
        const int delivered=observer2;f.sequence=12;runtime.feed_typed(10,f,epoch);check(observer2==delivered,"Old binding crossed apply boundary");
        runtime.feed_typed(10,f,runtime.epoch());check(runtime.latest_views()[12].history.size()==2,"Independent branch did not preserve/continue history after disconnect");
        f.sequence=4;runtime.feed_typed(0,f,runtime.epoch());const auto zero_tick=runtime.latest_views()[2];
        check(zero_tick.available && zero_tick.history.size()==2 && zero_tick.history.front()==retained_history.back() &&
            zero_tick.history.back().size()==f.values.size() && std::all_of(zero_tick.history.back().begin(),zero_tick.history.back().end(),[](float v){return v==0;}),
            "Disconnected history did not retain rows and append a dimensioned zero tick");
        runtime.unsubscribe_typed(two);
        auto history_only=graph({node(0,"NetworkInput",{{"data_type","history"}}),node(1,"SpectrogramView")},{edge(0,1)});
        check_apply(runtime,history_only,error);f.type=FrameType::History;f.history={{1,2},{3,4}};
        runtime.feed_typed(0,f);check(runtime.latest_views()[1].history==f.history,"History ingress performed earlier stages");
        int capture_opens=0;
        AudioPipeline remote_only([&](AudioSourceKind){++capture_opens;return std::unique_ptr<IAudioSource>{};});
        check_apply(remote_only,history_only,error);remote_only.start_analyzer();remote_only.start_capture();
        check(capture_opens==0,"History-only runtime opened a capture adapter");remote_only.stop_analyzer();
        {
            QUdpSocket receiver;check(receiver.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)),"WLED test bind failed");
            AudioPipeline lights;
            auto outputs=graph({node(0,"NetworkInput",{{"data_type","mel"}}),node(1,"WLED"),node(2,"MelView"),
                node(10,"NetworkInput",{{"data_type","mel"}}),node(11,"Spectrogram"),node(12,"SpectrogramView")},
                {edge(0,1),edge(0,2),edge(10,11),edge(11,12)});
            check_apply(lights,outputs,error);lights.start_analyzer();
            auto duplicate_wled=outputs;auto duplicate_nodes=duplicate_wled["nodes"].toArray();duplicate_nodes.append(node(20,"WLED"));duplicate_wled["nodes"]=duplicate_nodes;
            check(!GraphProgram::compile(duplicate_wled,error) && error.contains("WLED"),"Additional physical WLED sink was silently accepted");
            lights.start_wled(QHostAddress::LocalHost,receiver.localPort(),QHostAddress::LocalHost);
            auto wait_packet=[&](bool silent,int timeout,float peak_hz=750) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
                while(std::chrono::steady_clock::now()<deadline) {
                    if(!receiver.hasPendingDatagrams()) receiver.waitForReadyRead(10);
                    while(receiver.hasPendingDatagrams()) {
                        QByteArray bytes(44,0);receiver.readDatagram(bytes.data(),bytes.size());
                        wled_audio_reactive::AudioSyncPacket packet{};std::memcpy(&packet,bytes.constData(),sizeof(packet));
                        if(silent ? packet.sampleRaw==0 && packet.FFT_MajorPeak==0 : packet.sampleRaw>0 && packet.FFT_MajorPeak==peak_hz) return true;
                    }
                }
                return false;
            };
            auto drain=[&]{while(receiver.hasPendingDatagrams()){QByteArray bytes(44,0);receiver.readDatagram(bytes.data(),bytes.size());}};
            TypedFrame m;m.type=FrameType::Mel;m.sample_rate=48000;m.values.assign(50,1);m.generation=1;m.sequence=1;m.peak_hz=750;m.magnitude=1;
            lights.feed_typed(0,m);check(wait_packet(false,200),"WLED typed branch did not run without GUI");
            for(int bands:{10,25}) {
                ++m.sequence;m.values.assign(bands,1);m.peak_hz=bands==10?1100:1500;drain();lights.feed_typed(0,m);
                check(wait_packet(false,200,m.peak_hz),"Non-default Mel bands stopped WLED or changed Hz peak");
                check(lights.latest_views()[2].values.size()==size_t(bands),"WLED formatting mutated graph Mel dimension");
            }
            ++m.sequence;m.values.assign(50,1);m.peak_hz=750;drain();lights.feed_typed(0,m);
            check(wait_packet(false,200),"WLED did not restore default Mel bands");
            lights.feed_typed(10,m);drain();lights.clear_ingress(10);
            check(wait_packet(false,100) && lights.current_status("WLED").state==State::Running,"Unrelated clear stopped/zeroed WLED");
            ++m.sequence;lights.feed_typed(10,m);drain();lights.clear_ingress(0);
            check(wait_packet(true,100),"WLED unavailable did not immediately publish silence");
            check(lights.latest_views()[12].available && lights.latest_views()[12].history.size()==1,"WLED clear affected independent history");
            m.sequence=100;lights.feed_typed(0,m);check(wait_packet(false,200),"WLED did not recover for disable check");drain();
            auto disabled=outputs;auto disabled_nodes=disabled["nodes"].toArray();
            for(int i=0;i<disabled_nodes.size();++i) {auto n=disabled_nodes[i].toObject();if(n["internal-data"].toObject()["model-name"]!="WLED") continue;
                auto params=n["internal-data"].toObject();params["enabled"]=false;n["internal-data"]=params;disabled_nodes[i]=n;}
            disabled["nodes"]=disabled_nodes;const auto prior_epoch=lights.epoch();
            check_apply(lights,disabled,error);check(wait_packet(true,100),"Disabled WLED did not immediately silence");
            check_apply(lights,outputs,error);drain();
            lights.feed_typed(0,m,prior_epoch);check(!wait_packet(false,40),"Old apply epoch crossed reenable");
            ++m.sequence;lights.feed_typed(0,m,lights.epoch());check(wait_packet(false,200),"Reenabled WLED did not resume");
            lights.stop_wled();lights.stop_analyzer();
        }
        auto cadence_graph=graph({node(0,"NetworkInput",{{"data_type","audio"}}),node(1,"Window",{{"size",512}}),node(2,"WindowView")},{edge(0,1),edge(1,2)});
        auto cp=GraphProgram::compile(cadence_graph,error);check(bool(cp),qPrintable(error));GraphEvaluationContext cadence;
        f=TypedFrame{};f.sample_rate=44100;f.left.assign(147,0.1);f.right=f.left;int frames=0;
        for(int i=0;i<300;++i) {f.sequence=i+1;if(cp->evaluate(0,f,cadence).views.contains(2)) ++frames;}
        check(frames==60,"Window cadence is not 60Hz");
        auto middle=graph({node(0,"NetworkInput",{{"data_type","audio"}}),node(1,"Window",{{"size",512}}),node(2,"WindowFunction"),
            node(3,"StereoFFT"),node(4,"BarkWeighting"),node(5,"SpectrumMerge"),node(6,"Mel"),node(7,"NetworkOutput",{{"data_type","mel"}})},
            {edge(0,1),edge(1,2),edge(2,3),edge(3,4),edge(4,5),edge(5,6),edge(6,7)});
        auto mp=GraphProgram::compile(middle,error);check(bool(mp),qPrintable(error));GraphEvaluationContext mc;
        auto downstream=graph({node(0,"NetworkInput",{{"data_type","mel"}}),node(1,"Spectrogram"),node(2,"SpectrogramView")},{edge(0,1),edge(1,2)});
        auto dp=GraphProgram::compile(downstream,error);check(bool(dp),qPrintable(error));GraphEvaluationContext dc;
        f=TypedFrame{};f.sample_rate=48000;f.left.assign(4096,0.1);f.right=f.left;f.sequence=1;f.generation=1;f.epoch=100;
        auto bulk=mp->evaluate(0,f,mc);check(bulk.output_events.size()==5,"Large PCM block lost output events");
        uint64_t previous=0;GraphOutput history_result;
        for(auto [id,event]:bulk.output_events) {
            check(event.sequence==++previous,"Window output sequence is not consecutive");
            event.producer_epoch=0;event.epoch=200;history_result=dp->evaluate(0,event,dc);
        }
        check(history_result.views[2].history.size()==5,"Downstream dropped same-block windows");
        f.epoch=101;bulk=mp->evaluate(0,f,mc);check(bulk.output_events.size()==5,"Producer restart PCM was rejected");
        check(bulk.output_events.front().second.discontinuity,"Producer restart lost discontinuity");
        for(auto [id,event]:bulk.output_events) {
            check(event.sequence==++previous,"Upstream restart reset live output sequence");
            event.producer_epoch=0;event.epoch=200;history_result=dp->evaluate(0,event,dc);
        }
        check(history_result.views[2].history.size()==5,"B→C restart did not reset downstream history");
        f.epoch=100;f.sequence=2;check(mp->evaluate(0,f,mc).output_events.empty(),"Retired PCM producer crossed restart");
        mp->clear_ingress(0,mc);dp->clear_ingress(0,dc);
        f.epoch=101;bulk=mp->evaluate(0,f,mc);
        check(bulk.output_events.front().second.sequence==previous+1 && bulk.output_events.front().second.discontinuity,
            "Unavailable/resume reset output counter or omitted gap");
        auto pass_pcm=graph({node(0,"NetworkInput",{{"data_type","audio"}}),node(1,"NetworkOutput",{{"data_type","audio"}})},{edge(0,1)});
        auto pass=GraphProgram::compile(pass_pcm,error);GraphEvaluationContext pc;
        f.left.resize(240);f.right.resize(240);auto raw=pass->evaluate(0,f,pc);
        check(raw.output_events.size()==1 && raw.output_events[0].second.type==FrameType::PCM && raw.output_events[0].second.left.size()==240,
            "Audio egress converted source PCM to a window");
        // Two real silent producer nodes run without servicing the GUI event loop.
        auto local=graph({node(0,"TestSignal"),node(1,"Window"),node(2,"WindowFunction"),node(3,"StereoFFT"),node(4,"FFTView"),
            node(5,"BarkWeighting"),node(6,"SpectrumMerge"),node(7,"Mel"),node(8,"Spectrogram"),node(9,"SpectrogramView"),
            node(10,"TestSignal",{{"amplitude",0.15}}),node(11,"Window"),node(12,"WindowFunction"),node(13,"StereoFFT"),node(14,"FFTView"),
            node(15,"BarkWeighting"),node(16,"SpectrumMerge"),node(17,"Mel"),node(18,"Spectrogram"),node(19,"SpectrogramView")},
            {edge(0,1),edge(1,2),edge(2,3),edge(3,4),edge(3,5),edge(5,6),edge(6,7),edge(7,8),edge(8,9),
             edge(10,11),edge(11,12),edge(12,13),edge(13,14),edge(13,15),edge(15,16),edge(16,17),edge(17,18),edge(18,19)});
        check_apply(runtime,local,error);runtime.start_analyzer();runtime.start_capture();
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const auto views=runtime.latest_views();check(views[4].sequence>=250 && views[14].sequence>=250,"Multiple local producers depend on GUI/one source");
        check(views[9].history.size()>=250 && views[19].history.size()>=250 && views[9].history.back()!=views[19].history.back(),
            "Multiple local producers share history/data");
        runtime.stop_capture();runtime.stop_analyzer();
        qInfo("Typed stages, numeric parity, ingress, independent histories/views, epoch fencing and 5s headless producers passed");
    } catch(const std::exception &e) { qCritical("Typed graph: %s",e.what());return 1; }
}
