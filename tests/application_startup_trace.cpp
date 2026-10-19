#include "synthetic_capture.h"
#include "performance_report.h"
#include "renderer_probe_telemetry.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/graph_panel.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <barelief/numeric_piano_view.h>
#include <QEvent>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <pipeline/numeric_engine.h>
#include <core_app/gui_event_profile.h>
#include <core_app/workspace/workspace_window.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <barelief/abstract/abs.h>
#include <barelief/quick2d/vector_plot_item.h>
#include <barelief/quick3d/surface_quick_item.h>
#include <barelief/quick3d/surface_overlay_item.h>
#include <QMatrix4x4>
#include <barelief/quick/quick_ui.h>
#include <barelief/render_preparation_policy.h>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QPointer>
#include <QQmlProperty>
#include <QSurfaceFormat>
#include <QScreen>
#include <QTimer>
#include <QFileInfo>
#include <atomic>
#include <memory>
#include <vector>
#include <cstdio>
#include <cmath>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

// Full production composition probe. All writes belong to the evidence directory;
// physical capture is replaced at its existing adapter injection boundary.
namespace {
class PianoPaintWitness final:public QObject {
public:
    bool* measuring=nullptr;QHash<QObject*,quint64> counts;
    bool eventFilter(QObject* object,QEvent* event)override {
        if(measuring && *measuring && event->type()==QEvent::Paint)++counts[object];
        return false;
    }
};
QJsonObject processMemory() {
#ifdef Q_OS_WIN
    using Query=BOOL(WINAPI*)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
    const auto module=GetModuleHandleW(L"kernel32.dll");
    const auto query=module?reinterpret_cast<Query>(GetProcAddress(module,"K32GetProcessMemoryInfo")):nullptr;
    PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
    if(query && query(GetCurrentProcess(),reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters),sizeof(counters)))
        return {{"working_set_bytes",double(counters.WorkingSetSize)},{"private_bytes",double(counters.PrivateUsage)}};
#endif
    return {};
}
// Diagnostic policy applies only to this owned process. No timeBeginPeriod or
// foreground activation. Microsoft documents Win11 occlusion timer throttling:
// https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation
class TimerResolutionPolicy {
    bool requested_=false,applied_=false,restored_=false;
    QJsonObject report_;
#ifdef Q_OS_WIN
    // Explicit ABI keeps this opt-in probe buildable with older Windows SDKs.
    struct PowerState {ULONG version,control,state;};
    using Query=BOOL(WINAPI*)(HANDLE,int,LPVOID,DWORD);
    using Set=BOOL(WINAPI*)(HANDLE,int,LPVOID,DWORD);
    static constexpr int powerClass=4; // ProcessPowerThrottling
    static constexpr ULONG ignoreTimerResolution=4,version=1;
    PowerState original_{version,0,0};Set set_=nullptr;
#endif
public:
    explicit TimerResolutionPolicy(bool requested):requested_(requested){
        report_["requested"]=requested_;if(!requested_)return;
#ifdef Q_OS_WIN
        const auto kernel=GetModuleHandleW(L"kernel32.dll");
        const auto query=kernel?reinterpret_cast<Query>(GetProcAddress(kernel,"GetProcessInformation")):nullptr;
        set_=kernel?reinterpret_cast<Set>(GetProcAddress(kernel,"SetProcessInformation")):nullptr;
        report_["apis_available"]=bool(query&&set_);if(!query||!set_)return;
        const bool queried=query(GetCurrentProcess(),powerClass,&original_,sizeof(original_));
        report_["query_ok"]=queried;report_["query_error"]=queried?0:int(GetLastError());
        // Without the original policy there can be no exact RAII restoration.
        if(!queried)return;
        report_["original_control_mask"]=int(original_.control);report_["original_state_mask"]=int(original_.state);
        PowerState desired{version,ignoreTimerResolution,0};
        applied_=set_(GetCurrentProcess(),powerClass,&desired,sizeof(desired));
        report_["apply_ok"]=applied_;report_["apply_error"]=applied_?0:int(GetLastError());
        PowerState actual{version,0,0};
        const bool verified=query(GetCurrentProcess(),powerClass,&actual,sizeof(actual));
        report_["applied_query_ok"]=verified;report_["applied_query_error"]=verified?0:int(GetLastError());
        if(verified){report_["applied_control_mask"]=int(actual.control);report_["applied_state_mask"]=int(actual.state);}
#else
        report_["apis_available"]=false;
#endif
    }
    TimerResolutionPolicy(const TimerResolutionPolicy&)=delete;
    ~TimerResolutionPolicy(){restore();}
    void restore(){
        if(restored_)return;restored_=true;
#ifdef Q_OS_WIN
        if(applied_){
            report_["restore_attempted"]=true;
            const bool ok=set_(GetCurrentProcess(),powerClass,&original_,sizeof(original_));
            report_["restore_ok"]=ok;report_["restore_error"]=ok?0:int(GetLastError());
        }
#endif
    }
    QJsonObject report()const{return report_;}
};
QByteArray read(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
QJsonObject distribution(std::vector<double> values){
    if(values.empty())return {{"count",0}};
    std::sort(values.begin(),values.end());double total=0;for(double v:values)total+=v;
    return {{"count",double(values.size())},{"mean_ms",total/values.size()},
        {"p50_ms",values[(values.size()-1)/2]},{"p95_ms",values[size_t((values.size()-1)*.95)]},{"max_ms",values.back()}};
}
struct Delivery {
    quint64 count=0,last=0,ordinal=0;std::vector<double> gaps;
    void receive(quint64 value=0){const auto now=core::perf::now();if(last&&gaps.size()<4096)gaps.push_back((now-last)/1e6);last=now;++count;ordinal=std::max(ordinal,value);}
    QJsonObject json(double seconds)const{return {{"count",double(count)},{"hz",count/seconds},{"last_ordinal",double(ordinal)},{"gap",distribution(gaps)}};}
};
struct Scene {
    struct Span {
        std::atomic<quint64> start{0},count{0},total{0},maximum{0};
        void observe(quint64 begin,core::perf::Stage stage){
            if(!begin)return;const auto duration=core::perf::now()-begin;
            count.fetch_add(1,std::memory_order_relaxed);total.fetch_add(duration,std::memory_order_relaxed);
            auto prior=maximum.load(std::memory_order_relaxed);while(duration>prior&&!maximum.compare_exchange_weak(prior,duration,std::memory_order_relaxed)){}
            core::perf::record(stage,begin,duration);
        }
        void finish(core::perf::Stage stage){observe(start.exchange(0,std::memory_order_relaxed),stage);}
        QJsonObject json()const{const auto n=count.load();return {{"count",double(n)},{"elapsed_ms",total.load()/1e6},{"mean_ms",n?total.load()/1e6/n:0},{"max_ms",maximum.load()/1e6}};}
    };
    QQuickWindow* window=nullptr;std::atomic<quint64> completed{0};RendererProbeTelemetry renderer;
    Span synchronization,rendering,frame,publishWait;
    std::atomic<quint64> firstPendingPublication{0},publications{0},synchronizedPublications{0};
    bool chart=false,piano=false;int node=0;QString dock;
};
bool simpleDocument(const QJsonObject& original,int spectrum,int analysisHz,const QString& style,QJsonObject& result){
    auto graph=numeric_generator_graph();graph.execution_interval={1,std::uint64_t(analysisHz)};
    const domain::NodeId sink=spectrum==2?11:10;
    std::erase_if(graph.nodes,[&](const auto& node){return node.id!=1&&node.id!=2&&node.id!=3&&node.id!=4&&node.id!=16&&node.id!=sink&&!(spectrum==2&&node.id==6);});
    graph.edges={{1,2,0,0},{2,3,0,0},{3,4,0,0}};
    if(spectrum==2){graph.edges.push_back({4,6,0,0});graph.edges.push_back({6,11,0,0});}
    else graph.edges.push_back({4,10,0,0});
    auto& oscillators=std::get<domain::SignalGeneratorProcessingParameters>(graph.nodes.front().parameters).oscillators;
    oscillators.clear();
    for(unsigned i=0;i<8;++i)oscillators.push_back({i+1,true,73.25+double(i*i)*233.7,.045,double(i)*.27,0,0});
    auto ui=original.value("ui").toObject();
    for(const auto* flag:{"show_raw_samples","show_window_signal","show_windowed_signal","show_window_function","show_spectrogram","show_data_flow"})ui[flag]=false;
    ui["show_amplitudes"]=spectrum==1;ui["show_test_amplitudes"]=spectrum==2;
    ui["pipeline_auto_apply"]=true;ui["workspace"]=QJsonObject{};ui["pipeline_presets"]=QJsonArray{};
    const QString key=spectrum==2?"mel":"fft";
    ui["graph_views"]=QJsonObject{{key,QJsonObject{{"display_mode",style},{"open",true}}}};
    desktop::NumericDraftPresentation presentation;presentation.application_settings={{"ui",ui}};presentation.applied_graph=graph;
    desktop::NumericProjectDocument project;
    if(!desktop::create_numeric_project(graph,presentation,project))return false;
    // Serialize through the canonical document boundary, never by inventing a
    // processing JSON schema. Only this fresh fixture will be saved below.
    result=QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(project.source.bytes().data()),qsizetype(project.source.bytes().size()))).object();
    return !result.isEmpty();
}
QJsonArray surfaces(MainWindow* window){
    QJsonArray result;
    for(auto* panel:window->findChildren<GraphPanel*>()){
        auto* surface=panel->active_spectrogram();auto* dock=qobject_cast<WorkspaceDock*>(panel->parentWidget());
        QJsonObject state{{"mode",panel->display_mode()},{"visible",panel->isVisible()},
            {"render_fps",panel->render_fps()},{"dock",dock?dock->objectName():QString{}},
            {"width",panel->width()},{"height",panel->height()}};
        if(surface){state["paint_fps"]=surface->display_fps();state["surface"]=QJsonObject::fromVariantMap(surface->axis_layout_telemetry());
            auto* host=surface->findChild<QQuickWidget*>("spectrogramQuickView");
            if(host && host->rootObject())if(auto* overlay=host->rootObject()->findChild<SurfaceOverlayItem*>("spectrogramOverlay")){
                state["overlay_paint_engine"]=overlay->paintEngineType();state["overlay_render_target"]=int(overlay->renderTarget());}}
        if(auto* chart=panel->active_chart()){
            QJsonArray plots;
            if(chart->rootObject())for(auto* plot:chart->rootObject()->findChildren<VectorPlotItem*>())plots.append(QJsonObject{
                {"visible",plot->isVisible()},{"paint_count",double(plot->paintCount())},{"consumed_count",double(plot->consumedPresentationCount())},{"render_target",int(plot->renderTarget())},
                {"geometry_updates",double(plot->property("geometryUpdateCount").toULongLong())},{"geometry_vertices",plot->property("geometryVertexCount").toInt()},
                {"paint_engine",plot->paintEngineType()},{"samples",plot->paintSamples()},{"width",plot->width()},{"height",plot->height()}});
            state["chart"]=QJsonObject{{"value_count",double(chart->value_count())},{"point_count",double(chart->projected_point_count())},{"quick_format_samples",chart->format().samples()},
                {"peak",chart->peak_value()},{"paint_fps",chart->rendered_fps()},{"style",chart->plot_style()},{"plots",plots}};
        }
        if(auto* piano=panel->piano_view())state["piano"]=QJsonObject{{"paint_fps",piano->display_fps()},{"value_count",double(piano->value_count())},{"peak",piano->peak_value()}};
        result.append(state);
    }return result;
}
}
int main(int argc,char** argv){
    bool snapshotPool=presentation::RenderPreparationPolicy::snapshot_pool,cameraMotion=false,maximized=false,actualCapture=false;QString meshMode="inline",overlayTarget;QList<int> jointNodes;int sliceMode=-1,focusNode=0;
    QString source,output,plotStyle="line",graphicsBackend="opengl",plotHost="widget",pianoHost="window";int fps=0,simpleSpectrum=0,analysisHz=120,previewHz=0,quickSamples=-1,swapInterval=-1,measurementMs=8000;bool hideEditor=false,timePositions=false,workerProbe=false,honorTimers=false,vectorImage=false,suppressPlotPaint=false;
    for(int i=1;i+1<argc;i+=2){const QString key=QString::fromLocal8Bit(argv[i]),value=QString::fromLocal8Bit(argv[i+1]);
        if(key=="--source")source=value;else if(key=="--output")output=value;else if(key=="--gui-fps")fps=value.toInt();else if(key=="--hide-editor")hideEditor=value.toInt()!=0;else if(key=="--time-positions")timePositions=value.toInt()!=0;
        else if(key=="--simple-spectrum")simpleSpectrum=value.toInt();else if(key=="--analysis-hz")analysisHz=value.toInt();else if(key=="--plot-style")plotStyle=value;
        else if(key=="--worker-probe")workerProbe=value.toInt()!=0;
        else if(key=="--actual-capture"){if(value!="0"&&value!="1")return 2;actualCapture=value=="1";}
        else if(key=="--graphics-backend"){if(value!="opengl"&&value!="d3d11")return 2;graphicsBackend=value;}
        else if(key=="--piano-host"){if(value!="window"&&value!="widget")return 2;pianoHost=value;}
        else if(key=="--plot-host"){if(value!="window"&&value!="widget")return 2;plotHost=value;}
        else if(key=="--honor-timers")honorTimers=value.toInt()!=0;
        else if(key=="--preview-hz")previewHz=value.toInt();
        else if(key=="--vector-image"){if(value.toInt()!=0){std::fprintf(stderr,"--vector-image retired: vectors now use scene geometry, not a painter texture\n");return 2;}}
        else if(key=="--suppress-plot-paint")suppressPlotPaint=value.toInt()!=0;
        else if(key=="--quick-samples")quickSamples=value.toInt();
        else if(key=="--swap-interval")swapInterval=value.toInt();
        else if(key=="--measure-ms"){bool ok=false;measurementMs=value.toInt(&ok);if(!ok||measurementMs<2000||measurementMs>8000)return 2;}
        else if(key=="--snapshot-pool"){if(value!="0" && value!="1")return 2;snapshotPool=value=="1";}
        else if(key=="--mesh-mode"){if(value!="inline" && value!="worker" && value!="pooled")return 2;meshMode=value;}
        else if(key=="--camera-motion"){if(value!="0" && value!="1")return 2;cameraMotion=value=="1";}
        else if(key=="--joint-nodes"){for(const auto& id:value.split(',')){bool ok=false;int node=id.toInt(&ok);if(!ok||node<=0||jointNodes.contains(node))return 2;jointNodes.append(node);}if(jointNodes.size()<2||jointNodes.size()>4)return 2;}
        else if(key=="--focus-node"){bool ok=false;focusNode=value.toInt(&ok);if(!ok||focusNode<=0)return 2;}
        else if(key=="--maximized"){if(value!="0"&&value!="1")return 2;maximized=value=="1";}
        else if(key=="--slice-mode"){bool ok=false;sliceMode=value.toInt(&ok);if(!ok||sliceMode<0||sliceMode>3)return 2;}
        else if(key=="--overlay-target"){if(value!="image" && value!="fbo")return 2;overlayTarget=value;}
        else{std::fprintf(stderr,"Unknown argument\n");return 2;}}
    if((!jointNodes.isEmpty()&&focusNode)||source.isEmpty()||output.isEmpty()||fps<0||fps>240||simpleSpectrum<0||simpleSpectrum>2||analysisHz!=120||previewHz<0||previewHz>240||(quickSamples!=-1&&quickSamples!=0&&quickSamples!=4)||(swapInterval < -1 || swapInterval > 1)||(plotStyle!="line"&&plotStyle!="bars")){std::fprintf(stderr,"Usage: application_startup_trace --source PATH --output FRESH_DIR --gui-fps 0..240 --hide-editor 0|1 [--simple-spectrum 0|1|2 --analysis-hz 120 --plot-style line|bars --preview-hz 0..240 --quick-samples -1|0|4 --swap-interval -1|0|1]\n");return 2;}
    const auto original=read(source);if(original.isEmpty()||QFile::exists(QDir(output).filePath("project.json"))||!QDir().mkpath(output))return 2;
    if(actualCapture&&simpleSpectrum)return 2;
    auto document=QJsonDocument::fromJson(original).object();if(document.value("schema_version").toInt()!=4)return 2;
    if(simpleSpectrum){QJsonObject fixture;if(!simpleDocument(document,simpleSpectrum,analysisHz,plotStyle,fixture))return 2;document=std::move(fixture);}
    auto ui=document["ui"].toObject();
    if(fps){auto rendering=ui["graph_rendering"].toObject();rendering["gui_fps_mode"]="fixed";rendering["gui_target_fps"]=fps;ui["graph_rendering"]=rendering;}
    document["ui"]=ui;const auto path=QDir(output).absoluteFilePath("project.json");
    {QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return 2;file.write(QJsonDocument(document).toJson());if(!file.commit())return 2;}
    auto encoded=path.toLocal8Bit();char executable[]="application_startup_trace",option[]="--config";char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
    QElapsedTimer elapsed;elapsed.start();const auto constructionStart=core::perf::now();
    quint32 lanes=1,rate=48000;
    for(const auto& entry:document["pipeline_graph"].toObject()["nodes"].toArray()){
        const auto node=entry.toObject();if(node["kind"]=="AudioSource"){
            const auto outputs=node["outputs"].toArray();quint32 channels=0;for(const auto& port:outputs)if(port.toObject()["id"].toInt()!=65534)++channels;lanes=std::max(lanes,channels);
            if(!outputs.isEmpty())rate=quint32(outputs[0].toObject()["contract"].toObject()["sample_rate"].toInt(48000));
        }
    }
    unsigned captureOpens=0;
    presentation::RenderPreparationPolicy::snapshot_pool=snapshotPool;
    presentation::RenderPreparationPolicy::mesh=meshMode=="worker"?presentation::MeshPreparation::Worker:
        meshMode=="pooled"?presentation::MeshPreparation::Pooled:presentation::MeshPreparation::Inline;
    presentation::RenderPreparationPolicy::plot_host=plotHost=="window"?presentation::PlotHost::NativeWindow:presentation::PlotHost::TextureWidget;
    presentation::RenderPreparationPolicy::piano_host=pianoHost=="window"?presentation::PlotHost::NativeWindow:presentation::PlotHost::TextureWidget;
    TimerResolutionPolicy timerResolutionPolicy(honorTimers);
    // Match the production entrypoint: configure graphics before QApplication.
    // An explicit diagnostic override below may then restore interval 1 for A/B.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    ui::quick::configure();
    if(graphicsBackend=="d3d11")QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    // Owned-process diagnostic, before any Qt contexts/window surfaces exist.
    // Apply overrides before constructing any shared OpenGL/RHI context.
    if(swapInterval>=0 || quickSamples>=0){auto format=QSurfaceFormat::defaultFormat();
        if(swapInterval>=0)format.setSwapInterval(swapInterval);
        if(quickSamples>=0)format.setSamples(quickSamples);
        QSurfaceFormat::setDefaultFormat(format);
    }
    CoreApplication app(count,arguments,[&captureOpens,actualCapture](pipeline::AudioSourceKind kind)->std::unique_ptr<pipeline::IAudioSource>{
        ++captureOpens;
        if(actualCapture)return pipeline::default_audio_source_factory()(kind);
        return std::make_unique<test_fixture::SyntheticCapture>(kind,true);
    });
    if(!actualCapture)if(auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get())) {
        QHash<int,QPair<int,int>> formats;
        for(const auto& entry:document["scene_graph"].toObject()["nodes"].toArray()) {
            const auto node=entry.toObject();if(node["kind"]!="AudioSource")continue;
            int kind=0,channels=0,nativeRate=48000;
            for(const auto& field:node["parameters"].toObject()["values"].toArray())if(field.toObject()["key"]=="source")kind=field.toObject()["value"].toObject()["decimal"].toString().toInt();
            for(const auto& port:node["outputs"].toArray())if(port.toObject()["id"].toInt()!=65534){++channels;nativeRate=port.toObject()["contract"].toObject()["sample_rate"].toInt(48000);}
            formats[kind]={nativeRate,channels};
        }
        model->set_capture_device_provider([formats](int kind,QString*) {
            const auto format=formats.value(kind,{48000,1});
            return QJsonArray{QJsonObject{{"id","synthetic-capture"},{"name","Synthetic capture"},{"is_default",true},{"native_sample_rate",format.first},{"native_channels",format.second}}};
        });
    }
    const double constructionMs=(core::perf::now()-constructionStart)/1e6;
    MainWindow* window=nullptr;for(auto* candidate:QApplication::topLevelWidgets())if(auto* w=qobject_cast<MainWindow*>(candidate))window=w;
    if(!window)return 3;
    window->setAttribute(Qt::WA_ShowWithoutActivating);window->setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    // This own-window policy is set before CoreApplication::exec shows it.
    window->setWindowTitle("Owned full application startup trace");
    bool quickFormatSetWhileVisible=false;int quickFormatsChanged=0;
    if(quickSamples>=0 || swapInterval>=0)for(auto* plot:window->findChildren<QuickPlotWidget*>()){
        quickFormatSetWhileVisible|=plot->isVisible();auto format=plot->format();
        if(quickSamples>=0)format.setSamples(quickSamples);
        if(swapInterval>=0)format.setSwapInterval(swapInterval);
        plot->setFormat(format);++quickFormatsChanged;
    }
    std::vector<std::pair<QPointer<VectorPlotItem>,quint64>> suppressedPlots;
    bool suppressionWritesOk=true;
    const auto suppressVisiblePlots=[&]{
        for(auto* panel:window->findChildren<GraphPanel*>())if(auto* chart=panel->active_chart())if(chart->rootObject())
            for(auto* plot:chart->rootObject()->findChildren<VectorPlotItem*>())if(plot->isVisible()){
                suppressedPlots.emplace_back(plot,0);
                // QQmlProperty::write removes the QML bindings. A visibility
                // callback would permit transient source-model reattachment.
                suppressionWritesOk&=QQmlProperty::write(plot,QStringLiteral("visible"),false);
                suppressionWritesOk&=QQmlProperty::write(plot,QStringLiteral("sourceModel"),QVariant::fromValue(static_cast<QObject*>(nullptr)));
            }
    };
    if(hideEditor)QTimer::singleShot(0,&app,[&]{if(auto* dock=window->workspaceDock("dataFlowDock"))dock->hide();});
    bool focusNodeApplied=false,jointApplied=false;
    QList<QPointer<WorkspaceDock>> jointDocks;
    const auto arrangeJoint=[&]{
        QList<WorkspaceDock*> targets;
        for(int id:jointNodes){WorkspaceDock* target=nullptr;
            for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").toInt()==id && dock->property("viewInstance").toInt()==0){target=dock;break;}
            if(!target)return false;targets.append(target);
        }
        for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").isValid()&&!targets.contains(dock))dock->hide();
        for(int i=0;i<targets.size();++i){auto* dock=targets[i];dock->show();
            if(i)window->splitDockWidget(targets[i-1],dock,Qt::Horizontal);
            dock->setAsCurrentTab();jointDocks.append(dock);
        }
        return true;
    };
    const auto focusTarget=[&]{
        WorkspaceDock* selected=nullptr;
        for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").toInt()==focusNode){
            if(!selected)selected=dock;
            const auto key=dock->property("graphViewKey").toString();
            if(key=="rawSamples"||key=="fft"||key=="mel"){selected=dock;break;}
        }
        if(!selected)return false;
        for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").isValid() && dock!=selected)dock->hide();
        selected->show();selected->raise();return true;
    };
    if(maximized)QTimer::singleShot(1600,&app,[&]{window->showNormal();window->showMaximized();});
    if(focusNode)QTimer::singleShot(1500,&app,[&]{
        focusNodeApplied=focusTarget();
    });
    bool measuring=false;PianoPaintWitness pianoPaints;pianoPaints.measuring=&measuring;std::atomic<bool> renderMeasuring{false};Delivery frames,histories,snapshots,previews,renderTicks,spectrumFrames,plotPublications;
    // Opt-in observer only. The tap is a lossless FIFO witness delivered by
    // engine's 16ms transport drain, NOT an executor-output timestamp probe.
    std::atomic<bool> workerMeasuring{false};Delivery workerTicks,workerTap;
    QTimer* workerTimer=nullptr;std::uint64_t workerTapId=0,workerSequence=0;int workerPreviewInterval=-1,appliedPreviewInterval=-1;
    auto* worker=workerProbe&&simpleSpectrum?&app.pipeline():nullptr;
    if(worker)QMetaObject::invokeMethod(worker,[&]{
        workerTicks.gaps.reserve(4096);workerTap.gaps.reserve(4096);
        workerTimer=new QTimer(worker);workerTimer->setTimerType(Qt::PreciseTimer);workerTimer->setInterval(8);
        QObject::connect(workerTimer,&QTimer::timeout,worker,[&]{if(workerMeasuring.load(std::memory_order_acquire))workerTicks.receive();});
        workerTimer->start();
        workerTapId=worker->subscribe_numeric_tap({{"main",simpleSpectrum==2?11:10,domain::PortDirection::Input,0}},[&](const pipeline::DeliveredNumericFrame& frame){
            if(!workerMeasuring.load(std::memory_order_acquire)||!frame.frame)return;
            const auto stamp=frame.frame.view().stamp;if(!stamp.available||stamp.sequence<=workerSequence)return;
            workerSequence=stamp.sequence;workerTap.receive(workerSequence);
        },{});
    },Qt::BlockingQueuedConnection);
    quint64 spectrumSequence=0;bool spectrumPositive=false;double spectrumFirstEnergy=-1;bool spectrumEnergyChanged=false;
    QPointer<SpectrogramGraph> focusedSurfaceBaseline;
    quint64 focusedSurfaceInitialFrames=0,focusedSurfaceInitialSnapshots=0;
    quint64 firstGraphMs=0;double sampleSeconds=0;QJsonObject initialSnapshot;QJsonArray initialSurfaces,series;
    std::vector<std::unique_ptr<Scene>> scenes;
    QObject::connect(&app.presenter(),&GuiPresenter::numeric_frame_ready,&app,[&](const pipeline::DeliveredNumericFrame& frame){
        if(!measuring)return;frames.receive();
        if(!simpleSpectrum||!frame.context||frame.context->endpoint.node!=(simpleSpectrum==2?11:10)||frame.context->endpoint.direction!=domain::PortDirection::Input||!frame.frame)return;
        const auto view=frame.frame.view();if(!view.stamp.available)return;
        const auto* numeric=std::get_if<signal_contracts::NumericView>(&view.payload);if(!numeric)return;
        if(view.stamp.sequence<=spectrumSequence)return;spectrumSequence=view.stamp.sequence;spectrumFrames.receive(spectrumSequence);
        double energy=0;for(size_t i=0;i<numeric->actual[0];++i){float x=0;if(signal_contracts::load_element(*numeric,{0,i,0},x)!=signal_contracts::Status::ok||!std::isfinite(x))return;energy+=double(x)*x;}
        spectrumPositive|=energy>1e-12;if(spectrumFirstEnergy<0)spectrumFirstEnergy=energy;else spectrumEnergyChanged|=std::abs(energy-spectrumFirstEnergy)>1e-12;
    });
    QObject::connect(&app.presenter(),&GuiPresenter::numeric_history_ready,&app,[&](const auto& row){if(measuring)histories.receive(row.accepted_row_ordinal);});
    QObject::connect(&app.presenter(),&GuiPresenter::numeric_history_snapshot_ready,&app,[&](const auto&,quint64 ordinal,quint64){if(measuring)snapshots.receive(ordinal);});
    QObject::connect(&app.presenter(),&GuiPresenter::pipeline_preview,&app,[&](const auto&){if(measuring)previews.receive();});
    if(auto* timer=window->findChild<QTimer*>("numericRenderTimer"))QObject::connect(timer,&QTimer::timeout,&app,[&]{if(measuring)renderTicks.receive();});
    QObject::connect(&app.presenter(),&GuiPresenter::graph_apply_finished,&app,[&](quint64,bool accepted,const QString&){if(accepted&&!firstGraphMs)firstGraphMs=elapsed.elapsed();});
    QTimer cameraTimer;cameraTimer.setTimerType(Qt::PreciseTimer);cameraTimer.setInterval(4);
    quint64 cameraInputs=0,cameraPanelNotifications=0,cameraPublications=0;
    QPointer<SpectrogramGraph> movingSurface;
    QObject::connect(&cameraTimer,&QTimer::timeout,&app,[&]{if(measuring && movingSurface){
        ++cameraInputs;
        movingSurface->quick_pointer(1,200+double(cameraInputs%100),200+double((cameraInputs/100)%2),Qt::NoButton,Qt::RightButton,Qt::NoModifier);
    }});
    QTimer poll;poll.setTimerType(Qt::PreciseTimer);poll.setInterval(500);
    QObject::connect(&poll,&QTimer::timeout,&app,[&]{if(measuring)series.append(QJsonObject{{"elapsed_ms",double(elapsed.elapsed())},{"engine",app.presenter().client().snapshot().diagnostics},{"surfaces",surfaces(window)},
        {"spectrum_unique_deliveries",double(spectrumFrames.count)},{"spectrum_last_sequence",double(spectrumSequence)},{"plot_publications",double(plotPublications.count)}});});poll.start();
    QTimer::singleShot(1000,&app,[&]{if(sliceMode>=0)for(auto* panel:window->findChildren<GraphPanel*>())if(panel->isVisible() && panel->active_spectrogram()){
        auto* surface=panel->active_spectrogram();surface->reset_camera();
        if(!overlayTarget.isEmpty()){
            auto* host=surface->findChild<QQuickWidget*>("spectrogramQuickView");
            if(host && host->rootObject())if(auto* overlay=host->rootObject()->findChild<SurfaceOverlayItem*>("spectrogramOverlay"))
                overlay->setRenderTarget(overlayTarget=="image"?QQuickPaintedItem::Image:QQuickPaintedItem::FramebufferObject);
        }
        surface->quick_action("selection",sliceMode);
        if(sliceMode){
            auto* host=surface->findChild<QQuickWidget*>("spectrogramQuickView");
            auto* canvas=host && host->rootObject()?host->rootObject()->findChild<SurfaceQuickItem*>("spectrogramContainer"):nullptr;if(!canvas)continue;
            const auto camera=surface->camera_state();const auto position=camera["position"].toArray(),rotation=camera["quaternion"].toArray();
            QMatrix4x4 projection,view;projection.perspective(45,float(canvas->width())/std::max(1.,canvas->height()),.005f,100);
            view.rotate(QQuaternion(rotation[0].toDouble(),rotation[1].toDouble(),rotation[2].toDouble(),rotation[3].toDouble()).conjugated());
            view.translate(-QVector3D(position[0].toDouble(),position[1].toDouble(),position[2].toDouble()));
            const int frame=int(surface->projected_history_frames()/2),band=int(surface->projected_history_bands()/2);float value=0;
            surface->projected_history_sample(frame,band,value);
            const auto p=projection*view*QVector4D(surface->projected_scene_geometry().point(frame,band,value),1);
            if(p.w()>0){const double x=(p.x()/p.w()+1)*canvas->width()/2,y=(1-p.y()/p.w())*canvas->height()/2;
                std::printf("slice fixture: %dx%d canvas %.0fx%.0f point %.1f,%.1f selection %d\n",int(surface->projected_history_frames()),int(surface->projected_history_bands()),canvas->width(),canvas->height(),x,y,surface->quick_state().value("selection").toInt());
                surface->quick_pointer(0,x,y,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
                surface->quick_pointer(2,x,y,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);}
        }
    }});
    QTimer measurementStart,measurementEnd;
    measurementStart.setSingleShot(true);measurementEnd.setSingleShot(true);
    QObject::connect(&measurementStart,&QTimer::timeout,&app,[&]{
        // Applying a real project is asynchronous. Do not profile a temporary
        // startup graph or a tab which the eventual workspace restore replaces.
        if(app.presenter().graph_request_pending() || (focusNode&&!focusTarget())){
            if(elapsed.elapsed()-constructionMs>12000){std::fprintf(stderr,"Trace target did not settle\n");app.exit(5);return;}
            measurementStart.start(50);return;
        }
        if(!jointNodes.isEmpty()&&!jointApplied){
            if(!arrangeJoint()){if(elapsed.elapsed()-constructionMs>12000){app.exit(5);return;}measurementStart.start(50);return;}
            jointApplied=true;measurementStart.start(300);return;
        }
        if(focusNode)focusNodeApplied=true;
        if(suppressPlotPaint)suppressVisiblePlots();
        if(timePositions)for(auto* panel:window->findChildren<GraphPanel*>())if(panel->active_spectrogram())panel->changePreference("time_axis_mode","positions");
        if(previewHz)app.presenter().set_numeric_preview_cadence_hz(previewHz);
        // Read the effective worker cadence after the public presenter setter,
        // before opening the measurement gate. Core and GUI targets stay intact.
        auto* previewEngine=&app.pipeline();
        QMetaObject::invokeMethod(previewEngine,[&]{appliedPreviewInterval=previewEngine->numeric_preview_interval_ms();},Qt::BlockingQueuedConnection);
        for(auto* panel:window->findChildren<GraphPanel*>())if(auto* piano=panel->piano_view())piano->installEventFilter(&pianoPaints);
        initialSnapshot=app.presenter().client().snapshot().diagnostics;initialSurfaces=surfaces(window);
        for(auto& [plot,initial]:suppressedPlots)if(plot)initial=plot->paintCount()+plot->property("geometryUpdateCount").toULongLong();
        for(auto* panel:window->findChildren<GraphPanel*>())if(auto* chart=panel->active_chart())if(chart->isVisible())
            QObject::connect(chart->presentation_model(),&QuickPlotModel::changed,&app,[&]{if(measuring)plotPublications.receive();});
        QList<QWidget*> quickHosts;
        for(auto* host:window->findChildren<QQuickWidget*>())if(host->objectName()!="plotTextureHost")quickHosts.append(host);
        for(auto* host:window->findChildren<QuickPlotWidget*>())quickHosts.append(host);
        for(auto* quick:quickHosts){
            auto* plotHost=dynamic_cast<QuickPlotWidget*>(quick);
            auto state=std::make_unique<Scene>();state->window=plotHost?plotHost->quickWindow():static_cast<QQuickWidget*>(quick)->quickWindow();auto* target=state.get();
            for(auto* dock:window->workspaceDocks())if(dock->isAncestorOf(quick)){target->node=dock->property("graphNode").toInt();target->dock=dock->objectName();break;}
            if(auto* chart=dynamic_cast<QuickPlotWidget*>(quick)){
                target->chart=dynamic_cast<AbstractChartView*>(quick)!=nullptr;
                target->piano=dynamic_cast<NumericPianoView*>(quick)!=nullptr;
                QObject::connect(chart->presentation_model(),&QuickPlotModel::changed,&app,[&,target]{
                    if(!renderMeasuring.load(std::memory_order_relaxed))return;
                    quint64 empty=0;target->firstPendingPublication.compare_exchange_strong(empty,core::perf::now(),std::memory_order_relaxed);
                    target->publications.fetch_add(1,std::memory_order_relaxed);
                });
            }
            QObject::connect(target->window,&QQuickWindow::beforeSynchronizing,&app,[&,target]{if(renderMeasuring.load(std::memory_order_relaxed)){
                target->synchronization.start.store(core::perf::now(),std::memory_order_relaxed);
                target->publishWait.observe(target->firstPendingPublication.exchange(0,std::memory_order_relaxed),core::perf::Stage::Quick2dPublish);
                target->synchronizedPublications.store(target->publications.load(std::memory_order_relaxed),std::memory_order_relaxed);
            }},Qt::DirectConnection);
            QObject::connect(target->window,&QQuickWindow::afterSynchronizing,&app,[&,target]{if(renderMeasuring.load(std::memory_order_relaxed))target->synchronization.finish(core::perf::Stage::Quick2dSync);},Qt::DirectConnection);
            QObject::connect(target->window,&QQuickWindow::beforeRendering,&app,[&,target]{if(renderMeasuring.load(std::memory_order_relaxed))target->rendering.start.store(core::perf::now(),std::memory_order_relaxed);},Qt::DirectConnection);
            QObject::connect(target->window,&QQuickWindow::afterRendering,&app,[&,target]{if(renderMeasuring.load(std::memory_order_relaxed))target->rendering.finish(core::perf::Stage::Quick2dRender);},Qt::DirectConnection);
            QObject::connect(target->window,&QQuickWindow::beforeFrameBegin,&app,[&,target]{if(renderMeasuring.load(std::memory_order_relaxed))target->frame.start.store(core::perf::now(),std::memory_order_relaxed);},Qt::DirectConnection);
            QObject::connect(target->window,&QQuickWindow::afterFrameEnd,&app,[&,target]{if(renderMeasuring.load(std::memory_order_relaxed))target->frame.finish(core::perf::Stage::Quick2dFrame);},Qt::DirectConnection);
            QObject::connect(target->window,&QQuickWindow::afterRendering,&app,[&,target]{if(renderMeasuring.load(std::memory_order_relaxed)){target->renderer.capture();target->completed.fetch_add(1,std::memory_order_relaxed);}},Qt::DirectConnection);
            scenes.push_back(std::move(state));
        }
        if(focusNodeApplied)for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").toInt()==focusNode && dock->property("renderExposed").toBool())
            if(auto* panel=qobject_cast<GraphPanel*>(dock->widget()))if(auto* surface=panel->active_spectrogram())
                {focusedSurfaceBaseline=surface;
                 const auto state=surface->axis_layout_telemetry();
                 focusedSurfaceInitialFrames=state.value("renderedFrames").toULongLong();
                 focusedSurfaceInitialSnapshots=state.value("snapshotRevision").toULongLong();}
        sampleSeconds=elapsed.nsecsElapsed()/1e9;measuring=true;renderMeasuring=true;workerMeasuring.store(true,std::memory_order_release);gui_event_profile::enabled=true;core::perf::enabled=true;
        // External stack profilers must sample the settled workspace, not GL
        // context creation during startup. One cold diagnostic marker only.
        std::fprintf(stderr,"TRACE_MEASUREMENT_STARTED\n");std::fflush(stderr);
        if(cameraMotion)for(auto* panel:window->findChildren<GraphPanel*>())if(panel->isVisible() && panel->active_spectrogram()){
            movingSurface=panel->active_spectrogram();
            QObject::connect(panel,&GraphPanel::state_changed,&app,[&]{if(measuring)++cameraPanelNotifications;});
            QObject::connect(movingSurface,&SpectrogramGraph::camera_changed,&app,[&]{if(measuring)++cameraPublications;});
            movingSurface->quick_pointer(0,200,200,Qt::RightButton,Qt::RightButton,Qt::NoModifier);cameraTimer.start();break;
        }
        measurementEnd.start(measurementMs);
    });
    measurementStart.start(2000);
    QObject::connect(&measurementEnd,&QTimer::timeout,&app,[&]{
        cameraTimer.stop();
        if(movingSurface)movingSurface->quick_pointer(2,200,200,Qt::RightButton,Qt::NoButton,Qt::NoModifier);
        measuring=false;renderMeasuring=false;gui_event_profile::enabled=false;core::perf::enabled=false;sampleSeconds=elapsed.nsecsElapsed()/1e9-sampleSeconds;
        workerMeasuring.store(false,std::memory_order_release);
        // Barrier waits for any observer callback already in progress before
        // reading worker-owned counters or destroying captured stack values.
        if(worker)QMetaObject::invokeMethod(worker,[&]{
            workerPreviewInterval=worker->numeric_preview_interval_ms();
            worker->unsubscribe_numeric_tap(workerTapId);workerTimer->stop();delete workerTimer;workerTimer=nullptr;
        },Qt::BlockingQueuedConnection);
        QJsonArray groups;for(size_t i=0;i<gui_event_profile::group_count;++i){const auto& g=gui_event_profile::groups[i];groups.append(QJsonObject{{"receiver",g.receiver.data()},{"type",g.type},{"count",double(g.count)},{"inclusive_ms",g.inclusive/1e6},{"exclusive_ms",g.exclusive/1e6},{"inclusive_cpu_cycles",double(g.inclusive_cycles)},{"exclusive_cpu_cycles",double(g.exclusive_cycles)},{"max_ms",g.maximum/1e6}});}
        save_performance_report(QDir(output).filePath("trace.json"),groups);
        QJsonArray renderers;for(const auto& scene:scenes)renderers.append(QJsonObject{{"node",scene->node},{"dock",scene->dock},{"width",scene->window->width()},{"height",scene->window->height()},{"visible",scene->window->isVisible()},
            {"completed",double(scene->completed.load())},{"fps",scene->completed.load()/sampleSeconds},{"identity",scene->renderer.snapshot()},
            {"graphics_api",int(scene->window->rendererInterface()->graphicsApi())},
            {"chart_scene",scene->chart},{"synchronization",scene->synchronization.json()},{"rendering",scene->rendering.json()},{"frame",scene->frame.json()},
            {"publication_to_sync",scene->publishWait.json()},{"publications",double(scene->publications.load())},{"synchronized_publication_ordinal",double(scene->synchronizedPublications.load())}});
        const auto actual=app._ctx.file_configs.ui.graph_rendering;auto* timer=window->findChild<QTimer*>("numericRenderTimer");
        const auto* editorDock=window->workspaceDock("dataFlowDock");
        bool positionApplied=!timePositions;
        for(auto* panel:window->findChildren<GraphPanel*>())if(panel->active_spectrogram())
            if(panel->preferences().value("time_axis_mode").toString()=="positions")positionApplied=true;
        bool simplePainted=false;
        if(simpleSpectrum)for(auto* panel:window->findChildren<GraphPanel*>())if(panel->isVisible())if(auto* chart=panel->active_chart())if(chart->value_count()==size_t(simpleSpectrum==2?50:2048)&&chart->rootObject())
            for(auto* plot:chart->rootObject()->findChildren<VectorPlotItem*>())simplePainted|=plot->isVisible()&&(plot->paintCount()>0||plot->property("geometryUpdateCount").toULongLong()>0);
        quint64 suppressedPaintDelta=0;bool suppressionApplied=suppressionWritesOk&&!suppressedPlots.empty();
        for(const auto& [plot,initial]:suppressedPlots){suppressionApplied&=plot&&!plot->isVisible()&&!plot->sourceModel();if(plot)suppressedPaintDelta+=plot->paintCount()+plot->property("geometryUpdateCount").toULongLong()-initial;}
        if(suppressPlotPaint)simplePainted=suppressionApplied&&suppressedPaintDelta==0&&plotPublications.count>0;
        bool sliceApplied=sliceMode<2;
        if(sliceMode>=2)for(auto* panel:window->findChildren<GraphPanel*>())if(panel->isVisible() && panel->active_spectrogram())
            sliceApplied|=panel->active_spectrogram()->axis_layout_telemetry().value("sliceSampleCount").toInt()>0;
        bool focusedVectorPainted=false;
        if(focusNodeApplied)for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").toInt()==focusNode && dock->property("renderExposed").toBool())
            if(auto* panel=qobject_cast<GraphPanel*>(dock->widget()))if(auto* chart=panel->active_chart();chart && chart->value_count()>0 && chart->rootObject())
                for(auto* plot:chart->rootObject()->findChildren<VectorPlotItem*>())focusedVectorPainted|=plot->isVisible()&&(plot->paintCount()>0||plot->property("geometryUpdateCount").toULongLong()>0);
        bool focusedPianoPainted=false;
        if(focusNodeApplied)for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").toInt()==focusNode && dock->property("renderExposed").toBool())if(auto* panel=qobject_cast<GraphPanel*>(dock->widget()))if(auto* piano=panel->piano_view())focusedPianoPainted=piano->value_count()>0 && std::any_of(scenes.begin(),scenes.end(),[&](const auto& scene){return scene->piano && scene->node==focusNode && scene->completed.load()>0 && scene->publications.load()>0;});
        bool focusedSurfaceRendered=false;
        if(focusNodeApplied)for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").toInt()==focusNode && dock->property("renderExposed").toBool())
            if(auto* panel=qobject_cast<GraphPanel*>(dock->widget()))if(auto* surface=panel->active_spectrogram())
                {const auto state=surface->axis_layout_telemetry();
                 focusedSurfaceRendered=focusedSurfaceBaseline && focusedSurfaceBaseline==surface && surface->isVisible() &&
                    state.value("renderedFrames").toULongLong()>focusedSurfaceInitialFrames &&
                    state.value("snapshotRevision").toULongLong()>focusedSurfaceInitialSnapshots;}
        const bool vectorSceneRendered=std::any_of(scenes.cbegin(),scenes.cend(),[](const auto& scene){return scene->chart&&scene->completed.load()>0;});
        const bool sourceValid=simpleSpectrum?(spectrumFrames.count>0&&spectrumPositive&&simplePainted&&captureOpens==0):
            !jointNodes.isEmpty()?frames.count>0||histories.count>0:focusNode?(focusNodeApplied&&((focusedVectorPainted&&frames.count>0&&plotPublications.count>0)||(focusedSurfaceRendered&&histories.count>0&&histories.ordinal>0)||(focusedPianoPainted&&frames.count>0))):(histories.count>0&&histories.ordinal>0);
        bool jointRendered=jointNodes.isEmpty();QJsonArray jointReport;
        if(!jointNodes.isEmpty()){jointRendered=jointApplied;
            for(const auto& dock:jointDocks){bool active=dock&&dock->property("renderExposed").toBool();bool progressed=false;
                if(dock)for(const auto& scene:scenes)if(scene->dock==dock->objectName()&&scene->completed.load()>0&&scene->publications.load()>0)progressed=true;
                if(dock)if(auto* panel=qobject_cast<GraphPanel*>(dock->widget()))if(auto* surface=panel->active_spectrogram()){
                    for(const auto& item:initialSurfaces){auto initial=item.toObject();if(initial["dock"].toString()==dock->objectName()){
                        auto before=initial["surface"].toObject();auto after=QJsonObject::fromVariantMap(surface->axis_layout_telemetry());
                        progressed=after["renderedFrames"].toDouble()>before["renderedFrames"].toDouble() && after["snapshotRevision"].toDouble()>before["snapshotRevision"].toDouble();}}
                }
                if(dock)if(auto* panel=qobject_cast<GraphPanel*>(dock->widget()))if(auto* piano=panel->piano_view())progressed=piano->value_count()>0 && std::any_of(scenes.begin(),scenes.end(),[&](const auto& scene){return scene->piano && scene->node==dock->property("graphNode").toInt() && scene->completed.load()>0 && scene->publications.load()>0;});
                jointReport.append(QJsonObject{{"node",dock?dock->property("graphNode").toInt():0},{"exposed",active},{"progressed",progressed}});jointRendered&=active&&progressed;
            }
        }
        const bool valid=jointRendered&&firstGraphMs&&sourceValid&&(!(simpleSpectrum||focusNode)||vectorSceneRendered||focusedSurfaceRendered||focusedPianoPainted)&&positionApplied&&(!hideEditor||!editorDock||editorDock->isHidden())&&
            sliceApplied&&(!suppressPlotPaint||(suppressionApplied&&suppressedPaintDelta==0&&plotPublications.count>0));
        timerResolutionPolicy.restore();
        quint64 pianoPaintCount=0;for(auto count:pianoPaints.counts)pianoPaintCount+=count;
        quint64 pianoFrames=0,pianoPublications=0;for(const auto& scene:scenes)if(scene->piano){pianoFrames+=scene->completed.load();pianoPublications+=scene->publications.load();}
        QJsonObject report{{"piano_rendered_frames",double(pianoFrames)},{"piano_frame_hz",pianoFrames/sampleSeconds},{"piano_publication_hz",pianoPublications/sampleSeconds},{"piano_paint_count",double(pianoPaintCount)},{"piano_paint_hz",pianoPaintCount/sampleSeconds},{"focused_piano_painted",focusedPianoPainted},{"source",QFileInfo(source).absoluteFilePath()},{"source_sha256",QString::fromLatin1(QCryptographicHash::hash(original,QCryptographicHash::Sha256).toHex())},
            {"joint_views",jointReport},{"joint_rendered",jointRendered},{"valid_processing_trace",valid},{"source_valid",sourceValid},{"focused_vector_painted",focusedVectorPainted},{"focused_surface_rendered",focusedSurfaceRendered},{"vector_scene_rendered",vectorSceneRendered},
            {"capture_lanes",int(lanes)},{"capture_sample_rate",int(rate)},
            {"actual_capture",actualCapture},{"simple_spectrum",simpleSpectrum},{"analysis_hz_override",simpleSpectrum?analysisHz:0},{"plot_style_override",plotStyle},{"capture_opens",int(captureOpens)},
            {"spectrum_unique_delivery",spectrumFrames.json(sampleSeconds)},{"spectrum_positive",spectrumPositive},{"spectrum_energy_changed",spectrumEnergyChanged},{"plot_publication",plotPublications.json(sampleSeconds)},
            {"worker_probe",workerProbe},{"worker_preview_interval_ms",workerPreviewInterval},{"worker_independent_8ms_timer",workerTicks.json(sampleSeconds)},
            {"requested_preview_hz_override",previewHz},{"applied_preview_interval_ms",appliedPreviewInterval},
            {"vector_image_override",vectorImage},
            {"quick_samples_override",quickSamples},{"quick_formats_changed",quickFormatsChanged},{"quick_format_set_while_visible",quickFormatSetWhileVisible},
            {"swap_interval_override",swapInterval},{"default_swap_interval",QSurfaceFormat::defaultFormat().swapInterval()},
            {"graphics_backend_override",graphicsBackend},{"plot_host",plotHost},{"piano_host",pianoHost},
            {"window_requested_swap_interval",window->windowHandle()?window->windowHandle()->requestedFormat().swapInterval():-99},
            {"window_actual_swap_interval",window->windowHandle()?window->windowHandle()->format().swapInterval():-99},
            {"suppress_plot_paint_override",suppressPlotPaint},{"plot_paint_suppression_applied",suppressionApplied},{"suppressed_plot_paint_delta",double(suppressedPaintDelta)},
            {"plot_suppression_property_writes_ok",suppressionWritesOk},
            {"timer_resolution_policy",timerResolutionPolicy.report()},
            {"worker_fifo_tap_delivery",workerTap.json(sampleSeconds)},{"worker_fifo_tap_id",double(workerTapId)},
            {"editor_dock_hidden",editorDock?editorDock->isHidden():true},
            {"time_positions_override",timePositions},{"time_positions_applied",positionApplied},
            {"source_unchanged",original==read(source)},{"platform",QApplication::platformName()},{"construction_ms",constructionMs},{"first_graph_ms",double(firstGraphMs)},
            {"sample_seconds",sampleSeconds},{"requested_gui_fps_override",fps},{"editor_hidden_override",hideEditor},
            {"snapshot_pool",snapshotPool},{"mesh_mode",meshMode},
            {"camera_motion",cameraMotion},{"camera_inputs",double(cameraInputs)},
            {"focus_node",focusNode},{"focus_node_applied",focusNodeApplied},
            {"maximized_requested",maximized},{"maximized_applied",window->isMaximized()},
            {"slice_mode",sliceMode},
            {"overlay_target_override",overlayTarget},
            {"slice_applied",sliceApplied},
            {"camera_panel_notifications",double(cameraPanelNotifications)},{"camera_publications",double(cameraPublications)},
            {"snapshot_pool_allocations",double(NumericMatrixSnapshotPool::allocations)},
            {"snapshot_pool_reuses",double(NumericMatrixSnapshotPool::reuses)},
            {"snapshot_pool_fallbacks",double(NumericMatrixSnapshotPool::fallbacks)},
            {"process_memory",processMemory()},
            {"applied_graph_rendering",actual},{"gui_timer_interval_ms",timer?timer->interval():-1},{"screen_refresh_hz",window->screen()?window->screen()->refreshRate():0},
            {"device_pixel_ratio",window->devicePixelRatioF()},{"window_size",QJsonArray{window->width(),window->height()}},
            {"frame_delivery",frames.json(sampleSeconds)},{"history_delivery",histories.json(sampleSeconds)},{"snapshot_delivery",snapshots.json(sampleSeconds)},
            {"preview_delivery",previews.json(sampleSeconds)},{"render_timer",renderTicks.json(sampleSeconds)},
            {"initial_engine",initialSnapshot},{"final_engine",app.presenter().client().snapshot().diagnostics},
            {"initial_surfaces",initialSurfaces},{"final_surfaces",surfaces(window)},{"series",series},{"quick_windows",renderers},{"gui_groups_overflow",double(gui_event_profile::overflow)}};
        // Capture only owned renderer framebuffers, after timing gates close.
        for(auto* panel:window->findChildren<GraphPanel*>())if(panel->isVisible()){
            if(auto* piano=panel->piano_view())report["piano_framebuffer_saved"]=piano->grabFramebuffer().save(QDir(output).filePath("piano.png"));
            if(auto* surface=panel->active_spectrogram())if(auto* host=surface->findChild<QQuickWidget*>("spectrogramQuickView"))report["surface_framebuffer_saved"]=host->grabFramebuffer().save(QDir(output).filePath("surface.png"));
        }
        QSaveFile file(QDir(output).filePath("summary.json"));if(!file.open(QIODevice::WriteOnly)){app.exit(4);return;}file.write(QJsonDocument(report).toJson());if(!file.commit()){app.exit(4);return;}
        std::printf("Full CoreApplication trace complete: sample %.3fs, GUI timer %.2fHz, history %.2fHz\n",sampleSeconds,renderTicks.count/sampleSeconds,histories.count/sampleSeconds);
        app.exit(valid?0:5);
    });
    return app.exec();
}
