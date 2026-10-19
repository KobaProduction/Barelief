#include <visualization/quick/quick_ui.h>
#include <visualization/quick2d/plot_model.h>
#include <visualization/quick2d/vector_plot_item.h>
#include <core/performance.h>
#include <dsp_core/dsp.h>
#include "renderer_probe_telemetry.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPolygonF>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickView>
#include <QQuickWidget>
#include <QSaveFile>
#include <QSurfaceFormat>
#include <QTimer>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace {
constexpr int warmupMs=1000;
struct ProcessCpu {bool available=false;quint64 ticks100ns=0;};
ProcessCpu processCpu(){
#ifdef Q_OS_WIN
    FILETIME created{},exited{},kernel{},user{};
    if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)){
        const auto ticks=[](const FILETIME& value){return (quint64(value.dwHighDateTime)<<32)|quint64(value.dwLowDateTime);};
        return {true,ticks(kernel)+ticks(user)};
    }
#endif
    return {};
}
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
QJsonObject distribution(std::vector<double> values){
    if(values.empty())return {{"count",0}};
    std::sort(values.begin(),values.end());
    const auto at=[&](double q){return values[std::min(values.size()-1,size_t(std::ceil(q*values.size())-1))];};
    double total=0;for(const auto value:values)total+=value;
    return {{"count",double(values.size())},{"mean_ms",total/values.size()},{"p50_ms",at(.5)},{"p95_ms",at(.95)},{"max_ms",values.back()}};
}
// Measures GUI event delivery elapsed time, including blocking synchronization.
// It is deliberately not named CPU usage or GPU time.
class ProbeApplication final:public QApplication {
public:
    using QApplication::QApplication;
    bool measuring=false;
    quint64 events=0,totalNs=0,maxNs=0;
    bool notify(QObject* receiver,QEvent* event) override {
        if(QThread::currentThread()!=thread() || !measuring)return QApplication::notify(receiver,event);
        const auto start=core::perf::now();
        const bool result=QApplication::notify(receiver,event);
        const auto elapsed=core::perf::now()-start;
        if(measuring){++events;totalNs+=elapsed;maxNs=std::max(maxNs,quint64(elapsed));}
        return result;
    }
};
struct SceneCounters {
    std::atomic<quint64> completed{0},begin{0},totalNs{0},maxNs{0};
    std::atomic<quint64> syncCompleted{0},syncBegin{0},syncTotalNs{0},syncMaxNs{0};
    std::atomic<bool> renderStartedInSample{false},syncStartedInSample{false};
    RendererProbeTelemetry renderer;
};
template<size_t Samples>
QVariantMap snapshot(const QString& style,const std::array<double,Samples>& samples,bool variantGeometry){
    QVariantList points;
    if(variantGeometry){
        points.reserve(samples.size());
        for(size_t i=0;i<samples.size();++i)points.append(QVariantMap{{"x",double(i)/(samples.size()-1)},{"y",(1.-samples[i])*.5}});
    }
    QVariantList xTicks,yTicks;
    for(int i=0;i<=4;++i){xTicks.append(QVariantMap{{"position",i/4.},{"text",QString::number(i*(Samples-1)/4)}});yTicks.append(QVariantMap{{"position",i/4.},{"text",QString::number(Samples==2048?.1*(1.-i/4.):1.-i*.5)}});}
    QVariantMap state{{"kind","vector"},{"style",style},{"title",QString("Topology probe: %1").arg(style)},
        {"baseline",Samples==2048?1.:.5},{"xTicks",xTicks},{"yTicks",yTicks},{"xTitle","Sample index"},{"yTitle","Amplitude"},
        {"plotLeft",82},{"lineWidth",1.25},{"pointSize",12},{"barWidth",80}};
    if(variantGeometry)state.insert("points",points);
    return state;
}
template<size_t Samples>
QPolygonF typedPoints(const std::array<double,Samples>& samples){
    QPolygonF points;points.reserve(samples.size());
    for(size_t i=0;i<samples.size();++i)points.append(QPointF(double(i)/(samples.size()-1),(1.-samples[i])*.5));
    return points;
}
template<int plotCount, size_t Samples>
QJsonObject run(ProbeApplication& app,const QString& topology,int fps,int durationMs,int swapInterval,const QString& geometryPath){
    const QStringList styles{"line","line","line","points","points","bars","bars"};
    const qint64 periodNs=1000000000LL/fps;
    std::array<SceneCounters,plotCount> scenes;
    std::atomic<bool> measuring{false};
    QWidget host;
    host.setAttribute(Qt::WA_ShowWithoutActivating);
    host.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    host.setWindowTitle("Owned render topology probe");host.resize(plotCount==1?1332:1600,plotCount==1?825:1000);
    auto* grid=new QGridLayout(&host);grid->setContentsMargins(0,0,0,0);grid->setSpacing(6);
    std::array<QuickPlotModel*,plotCount> models{};
    std::array<VectorPlotItem*,plotCount> plots{};
    std::array<QWidget*,plotCount> containers{};
    std::array<QQuickView*,plotCount> nativeViews{};
    std::array<QQuickWindow*,plotCount> windows{};
    std::array<QQuickItem*,plotCount> roots{};
    QQuickView* sharedView=nullptr;QWidget* sharedContainer=nullptr;
    // This hidden widget is only a layout calculator, never a rendered host.
    // It preserves the same QWidget grid rounding as the seven-window arms.
    QWidget geometryHost;QGridLayout* geometryGrid=nullptr;
    if(topology=="shared"){
        sharedView=new QQuickView;
        sharedView->setFlags(sharedView->flags()|Qt::WindowDoesNotAcceptFocus);
        sharedView->setFormat(QSurfaceFormat::defaultFormat());
        auto* bridge=new ui::quick::UiBridge(sharedView);
        sharedView->rootContext()->setContextProperty("quickUi",bridge);
        sharedView->setColor(bridge->palette().value("window").value<QColor>());
        sharedContainer=QWidget::createWindowContainer(sharedView,&host);
        sharedContainer->setFocusPolicy(Qt::NoFocus);grid->addWidget(sharedContainer,0,0);
        geometryGrid=new QGridLayout(&geometryHost);geometryGrid->setContentsMargins(0,0,0,0);geometryGrid->setSpacing(6);
        for(int i=0;i<plotCount;++i)geometryGrid->addWidget(new QWidget(&geometryHost),i/2,i%2);
        const auto layoutShared=[&]{
            geometryHost.resize(sharedView->size());geometryGrid->setGeometry(geometryHost.rect());
            for(int i=0;i<plotCount;++i)if(roots[i]){
                const auto cell=geometryGrid->cellRect(i/2,i%2);roots[i]->setPosition(cell.topLeft());roots[i]->setSize(cell.size());
            }
        };
        QObject::connect(sharedView,&QQuickWindow::widthChanged,&geometryHost,layoutShared);
        QObject::connect(sharedView,&QQuickWindow::heightChanged,&geometryHost,layoutShared);
    }
    QList<QMetaObject::Connection> connections;
    std::array<double,Samples> samples{};
    dsp_core::MagnitudeSpectrum fft;
    std::vector<float> pcm, coefficients;
    if constexpr(plotCount==1){
        require(fft.prepare(Samples*2),"FFT preparation failed");
        pcm.resize(Samples*2);coefficients.resize(Samples);
    }
    const auto publishOne=[&](int i){
        // Both arms build one owned normalized snapshot for each plot. Only the
        // geometry representation/handoff differs; no QML geometry is omitted.
        if(geometryPath=="typed")models[i]->publishVector(snapshot(styles[i],samples,false),typedPoints(samples));
        else models[i]->publish(snapshot(styles[i],samples,true));
    };
    for(int i=0;i<plotCount;++i){
        models[i]=new QuickPlotModel(&host);publishOne(i);
        QQuickWindow* window=nullptr;QQuickItem* root=nullptr;
        if(topology=="shared"){
            auto* context=new QQmlContext(sharedView->rootContext(),sharedView);
            context->setContextProperty("graphModel",models[i]);
            QQmlComponent component(sharedView->engine(),QUrl("qrc:/plots2d/Plot2D.qml"));
            root=qobject_cast<QQuickItem*>(component.create(context));
            require(root,"shared Plot2D.qml component creation failed");
            root->setParent(sharedView->contentItem());root->setParentItem(sharedView->contentItem());
            window=sharedView;containers[i]=sharedContainer;
        }else if(topology=="widget"){
            auto* quick=new QQuickWidget(&host);
            quick->setFormat(QSurfaceFormat::defaultFormat());
            ui::quick::initialize(quick,models[i],QUrl("qrc:/plots2d/Plot2D.qml"));
            require(quick->status()!=QQuickWidget::Error,"QQuickWidget QML load failed");
            window=quick->quickWindow();root=quick->rootObject();containers[i]=quick;
        }else{
            // No QQuickWidget is constructed in this process arm. Same QML,
            // model, palette bridge and vector renderer, only host differs.
            auto* view=new QQuickView;
            nativeViews[i]=view;
            view->setFlags(view->flags()|Qt::WindowDoesNotAcceptFocus);view->setFormat(QSurfaceFormat::defaultFormat());
            view->setResizeMode(QQuickView::SizeRootObjectToView);
            auto* bridge=new ui::quick::UiBridge(view);
            view->rootContext()->setContextProperty("quickUi",bridge);
            view->rootContext()->setContextProperty("graphModel",models[i]);
            view->setColor(bridge->palette().value("window").value<QColor>());
            view->setSource(QUrl("qrc:/plots2d/Plot2D.qml"));
            require(view->status()!=QQuickView::Error,"QQuickView QML load failed");
            window=view;root=view->rootObject();
            containers[i]=QWidget::createWindowContainer(view,&host);
        }
        require(root,"QML root absent");
        windows[i]=window;
        roots[i]=root;
        plots[i]=root->findChild<VectorPlotItem*>("numericVectorPlot");require(plots[i],"actual VectorPlotItem absent");
        if(topology!="shared"){containers[i]->setFocusPolicy(Qt::NoFocus);grid->addWidget(containers[i],i/2,i%2);}
        // A shared scene has exactly one callback/counter set. Per-plot reports
        // refer to scene zero rather than manufacturing seven scene completions.
        if(topology=="shared"&&i!=0)continue;
        connections.append(QObject::connect(window,&QQuickWindow::beforeSynchronizing,&host,[&,i]{
            scenes[i].syncStartedInSample=measuring.load(std::memory_order_relaxed);scenes[i].syncBegin=core::perf::now();
        },Qt::DirectConnection));
        connections.append(QObject::connect(window,&QQuickWindow::afterSynchronizing,&host,[&,i]{
            if(!measuring.load(std::memory_order_relaxed)||!scenes[i].syncStartedInSample.load(std::memory_order_relaxed))return;
            auto& scene=scenes[i];const auto elapsed=core::perf::now()-scene.syncBegin.load(std::memory_order_relaxed);
            ++scene.syncCompleted;scene.syncTotalNs.fetch_add(elapsed);
            auto maximum=scene.syncMaxNs.load();while(elapsed>maximum&&!scene.syncMaxNs.compare_exchange_weak(maximum,elapsed)){}
        },Qt::DirectConnection));
        connections.append(QObject::connect(window,&QQuickWindow::beforeRendering,&host,[&,i]{
            scenes[i].renderStartedInSample=measuring.load(std::memory_order_relaxed);scenes[i].begin=core::perf::now();
        },Qt::DirectConnection));
        connections.append(QObject::connect(window,&QQuickWindow::afterRendering,&host,[&,i]{
            scenes[i].renderer.capture();
            if(!measuring.load(std::memory_order_relaxed)||!scenes[i].renderStartedInSample.load(std::memory_order_relaxed))return;
            const auto elapsed=core::perf::now()-scenes[i].begin.load(std::memory_order_relaxed);
            auto& scene=scenes[i];++scene.completed;scene.totalNs.fetch_add(elapsed);
            auto maximum=scene.maxNs.load();while(elapsed>maximum&&!scene.maxNs.compare_exchange_weak(maximum,elapsed)){}
        },Qt::DirectConnection));
    }
    host.show();
    if(sharedView){
        geometryHost.resize(sharedView->size());geometryGrid->setGeometry(geometryHost.rect());
        for(int i=0;i<plotCount;++i){const auto cell=geometryGrid->cellRect(i/2,i%2);roots[i]->setPosition(cell.topLeft());roots[i]->setSize(cell.size());}
    }
    QElapsedTimer clock;clock.start();
    qint64 nextSupply=periodNs,nextHeartbeat=10000000;
    quint64 supplies=0,missed=0;
    std::vector<double> generation,publishing,lateness,heartbeat;
    const size_t capacity=size_t(durationMs*fps/1000+100);
    generation.reserve(capacity);publishing.reserve(capacity);lateness.reserve(capacity);heartbeat.reserve(size_t(durationMs/10+100));
    QTimer producer,beat,end,safety;producer.setSingleShot(true);producer.setTimerType(Qt::PreciseTimer);
    beat.setInterval(10);beat.setTimerType(Qt::PreciseTimer);end.setSingleShot(true);
    const auto arm=[&]{producer.start(int(std::max<qint64>(1,(nextSupply-clock.nsecsElapsed()+999999)/1000000)));};
    QObject::connect(&producer,&QTimer::timeout,&host,[&]{
        const auto now=clock.nsecsElapsed();const auto start=core::perf::now();
        if constexpr(plotCount==1){
            for(size_t i=0;i<pcm.size();++i){double value=0;for(int tone=1;tone<=8;++tone)value+=.08*std::sin(tone*i*.05+now/1e9*(.6+.1*tone));pcm[i]=float(value);}
            require(fft.process(pcm,coefficients),"FFT processing failed");
            // Display a fixed amplitude range [0, .1] using the common normalized mapping.
            for(size_t i=0;i<samples.size();++i)samples[i]=20.*coefficients[i]-1.;
        }else{
            for(size_t i=0;i<samples.size();++i){double value=0;for(int tone=1;tone<=8;++tone)value+=.08*std::sin(tone*i*.05+now/1e9*(.6+.1*tone));samples[i]=value;}
        }
        const auto generated=core::perf::now();
        for(int i=0;i<plotCount;++i)publishOne(i);
        const auto published=core::perf::now();
        if(measuring){++supplies;generation.push_back((generated-start)/1e6);publishing.push_back((published-generated)/1e6);lateness.push_back(std::max(0.,double(now-nextSupply)/1e6));}
        nextSupply+=periodNs;
        if(nextSupply<=clock.nsecsElapsed()){const auto skipped=(clock.nsecsElapsed()-nextSupply)/periodNs+1;if(measuring)missed+=quint64(skipped);nextSupply+=skipped*periodNs;}
        arm();
    });
    QObject::connect(&beat,&QTimer::timeout,&host,[&]{const auto now=clock.nsecsElapsed();if(measuring)heartbeat.push_back(std::max(0.,double(now-nextHeartbeat)/1e6));nextHeartbeat=now+10000000;});
    const auto vectorUpdates=[](VectorPlotItem* plot){
        auto* geometry=qobject_cast<VectorGeometryItem*>(plot);
        return plot->paintCount()+(geometry?geometry->geometryUpdateCount():0);
    };
    std::array<quint64,plotCount> paintStart{},uniqueStart{};
    qreal sampleHostDprStart=0;
    std::array<qreal,plotCount> effectiveDprStart{};
    struct Endpoints {
        quint64 paints=0,unique=0,completed=0,totalNs=0,maxNs=0,syncCompleted=0,syncTotalNs=0,syncMaxNs=0;int engine=-1,samples=-1;
        qreal effectiveDpr=0,windowDpr=0;QSizeF rootSize,vectorSize;QSize windowSize,scenePixelSize;
    };
    std::array<Endpoints,plotCount> endpoints{};
    struct StageEndpoint {quint64 count=0,total=0,maximum=0;};
    std::array<StageEndpoint,static_cast<size_t>(core::perf::Stage::Count)> stageEndpoints{};
    ProcessCpu cpuStart;
    QElapsedTimer sampleClock;QEventLoop loop;
    bool safetyExpired=false;safety.setSingleShot(true);
    QObject::connect(&safety,&QTimer::timeout,&loop,[&]{safetyExpired=true;loop.quit();});
    safety.start(warmupMs+durationMs+5000);
    QTimer::singleShot(warmupMs,&host,[&]{
        for(int i=0;i<plotCount;++i){paintStart[i]=vectorUpdates(plots[i]);uniqueStart[i]=plots[i]->consumedPresentationCount();}
        sampleHostDprStart=host.devicePixelRatioF();
        for(int i=0;i<plotCount;++i)effectiveDprStart[i]=windows[i]->effectiveDevicePixelRatio();
        core::perf::enabled=true;cpuStart=processCpu();sampleClock.start();measuring=true;app.measuring=true;
        nextSupply=clock.nsecsElapsed()+periodNs;nextHeartbeat=clock.nsecsElapsed()+10000000;arm();beat.start();end.start(durationMs);
    });
    QObject::connect(&end,&QTimer::timeout,&loop,&QEventLoop::quit);
    arm();beat.start();loop.exec();
    measuring=false;app.measuring=false;core::perf::enabled=false;
    const auto sampleEndNs=sampleClock.isValid()?sampleClock.nsecsElapsed():0;
    const auto cpuEnd=processCpu();
    const auto deadlineEndNs=clock.nsecsElapsed();
    // Capture every raw counter before allocating/serializing any report data.
    for(int i=0;i<plotCount;++i){auto& e=endpoints[i];const auto& s=scenes[topology=="shared"?0:i];
        e.unique=plots[i]->consumedPresentationCount()-uniqueStart[i];e.paints=vectorUpdates(plots[i])-paintStart[i];
        e.completed=s.completed.load();e.totalNs=s.totalNs.load();e.maxNs=s.maxNs.load();
        e.syncCompleted=s.syncCompleted.load();e.syncTotalNs=s.syncTotalNs.load();e.syncMaxNs=s.syncMaxNs.load();
        e.engine=plots[i]->paintEngineType();e.samples=plots[i]->paintSamples();
    }
    for(size_t i=0;i<stageEndpoints.size();++i){const auto& source=core::perf::counters[i];stageEndpoints[i]={source.count.load(),source.total.load(),source.maximum.load()};}
    // Read visible-window geometry/DPR before hiding or disposing any QML item.
    // The effective ratio includes QuickWidget's render-control redirection.
    const qreal sampleHostDpr=host.devicePixelRatioF();
    for(int i=0;i<plotCount;++i){auto& e=endpoints[i];
        e.effectiveDpr=windows[i]->effectiveDevicePixelRatio();e.windowDpr=windows[i]->devicePixelRatio();
        e.rootSize=roots[i]->size();e.vectorSize=plots[i]->size();e.windowSize=windows[i]->size();
        e.scenePixelSize={qRound(e.windowSize.width()*e.effectiveDpr),qRound(e.windowSize.height()*e.effectiveDpr)};
    }
    producer.stop();beat.stop();safety.stop();
    require(!safetyExpired&&sampleEndNs>0,"bounded probe lifetime expired before normal completion");
    if(nextSupply<=deadlineEndNs)missed+=quint64((deadlineEndNs-nextSupply)/periodNs+1);
    const double seconds=sampleEndNs/1e9;
    const bool cpuAvailable=cpuStart.available&&cpuEnd.available&&cpuEnd.ticks100ns>=cpuStart.ticks100ns;
    const double cpuMs=cpuAvailable?(cpuEnd.ticks100ns-cpuStart.ticks100ns)/10000.:0.;
    QJsonArray resultPlots;bool valid=true;
    const bool producerMet=supplies/seconds>=fps*.95 && missed<=.01*double(supplies+missed);
    bool targetMet=producerMet;
    for(int i=0;i<plotCount;++i){
        const auto& e=endpoints[i];const auto unique=e.unique,painted=e.paints,completed=e.completed;
        const bool met=producerMet&&unique/seconds>=fps*.95;targetMet&=met;valid&=painted>0&&unique>0&&completed>0;
        const int sceneIndex=topology=="shared"?0:i;
        resultPlots.append(QJsonObject{{"style",styles[i]},{"logical_size",QJsonArray{e.rootSize.width(),e.rootSize.height()}},
            {"scene_counter_index",sceneIndex},{"scene_counter_shared",topology=="shared"},
            {"vector_size",QJsonArray{e.vectorSize.width(),e.vectorSize.height()}},
            {"measured_dpr",e.effectiveDpr},{"measured_dpr_start",effectiveDprStart[i]},{"window_dpr",e.windowDpr},
            {"scene_logical_size",QJsonArray{e.windowSize.width(),e.windowSize.height()}},
            {"scene_pixel_size_from_dpr",QJsonArray{e.scenePixelSize.width(),e.scenePixelSize.height()}},
            {"scene_submissions",double(completed)},{"scene_submissions_fps",completed/seconds},
            {"actual_vector_updates",double(painted)},{"unique_presentations_drawn",double(unique)},{"unique_presentations_fps",unique/seconds},
            {"paint_engine_type",e.engine},{"actual_paint_samples",e.samples},
            {"scene_render_mean_ms",completed?e.totalNs/double(completed)/1e6:0.},{"scene_render_max_ms",e.maxNs/1e6},
            {"scene_sync_count",double(e.syncCompleted)},
            {"scene_sync_mean_ms",e.syncCompleted?e.syncTotalNs/double(e.syncCompleted)/1e6:0.},
            {"scene_sync_max_ms",e.syncMaxNs/1e6},
            {"renderer",scenes[sceneIndex].renderer.snapshot()},{"target_met",met}});
    }
    for(const auto& connection:connections)QObject::disconnect(connection);
    host.hide();
    // Dispose QML bindings while the presentation models are still alive.
    for(int i=0;i<plotCount;++i){
        if(sharedView){delete roots[i];roots[i]=nullptr;}
        else if(nativeViews[i])nativeViews[i]->setSource(QUrl{});
        else static_cast<QQuickWidget*>(containers[i])->setSource(QUrl{});
    }
    QJsonArray stages;
    for(size_t i=0;i<stageEndpoints.size();++i){const auto& c=stageEndpoints[i];if(c.count)stages.append(QJsonObject{{"stage",core::perf::names[i]},{"count",double(c.count)},{"mean_ms",c.total/double(c.count)/1e6},{"max_ms",c.maximum/1e6}});}
    return {{"fixture_revision",2},{"sample_count",int(Samples)},{"fft_input_size",plotCount==1?int(Samples*2):0},{"host",topology},{"geometry_path",geometryPath},{"plots",resultPlots},{"distinct_scene_count",topology=="shared"?1:plotCount},{"target_fps",fps},{"requested_samples",8},{"requested_swap_interval",swapInterval},{"warmup_ms",warmupMs},{"sample_seconds",seconds},
        {"live_supplies",double(supplies)},{"supplied_hz",supplies/seconds},{"missed_supply_deadlines",double(missed)},
        {"scheduled_supply_deadlines",double(supplies+missed)},{"producer_target_met",producerMet},{"target_met",targetMet},{"valid",valid},
        {"generation_elapsed",distribution(generation)},{"snapshot_publish_elapsed",distribution(publishing)},{"supply_lateness",distribution(lateness)},
        {"geometry_path_scope","Each plot constructs the same sample_count normalized points inside snapshot_publish_elapsed; typed uses QPolygonF/publishVector and metadata without points, variant uses point QVariantMaps/publish fallback. No cross-plot source geometry sharing in either arm."},
        {"gui_heartbeat_lateness",distribution(heartbeat)},{"gui_events",double(app.events)},{"gui_event_elapsed_total_ms",app.totalNs/1e6},{"gui_event_elapsed_max_ms",app.maxNs/1e6},
        {"process_cpu_available",cpuAvailable},{"process_cpu_ms",cpuAvailable?QJsonValue(cpuMs):QJsonValue(QJsonValue::Null)},
        {"average_busy_logical_cores",cpuAvailable?QJsonValue(cpuMs/(seconds*1000.)):QJsonValue(QJsonValue::Null)},
        {"process_cpu_scope","Windows GetProcessTimes process-wide kernel+user CPU time over the measured sample; includes all owned process threads. Average busy logical cores is CPU time divided by wall time, not system-wide CPU percent. Unavailable on other platforms."},
        {"stage_elapsed",stages},{"dpr",sampleHostDpr},{"dpr_start",sampleHostDprStart},{"render_loop_environment",qEnvironmentVariable("QSG_RENDER_LOOP")},
        {"dpr_scope","Host and per-plot DPR/geometry captured while visible at sample endpoints, before hide or QML teardown. measured_dpr is QQuickWindow::effectiveDevicePixelRatio; window_dpr is QWindow::devicePixelRatio. scene_pixel_size_from_dpr is calculated from logical scene size and effective DPR, not an observed framebuffer allocation."},
        {"scope",plotCount==1?"One line Plot2D.qml instance, 2048 coefficients of a real portable 4096-sample FFT from eight varying synthetic tones. Fixed amplitude range. Isolates host topology; excludes GraphPanel, docks, audio capture, executor and surface3D. Separate process per host required.":"Seven identical Plot2D.qml instances: three line, two points, two bars; same 256 samples from eight varying synthetic tones. widget/window use seven independent scenes; shared uses one native scene and seven per-plot contexts. Shared per-plot scene fields repeat scene zero, not seven independent completions; unique vector counters remain per item. Isolates vector host topology; excludes GraphPanel, DSP, docks, surface3D, image, metrics, audio and desktop capture. Separate process per host is required."},
        {"counter_scope","afterRendering counts scene submission, not GPU completion or physical presentation. beforeSynchronizing through afterSynchronizing measures the GUI-blocking scene synchronization interval; beforeRendering through afterRendering measures later scene render submission. Vector atomics count geometry updates (or software paints) and one consumed revision per new snapshot; endpoint may include an in-flight warmup revision. Timing fields are elapsed wall time, including waits, not CPU utilization; GUI event totals can include nested event delivery. Native render threading is observed via render_thread_is_gui."}};
}
}
int main(int argc,char** argv){
    try{
        QCommandLineParser parser;parser.addHelpOption();
        parser.addOption({"host","Host topology: widget, window or shared","host","widget"});
        parser.addOption({"geometry","Geometry handoff: typed or variant","path","typed"});
        parser.addOption({"fps","Supply target: 60, 90, 120 or 150","fps","120"});
        parser.addOption({"single-fft","One 2048-bin FFT line instead of seven small plots"});
        parser.addOption({"duration-ms","Measured duration, 1000 through 30000 ms","duration","4000"});
        parser.addOption({"swap-interval","Requested swap interval: 0 or 1 (diagnostic)","interval","1"});
        parser.addOption({"output","JSON report path (required)","path"});
        // Read the format request before QApplication or any GL context exists.
        QStringList arguments;for(int i=0;i<argc;++i)arguments.append(QString::fromLocal8Bit(argv[i]));
        require(parser.parse(arguments),"invalid command line (use --help)");
        bool validInterval=false;const int swapInterval=parser.value("swap-interval").toInt(&validInterval);
        require(validInterval&&(swapInterval==0||swapInterval==1),"--swap-interval must be 0 or 1");
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
        auto format=QSurfaceFormat::defaultFormat();format.setSamples(8);format.setSwapInterval(swapInterval);QSurfaceFormat::setDefaultFormat(format);
        ui::quick::configure();
        format=QSurfaceFormat::defaultFormat();format.setSwapInterval(swapInterval);QSurfaceFormat::setDefaultFormat(format);
        ProbeApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);parser.process(app);
        const auto host=parser.value("host");const int fps=parser.value("fps").toInt(),duration=parser.value("duration-ms").toInt();
        const auto geometryPath=parser.value("geometry");require(geometryPath=="typed"||geometryPath=="variant","--geometry must be typed or variant");
        require(host=="widget"||host=="window"||host=="shared","--host must be widget, window or shared");require(fps==60||fps==90||fps==120||fps==150,"--fps must be 60, 90, 120 or 150");
        require(duration>=1000&&duration<=30000,"--duration-ms must be 1000 through 30000");require(!parser.value("output").isEmpty(),"--output is required");
        const auto result=parser.isSet("single-fft")?run<1,2048>(app,host,fps,duration,swapInterval,geometryPath):run<7,256>(app,host,fps,duration,swapInterval,geometryPath);QSaveFile file(parser.value("output"));require(file.open(QIODevice::WriteOnly),"cannot open report");
        const auto bytes=QJsonDocument(result).toJson();require(file.write(bytes)==bytes.size()&&file.commit(),"cannot save report");
        return result.value("valid").toBool()?0:2;
    }catch(const std::exception& error){std::fprintf(stderr,"render_topology_probe: %s\n",error.what());return 1;}
}
