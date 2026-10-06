#include "pipeline/audio_pipeline.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QDebug>
#include <thread>
#include <stdexcept>
#include <algorithm>
using namespace pipeline;
static void check(bool ok,const char *why) { if(!ok) throw std::runtime_error(why); }
template<class Predicate> static void wait_for(Predicate predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!predicate()) { check(std::chrono::steady_clock::now()<deadline,"Default runtime did not become ready");std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        QString error;const auto document=GraphProgram::defaults();const auto plan=GraphProgram::compile(document,error);
        check(plan && plan->capture_source()==0 && plan->contains("AudioSource") && !plan->contains("TestSignal"),"Default did not select real system audio");
        check(plan->contains("Window") && plan->contains("WindowFunction") && plan->contains("StereoFFT") &&
            plan->contains("BarkWeighting") && plan->contains("Mel") && plan->contains("SpectrogramView"),"Default processing chain missing");
        int opens=0;bool fail_open=false;const auto fake=default_audio_source_factory();
        // Verify the real SystemOutput selection without opening a physical driver.
        // The injected adapter generates eight changing tones in memory only.
        AudioPipeline runtime([&](AudioSourceKind requested) {
            check(requested==AudioSourceKind::SystemOutput,"Default selected microphone/test source");++opens;
            if(fail_open) return std::unique_ptr<IAudioSource>{};
            return fake(AudioSourceKind::TestSignal);
        });
        check(runtime.graph()==document && opens==0,"Constructor opened an audio source");
        check(runtime.apply_graph(document,error) && opens==1,"Valid first Apply did not start enabled source");
        wait_for([&]{return runtime.history_count()>=3;});check(opens==1,"Default did not start its audio source");
        auto views=runtime.latest_views();check(views[7].available && !views[7].history.empty(),"Default did not deliver history view");
        const QJsonObject remote{{"version",2},{"nodes",QJsonArray{
            QJsonObject{{"id",0},{"internal-data",QJsonObject{{"model-name","NetworkInput"},{"data_type","history"},{"stream_id","remote"}}}},
            QJsonObject{{"id",1},{"internal-data",QJsonObject{{"model-name","SpectrogramView"}}}}}},
            {"connections",QJsonArray{QJsonObject{{"outNodeId",0},{"outPortIndex",0},{"inNodeId",1},{"inPortIndex",0}}}}};
        check(runtime.apply_graph(remote,error),qPrintable(error));check(opens==1,"Remote graph opened a local device");
        check(runtime.apply_graph(document,error),qPrintable(error));
        wait_for([&]{return runtime.history_count()>=3;});check(opens==2,"Returning to Default lost capture start intent");
        runtime.stop_capture();runtime.stop_analyzer();
        check(runtime.apply_graph(remote,error) && runtime.apply_graph(document,error) && opens==2,"Applying Default ignored explicit Stop");
        // Start after the successful ACK is the GUI/controller's explicit action.
        runtime.start_analyzer();runtime.start_capture();wait_for([&]{return runtime.history_count()>=2;});check(opens==3,"Apply-and-start Default failed");
        runtime.stop_capture();runtime.stop_analyzer();fail_open=true;
        runtime.start_analyzer();runtime.start_capture();check(opens==4 && runtime.current_status("Capture").state==State::Failed,"Fake failed adapter did not fail");
        check(runtime.apply_graph(remote,error),qPrintable(error));fail_open=false;
        check(runtime.apply_graph(document,error),qPrintable(error));wait_for([&]{return runtime.history_count()>=2;});
        check(opens==5,"Failed/null adapter lost capture start intent");
        auto disabled=document;auto disabled_nodes=disabled["nodes"].toArray();auto disabled_source=disabled_nodes[0].toObject();
        auto disabled_params=disabled_source["internal-data"].toObject();disabled_params["enabled"]=false;disabled_source["internal-data"]=disabled_params;
        disabled_nodes[0]=disabled_source;disabled["nodes"]=disabled_nodes;
        check(runtime.apply_graph(disabled,error) && opens==5,"Disabled source opened a device");
        wait_for([&]{const auto view=runtime.latest_views()[7];return view.available && !view.history.empty() && std::all_of(view.history.back().begin(),view.history.back().end(),[](float v){return v==0;});});
        check(runtime.apply_graph(document,error),qPrintable(error));wait_for([&]{return runtime.history_count()>=2;});check(opens==5,"Reenabled gate unnecessarily reopened source");
        auto moved=document;auto moved_nodes=moved["nodes"].toArray();auto moved_source=moved_nodes[0].toObject();moved_source["position"]=QJsonObject{{"x",42},{"y",13}};moved_nodes[0]=moved_source;moved["nodes"]=moved_nodes;
        const auto epoch=runtime.epoch();check(runtime.apply_graph(moved,error) && opens==5 && runtime.epoch()==epoch,"Coordinates restarted source");
        runtime.stop_capture();runtime.stop_analyzer();
        qInfo("Connected real-source default, ready history, preset return/start/stop intent and null adapter recovery passed");
    } catch(const std::exception &e) {qCritical("Default runtime: %s",e.what());return 1;}
}
