#include <pipeline/audio_pipeline.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <iostream>

static thread_local bool measuring=false;
static thread_local std::size_t allocated_bytes=0;
static thread_local std::size_t allocation_count=0;
void *operator new(std::size_t size) {
    if(measuring){allocated_bytes+=size;++allocation_count;}
    if(auto p=std::malloc(size))return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept {std::free(p);}
void operator delete(void *p,std::size_t) noexcept {std::free(p);}
using namespace pipeline;
static void measure_idle(bool history) {
    AudioPipeline engine([](AudioSourceKind){return std::unique_ptr<IAudioSource>{};});
    QJsonArray nodes{
        QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","mel"},{"stream_id","idle"}}}},
        QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name",history?"Spectrogram":"MelView"},{"history_size",8}}}}};
    QJsonArray edges{QJsonObject{{"outNodeId",0},{"inNodeId",1},{"outPortIndex",0},{"inPortIndex",0}}};
    if(history) {
        nodes.append(QJsonObject{{"id",2},{"internal-data",QJsonObject{{"model-name","SpectrogramView"}}}});
        edges.append(QJsonObject{{"outNodeId",1},{"inNodeId",2},{"outPortIndex",0},{"inPortIndex",0}});
    }
    QString error;
    if(!engine.apply_graph(QJsonObject{{"version",2},{"nodes",nodes},{"connections",edges}},error))
        throw std::runtime_error(error.toStdString());
    engine.set_view_subscription(0);
    TypedFrame frame;frame.type=FrameType::Mel;frame.sample_rate=48000;frame.generation=1;frame.epoch=10;
    frame.values.assign(50,.25f);
    for(int i=1;i<=3;++i){frame.sequence=i;engine.feed_typed(0,frame);}
    const auto retained=engine.latest_views(); // owned control-plane snapshot must stay unchanged
    allocated_bytes=allocation_count=0;
    measuring=true;
    for(int i=4;i<=12;++i){frame.sequence=i;engine.feed_typed(0,frame);}
    measuring=false;
    std::cout<<"Idle "<<(history?"history":"Mel")<<" allocations="<<allocation_count<<", bytes="<<allocated_bytes<<'\n';
    if(allocation_count)throw std::runtime_error("Unchanged unsubscribed ingress allocated during processing");
    const auto view=history?2:1;
    const auto current=engine.latest_views();
    if(retained.value(view).sequence!=3 || current.value(view).sequence!=12 ||
       (history && current.value(view).history.size()!=8))
        throw std::runtime_error("Idle processing lost latest state or changed a retained snapshot");
    if(frame.sequence!=12 || frame.epoch!=10 || frame.generation!=1 || frame.values!=std::vector<float>(50,.25f))
        throw std::runtime_error("Borrowed input was changed during synchronous processing");
    frame.values.assign(32,.5f);frame.sequence=13;engine.feed_typed(0,frame);
    allocated_bytes=allocation_count=0;frame.sequence=14;measuring=true;engine.feed_typed(0,frame);measuring=false;
    if(allocation_count)throw std::runtime_error("Renegotiated geometry did not return to allocation-free processing");
    frame.values.clear();
    const auto resized=engine.latest_views().value(view);
    if(resized.values!=std::vector<float>(32,.5f) || (history && resized.history.back()!=resized.values))
        throw std::runtime_error("View cache borrowed caller storage beyond return");
    TypedFrame delivered;
    engine.subscribe_typed([&](int id,TypedFrame owned){
        if(id!=0)throw std::runtime_error("Selected subscriber received another node");
        delivered=std::move(owned);
    },std::vector<int>{0});
    frame.values.assign(16,.75f);frame.sequence=15;engine.feed_typed(0,frame);frame.values.clear();
    const auto retained_delivery=delivered;
    if(delivered.values!=std::vector<float>(16,.75f))throw std::runtime_error("Typed subscriber retained borrowed input");
    frame.values.assign(16,.9f);frame.sequence=16;engine.feed_typed(0,frame);
    if(retained_delivery.values!=std::vector<float>(16,.75f) || delivered.values!=frame.values ||
       engine.latest_views().value(view).values!=frame.values)
        throw std::runtime_error("Owned subscriber snapshot changed across the next processing tick");
}
static void measure_history_input() {
    AudioPipeline engine([](AudioSourceKind){return std::unique_ptr<IAudioSource>{};});
    QString error;
    QJsonObject graph{{"version",2},{"nodes",QJsonArray{
        QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","history"},{"stream_id","idle-history"}}}},
        QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","SpectrogramView"}}}}}},
        {"connections",QJsonArray{QJsonObject{{"outNodeId",0},{"inNodeId",1},{"outPortIndex",0},{"inPortIndex",0}}}}};
    if(!engine.apply_graph(graph,error))throw std::runtime_error(error.toStdString());
    engine.set_view_subscription(0);
    TypedFrame frame;frame.type=FrameType::History;frame.sample_rate=48000;frame.generation=1;frame.epoch=10;
    frame.history.assign(4,std::vector<float>(50,.25f));
    for(int i=1;i<=3;++i){frame.sequence=i;engine.feed_typed(0,frame);}
    const auto retained=engine.latest_views();
    allocated_bytes=allocation_count=0;measuring=true;
    for(int i=4;i<=12;++i){frame.sequence=i;engine.feed_typed(0,frame);}
    measuring=false;
    std::cout<<"Idle History ingress allocations="<<allocation_count<<", bytes="<<allocated_bytes<<'\n';
    if(allocation_count)throw std::runtime_error("Unchanged History ingress allocated during processing");
    const auto payload=frame.history;frame.history.clear();
    const auto current=engine.latest_views().value(1);
    if(current.sequence!=12 || current.history!=payload || retained.value(1).sequence!=3)
        throw std::runtime_error("History ingress cache retained borrowed row storage");
}
static std::size_t measure(int capacity) {
    AudioPipeline engine([](AudioSourceKind)->std::unique_ptr<IAudioSource>{throw std::runtime_error("Unexpected physical capture");});
    const QJsonObject graph{{"version",2},{"nodes",QJsonArray{
        QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","mel"},{"stream_id","history-demand"}}}},
        QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","Spectrogram"},{"history_size",capacity}}}},
        QJsonObject{{"id",2},{"internal-data",QJsonObject{{"model-name","SpectrogramView"}}}}}},
        {"connections",QJsonArray{
            QJsonObject{{"outNodeId",0},{"outPortIndex",0},{"inNodeId",1},{"inPortIndex",0}},
            QJsonObject{{"outNodeId",1},{"outPortIndex",0},{"inNodeId",2},{"inPortIndex",0}}}}};
    QString error;
    if(!engine.apply_graph(graph,error))throw std::runtime_error(error.toStdString());
    int routed=0;
    engine.subscribe_typed([&](int id,TypedFrame frame){
        if(id!=0 || frame.type!=FrameType::Mel)throw std::runtime_error("Unrequested history was routed");
        ++routed;
    },std::vector<int>{0});
    TypedFrame frame;frame.type=FrameType::Mel;frame.sample_rate=48000;
    frame.values.assign(50,.25f);frame.generation=1;frame.epoch=10;
    for(int i=1;i<=400;++i){frame.sequence=i;engine.feed_typed(0,frame);}
    const auto before=engine.latest_views().value(2);
    if(before.history.size()!=std::size_t(capacity))throw std::runtime_error("History warmup failed");
    frame.sequence=401;allocated_bytes=0;measuring=true;
    engine.feed_typed(0,frame);
    measuring=false;
    const auto bytes=allocated_bytes;
    if(routed!=401)throw std::runtime_error("Selected output subscription lost a frame");
    const auto after=engine.latest_views().value(2);
    if(after.history.size()!=std::size_t(capacity) || after.sequence!=401)
        throw std::runtime_error("On-demand snapshot did not expose latest history");
    return bytes;
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        measure_idle(false);measure_idle(true);measure_history_input();
        const auto small=measure(8),large=measure(400);
        std::cout<<"Processing allocations: small="<<small<<", large="<<large<<'\n';
        if(large>small+4096)throw std::runtime_error("Processing copied history proportional to retained rows");
        return 0;
    }catch(const std::exception &e){measuring=false;std::cerr<<e.what()<<'\n';return 1;}
}
