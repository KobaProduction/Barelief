#include "pipeline/audio_pipeline.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <stdexcept>
using namespace pipeline;
static void check(bool ok,const char *message) {if(!ok) throw std::runtime_error(message);}
static QJsonObject node(int id,const char *kind,QJsonObject params={}) {
    params["model-name"]=kind;
    if(QString(kind).startsWith("Network")) params["stream_id"]=QString::number(id);
    return {{"id",id},{"internal-data",params}};
}
static QJsonObject edge(int from,int to) {return {{"outNodeId",from},{"outPortIndex",0},{"inNodeId",to},{"inPortIndex",0}};}
static void parameter(QJsonObject &document,int id,const char *key,QJsonValue value) {
    auto nodes=document["nodes"].toArray();
    for(int i=0;i<nodes.size();++i) {auto n=nodes[i].toObject();if(n["id"].toInt()!=id) continue;
        auto params=n["internal-data"].toObject();params[key]=value;n["internal-data"]=params;nodes[i]=n;}
    document["nodes"]=nodes;
}
static bool zero(const std::vector<float> &values) {return !values.empty() && std::all_of(values.begin(),values.end(),[](float x){return x==0;});}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        const QJsonObject connected{{"version",2},{"custom",QJsonObject{{"preserved",true}}},{"nodes",QJsonArray{
            node(0,"NetworkInput",{{"data_type","audio"}}),node(1,"Window",{{"size",512}}),node(2,"WindowFunction"),
            node(3,"StereoFFT"),node(4,"SpectrumMerge"),node(5,"Mel",{{"bands",3}}),node(6,"Spectrogram",{{"history_size",8}}),
            node(7,"SpectrogramView"),node(8,"NetworkOutput",{{"data_type","history"}}),node(9,"WLED"),
            node(10,"NetworkInput",{{"data_type","mel"}}),node(11,"Spectrogram",{{"history_size",8}}),node(12,"SpectrogramView")}},
            {"connections",QJsonArray{edge(0,1),edge(1,2),edge(2,3),edge(3,4),edge(4,5),edge(5,6),edge(6,7),edge(6,8),edge(5,9),edge(10,11),edge(11,12)}}};
        AudioPipeline runtime([](AudioSourceKind)->std::unique_ptr<IAudioSource>{throw std::runtime_error("Unit opened an audio device");});
        QString error;check(runtime.apply_graph(connected,error),qPrintable(error));
        TypedFrame pcm;pcm.type=FrameType::PCM;pcm.sample_rate=48000;pcm.generation=7;pcm.epoch=900;
        pcm.left.resize(800);for(size_t i=0;i<pcm.left.size();++i) pcm.left[i]=std::sin(2*3.141592653589793*43*i/512);pcm.right=pcm.left;
        TypedFrame other;other.type=FrameType::Mel;other.sample_rate=48000;other.generation=1;other.epoch=800;other.values={4,5,6};
        for(int i=1;i<=2;++i) {pcm.sequence=i;pcm.timestamp_ns=i*16666667LL;runtime.feed_typed(0,pcm);other.sequence=i;runtime.feed_typed(10,other);}
        const auto initial=runtime.latest_views()[7];check(initial.history.size()==2 && !zero(initial.history.back()),"Initial nonzero history missing");
        auto disconnected=connected;auto edges=disconnected["connections"].toArray();edges.removeAt(1);disconnected["connections"]=edges;
        const auto old_epoch=runtime.epoch();check(runtime.apply_graph(disconnected,error),qPrintable(error));
        check(runtime.latest_views()[7].history==initial.history,"Disconnect Apply reset history");
        GraphOutput events;auto token=runtime.subscribe_typed([&](int id,TypedFrame f){events.ports[id]=std::move(f);});
        for(int i=3;i<=4;++i) {pcm.sequence=i;pcm.timestamp_ns=i*16666667LL;runtime.feed_typed(0,pcm,runtime.epoch());}
        const auto silent=runtime.latest_views()[7];
        check(silent.available && silent.history_capacity==8 && silent.history.size()==4 && silent.history[0]==initial.history[0],"Silence lost capacity or previous rows");
        check(zero(silent.history[2]) && zero(silent.history[3]) && silent.history[3].size()==3,"Disconnect did not append dimensioned zeros");
        check(silent.timestamp_ns>initial.timestamp_ns && silent.sequence>initial.sequence,"Disconnect stopped the source clock");
        check(events.ports[8].available && zero(events.ports[8].history.back()) && zero(events.ports[9].values),"Network/WLED silence contract missing");
        check(runtime.latest_views()[12].history.size()==2,"Unrelated branch changed on disconnect");
        check(runtime.apply_graph(connected,error),qPrintable(error));pcm.sequence=5;pcm.timestamp_ns=5*16666667LL;
        runtime.feed_typed(0,pcm,old_epoch);check(runtime.latest_views()[7].history.size()==4,"Old receiver epoch crossed reconnect");
        runtime.feed_typed(0,pcm,runtime.epoch());const auto resumed=runtime.latest_views()[7];
        check(resumed.history.size()==5 && resumed.history[0]==initial.history[0] && !zero(resumed.history.back()),"Reconnect lost history or nonzero values");
        auto disabled=connected;parameter(disabled,2,"enabled",false);check(runtime.apply_graph(disabled,error),qPrintable(error));
        pcm.sequence=6;runtime.feed_typed(0,pcm);check(runtime.latest_views()[7].history.size()==6 && zero(runtime.latest_views()[7].history.back()),"Gate did not append silence");
        check(runtime.apply_graph(connected,error),qPrintable(error));pcm.sequence=7;runtime.feed_typed(0,pcm);
        check(runtime.latest_views()[7].history.size()==7 && !zero(runtime.latest_views()[7].history.back()),"Reenable reset history");
        auto dimensions=connected;parameter(dimensions,5,"bands",4);check(runtime.apply_graph(dimensions,error),qPrintable(error));
        pcm.sequence=8;runtime.feed_typed(0,pcm);check(runtime.latest_views()[7].history.size()==1 && runtime.latest_views()[7].history.back().size()==4,"Dimension change did not reset incompatible history");
        check(runtime.latest_views()[12].history.size()==2,"Dimension change reset unrelated history");
        auto invalid=connected;invalid["connections"]=QJsonArray{edge(0,5)};const auto epoch=runtime.epoch();
        check(!runtime.apply_graph(invalid,error) && runtime.epoch()==epoch,"Invalid graph was executed");
        check(runtime.graph()["custom"]==connected["custom"],"Unknown metadata lost");
        runtime.unsubscribe_typed(token);
        // Pure evaluator: external Mel establishes dimensions even when its
        // optional bands field is missing or stale. Only width changes reset.
        const QJsonObject external{{"version",2},{"nodes",QJsonArray{
            node(0,"NetworkInput",{{"data_type","mel"}}),node(1,"Spectrogram",{{"history_size",4}}),node(2,"SpectrogramView"),
            node(10,"NetworkInput",{{"data_type","mel"}}),node(11,"Spectrogram"),node(12,"SpectrogramView")}},
            {"connections",QJsonArray{edge(0,1),edge(1,2),edge(10,11),edge(11,12)}}};
        const auto program=GraphProgram::compile(external,error);check(bool(program),qPrintable(error));GraphEvaluationContext context;
        TypedFrame mel;mel.type=FrameType::Mel;mel.sample_rate=48000;mel.epoch=300;mel.sequence=1;mel.values={1,2,3};mel.timestamp_ns=100;
        auto metadata=program->evaluate(0,mel,context).views[2];
        check(metadata.bands==3 && metadata.history_capacity==4 && metadata.sequence==1 && metadata.timestamp_ns==100,"External Mel history metadata disagrees with rows");
        program->evaluate(10,mel,context);mel.sequence=2;mel.timestamp_ns=200;mel.bands=99;
        metadata=program->evaluate(0,mel,context).views[2];
        check(metadata.bands==3 && metadata.history.size()==2 && !metadata.discontinuity && metadata.history.front()==std::vector<float>({1,2,3}),"Ordinary frame reset history or trusted stale bands");
        mel.sequence=3;mel.values={4,5};metadata=program->evaluate(0,mel,context).views[2];
        check(metadata.bands==2 && metadata.history.size()==1 && metadata.discontinuity,"External width change mixed incompatible history rows");
        mel.sequence=2;mel.values={1,2,3};metadata=program->evaluate(10,mel,context).views[12];
        check(metadata.bands==3 && metadata.history.size()==2 && !metadata.discontinuity,"External width change reset unrelated history");
        qInfo("Disconnect/ticks/reconnect, compatible history, zero contracts, dimensions and unrelated branches passed");
    } catch(const std::exception &error) {qCritical("Continuity: %s",error.what());return 1;}
}
