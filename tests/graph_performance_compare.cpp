#include <core_app/graph_panel.h>
#include <visualization/charts/line.h>
#include <visualization/numeric_image_view.h>
#include <visualization/numeric_pie_view.h>
#include <visualization/numeric_metrics_view.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <visualization/ui_text.h>
#include "performance_report.h"
#include "renderer_probe_telemetry.h"
#include <QApplication>
#include <QGridLayout>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScreen>
#include <QSysInfo>
#include <QStringList>
#include <QTimer>
#include <QThread>
#include <QPaintEngine>
#include <QWindow>
#include <QImage>
#include <QRectF>
#include <QResizeEvent>
#include <QtMath>
#include <cstring>
#include <functional>
#include <memory>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <QtGui/qscreen_platform.h>
#include <shobjidl.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#ifdef NUMERIC_QML
#include <visualization/quick/quick_ui.h>
#include <visualization/quick2d/plot_model.h>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QQuickItem>
#include <QSGRendererInterface>
#include <QQmlDebuggingEnabler>
#else
#include <QOpenGLWidget>
#endif

namespace {
constexpr int warmupMs=800, sampleMs=3000;
int targetRate(const QString& workload){return workload.startsWith("live")?workload.mid(4).toInt():120;}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
QJsonArray dimensions(const QSize& size){return {size.width(),size.height()};}
QJsonArray rectangle(const QRect& rect){return {rect.x(),rect.y(),rect.width(),rect.height()};}
quint64 hashBytes(quint64 hash,const void* source,size_t count){
    const auto* bytes=static_cast<const unsigned char*>(source);
    for(size_t i=0;i<count;++i){hash^=bytes[i];hash*=1099511628211ULL;}return hash;
}
quint64 imageHash(const QImage& image){
    if(image.isNull())return 0;
    quint64 hash=1469598103934665603ULL;
    for(int row=0;row<image.height();++row)hash=hashBytes(hash,image.constScanLine(row),size_t(image.width())*image.depth()/8);
    return hash;
}
QString hashText(quint64 hash){return QString::number(hash,16);}
#ifdef Q_OS_WIN
HWND ownRoot(const QWidget& window){
    const auto handle=reinterpret_cast<HWND>(window.winId());const auto root=GetAncestor(handle,GA_ROOT);return root?root:handle;
}
bool nativeForegroundMatches(const QWidget& window){
    const auto foreground=GetForegroundWindow(),root=ownRoot(window);
    if(!foreground||!root)return false;
    DWORD foregroundPid=0,ownPid=0;GetWindowThreadProcessId(foreground,&foregroundPid);GetWindowThreadProcessId(root,&ownPid);
    if(foregroundPid!=GetCurrentProcessId()||ownPid!=GetCurrentProcessId())return false;
    // A native Qt wrapper or an owned popup may be the foreground HWND. Both
    // must belong to this process and resolve to this exact owned root window.
    return foreground==root||GetAncestor(foreground,GA_ROOT)==root||GetAncestor(foreground,GA_ROOTOWNER)==root;
}
QJsonObject nativeWindowIdentity(const QWidget& window){
    const auto describe=[](HWND handle){
        DWORD pid=0;const DWORD thread=handle?GetWindowThreadProcessId(handle,&pid):0;
        wchar_t name[256]{};if(handle)GetClassNameW(handle,name,256);
        return QJsonObject{{"hwnd",QString::number(reinterpret_cast<quintptr>(handle),16)},
            {"class_name",QString::fromWCharArray(name)},{"pid",double(pid)},{"thread_id",double(thread)}};
    };
    const auto foreground=GetForegroundWindow(),own=reinterpret_cast<HWND>(window.winId());
    return {{"current_pid",double(GetCurrentProcessId())},{"foreground",describe(foreground)},
        {"foreground_root",describe(GetAncestor(foreground,GA_ROOT))},{"foreground_root_owner",describe(GetAncestor(foreground,GA_ROOTOWNER))},
        {"own",describe(own)},{"own_root",describe(GetAncestor(own,GA_ROOT))},{"own_root_owner",describe(GetAncestor(own,GA_ROOTOWNER))},
        {"direct_hwnd_match",foreground==own},{"owned_root_match",nativeForegroundMatches(window)}};
}
QJsonObject activateOwnedWindow(QWidget& window){
    const auto target=ownRoot(window);
    QJsonObject result{{"before",nativeWindowIdentity(window)}};
    DWORD targetPid=0;if(target)GetWindowThreadProcessId(target,&targetPid);
    if(!target||targetPid!=GetCurrentProcessId()){result["activation_refused_foreign_or_missing_root"]=true;return result;}
    result["initial_set_foreground_return"]=bool(SetForegroundWindow(target));
    bool attachAttempted=false,attached=false,attachedForegroundReturn=false;
    if(!nativeForegroundMatches(window)){
        const auto foreground=GetForegroundWindow();const DWORD currentThread=GetCurrentThreadId();
        const DWORD foregroundThread=foreground?GetWindowThreadProcessId(foreground,nullptr):0;
        if(foregroundThread&&foregroundThread!=currentThread){
            attachAttempted=true;attached=bool(AttachThreadInput(currentThread,foregroundThread,TRUE));
            if(attached){
                struct Detach {DWORD own,other;~Detach(){AttachThreadInput(own,other,FALSE);}} detach{currentThread,foregroundThread};
                BringWindowToTop(target);SetActiveWindow(target);attachedForegroundReturn=bool(SetForegroundWindow(target));
            }
        }
    }
    result["attach_thread_input_attempted"]=attachAttempted;result["attach_thread_input_succeeded"]=attached;
    result["attached_set_foreground_return"]=attachedForegroundReturn;result["after"]=nativeWindowIdentity(window);
    return result;
}
#else
QJsonObject nativeWindowIdentity(const QWidget&){return {};}
#endif
struct ForegroundScreen {
    QScreen* screen=nullptr;QString monitorDevice,mappingMethod;bool mapped=false;QJsonObject routing;QRect nativeGeometry,nativeAvailable;
};
ForegroundScreen foregroundScreen(){
    ForegroundScreen result;
#ifdef Q_OS_WIN
    const auto foreground=GetForegroundWindow();
    const auto foregroundMonitor=foreground?MonitorFromWindow(foreground,MONITOR_DEFAULTTONEAREST):nullptr;
    MONITORINFOEXW info{};info.cbSize=sizeof(info);
    if(foregroundMonitor&&GetMonitorInfoW(foregroundMonitor,reinterpret_cast<MONITORINFO*>(&info))){
        result.monitorDevice=QString::fromWCharArray(info.szDevice);
        result.nativeGeometry={info.rcMonitor.left,info.rcMonitor.top,info.rcMonitor.right-info.rcMonitor.left,info.rcMonitor.bottom-info.rcMonitor.top};
        result.nativeAvailable={info.rcWork.left,info.rcWork.top,info.rcWork.right-info.rcWork.left,info.rcWork.bottom-info.rcWork.top};
        for(DWORD index=0;;++index){DISPLAY_DEVICEW device{};device.cb=sizeof(device);
            if(!EnumDisplayDevicesW(nullptr,index,&device,0))break;
            if(result.monitorDevice.compare(QString::fromWCharArray(device.DeviceName),Qt::CaseInsensitive)==0){
                result.routing={{"device_name",QString::fromWCharArray(device.DeviceName)},{"device_string",QString::fromWCharArray(device.DeviceString)},
                    {"device_id",QString::fromWCharArray(device.DeviceID)},{"state_flags",double(device.StateFlags)}};break;
            }
        }
        for(auto* screen:QGuiApplication::screens()){
            const auto* native=screen->nativeInterface<QNativeInterface::QWindowsScreen>();
            if(native&&native->handle()==foregroundMonitor){result.screen=screen;result.mapped=true;result.mappingMethod="public_qt_windows_screen_hmonitor";break;}
        }
        if(!result.screen)for(auto* screen:QGuiApplication::screens())if(screen->name().compare(result.monitorDevice,Qt::CaseInsensitive)==0){result.screen=screen;result.mapped=true;result.mappingMethod="qscreen_name";break;}
        if(!result.screen)result.screen=QGuiApplication::screenAt(QPoint((info.rcMonitor.left+info.rcMonitor.right)/2,(info.rcMonitor.top+info.rcMonitor.bottom)/2));
    }
#endif
    if(!result.screen)result.screen=QGuiApplication::primaryScreen();
    return result;
}
class CurrentDesktopProbe {
public:
    CurrentDesktopProbe(){
#ifdef Q_OS_WIN
        const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);uninitialize_=SUCCEEDED(initialized);
        const CLSID classId{0xaa509086,0x5ca9,0x4c25,{0x8f,0x95,0x58,0x9d,0x3c,0x07,0xb4,0x8a}};
        const IID interfaceId{0xa5cd92ff,0x29be,0x454c,{0x8d,0x04,0xd8,0x28,0x79,0xfb,0x3f,0x1b}};
        CoCreateInstance(classId,nullptr,CLSCTX_INPROC_SERVER,interfaceId,reinterpret_cast<void**>(&manager_));
#endif
    }
    ~CurrentDesktopProbe(){
#ifdef Q_OS_WIN
        if(manager_)manager_->Release();if(uninitialize_)CoUninitialize();
#endif
    }
    int contains(const QWidget& window) const {
#ifdef Q_OS_WIN
        BOOL current=FALSE;
        if(manager_&&SUCCEEDED(manager_->IsWindowOnCurrentVirtualDesktop(reinterpret_cast<HWND>(window.winId()),&current)))return current?1:0;
#else
        Q_UNUSED(window);
#endif
        return -1;
    }
private:
#ifdef Q_OS_WIN
    IVirtualDesktopManager* manager_=nullptr;bool uninitialize_=false;
#endif
};
class WindowWatch final:public QObject {
public:
    using QObject::QObject;
    std::function<void(QEvent::Type)> changed;
    bool eventFilter(QObject*,QEvent* event) override {
        if(changed&&(event->type()==QEvent::WindowDeactivate||event->type()==QEvent::Hide||event->type()==QEvent::WindowStateChange))changed(event->type());
        return false;
    }
};
class RendererSizeWatch final:public QObject {
public:
    std::function<void()> resized;
    bool eventFilter(QObject*,QEvent* event) override {
        if(event->type()==QEvent::Resize){const auto* resize=static_cast<QResizeEvent*>(event);
            if(resized&&resize->size()!=resize->oldSize())resized();
        }
        return false;
    }
};
QJsonObject statistics(std::vector<double> values){
    if(values.empty())return {{"count",0}};
    std::sort(values.begin(),values.end());
    const auto percentile=[&](double fraction){return values[std::min(values.size()-1,size_t(std::ceil(fraction*values.size())-1))];};
    return {{"count",double(values.size())},{"p50_ms",percentile(.50)},{"p95_ms",percentile(.95)},{"max_ms",values.back()}};
}
// notify() returns after the selected renderer's paintEvent has completed.
// An event filter alone would count entry, before the renderer actually paints.
class PerfApplication final : public QApplication {
public:
    using QApplication::QApplication;
    QObject* paintTarget=nullptr;
    bool measuring=false;
    quint64 paints=0;
    std::vector<double> paintDurations;
    bool notify(QObject* receiver,QEvent* event) override {
        const bool selectedPaint=measuring && receiver==paintTarget && event->type()==QEvent::Paint;
        const bool profile=core::perf::enabled.load(std::memory_order_relaxed) && QThread::currentThread()==thread();
        if(!selectedPaint && !profile)return QApplication::notify(receiver,event);
        // Cache identity before delivery: a deferred-delete event can destroy receiver.
        std::array<char,64> receiverClass{};
        if(profile)std::snprintf(receiverClass.data(),receiverClass.size(),"%s",receiver->metaObject()->className());
        const int eventType=int(event->type());
        const auto started=core::perf::now();
        const bool result=QApplication::notify(receiver,event);
        const auto duration=core::perf::now()-started;
        if(selectedPaint){
            ++paints;
            if(paintDurations.size()<100000)paintDurations.push_back(duration/1e6);
        }
        if(profile && core::perf::enabled.load(std::memory_order_relaxed)){
            using namespace core::perf;
            record(Stage::GuiEvent,started,duration);
            // Keep the sixteen longest deliveries, without allocating in notify.
            const size_t capacity=slow_gui_events.size();
            if(slow_gui_event_count<capacity || duration>slow_gui_events[capacity-1].duration){
                const size_t count=std::min(slow_gui_event_count+1,capacity);
                size_t position=count-1;
                while(position>0 && slow_gui_events[position-1].duration<duration){slow_gui_events[position]=slow_gui_events[position-1];--position;}
                auto& slow=slow_gui_events[position];slow.duration=duration;slow.type=eventType;
                slow.receiver=receiverClass;
                slow_gui_event_count=count;
            }
        }
        return result;
    }
};
void resetProfile(){
    using namespace core::perf;
    enabled=false;
    for(auto& counter:counters){counter.count=0;counter.total=0;counter.maximum=0;for(auto& bin:counter.histogram)bin=0;}
    for(auto& event:events)event.ready=false;
    event_count=0;slow_gui_event_count=0;
}
struct Signal {
    std::vector<float> vector=std::vector<float>(256);
    std::vector<std::vector<float>> history=std::vector<std::vector<float>>(400,std::vector<float>(50));
    std::array<std::vector<float>,8> toneBasis;
    bool rolling=false;
    quint64 revision=0;
    void rollingRow(std::vector<float>& row,double time){
        std::array<double,8> centers{},gains{};
        for(size_t tone=0;tone<centers.size();++tone){
            centers[tone]=2.+tone*6.+2.*std::sin(time*1.3+tone*.7);
            gains[tone]=.9*(.65+.35*std::sin(time*1.7+tone));
        }
        for(size_t band=0;band<row.size();++band){double value=.02;
            for(size_t tone=0;tone<centers.size();++tone){const double delta=(double(band)-centers[tone])/.8;value+=gains[tone]*std::exp(-delta*delta);}
            row[band]=float(value);
        }
    }
    explicit Signal(bool matrix,bool rollingHistory=false,int historyRate=120):rolling(rollingHistory){
        if(!matrix)return;
        if(rolling){for(size_t frame=0;frame<history.size();++frame)rollingRow(history[frame],(double(frame)-double(history.size()-1))/historyRate);return;}
        // Expensive ridge construction is setup, outside warmup and sampling.
        // Only eight changing gains and bounded multiply/adds run per frame.
        for(size_t tone=0;tone<toneBasis.size();++tone){
            auto& basis=toneBasis[tone];basis.resize(400*50);
            for(size_t frame=0;frame<400;++frame){
                const double center=2.+tone*6.+1.8*std::sin(frame*.012+tone);
                for(size_t band=0;band<50;++band){const double delta=(double(band)-center)/1.2;basis[frame*50+band]=float(.11*std::exp(-delta*delta));}
            }
        }
    }
    void generate(double phase,bool matrix){
        ++revision;
        for(size_t i=0;i<vector.size();++i){double value=0;for(int tone=1;tone<=8;++tone)value+=.08*std::sin(tone*i*.05+phase*(.6+.1*tone));vector[i]=float(value);}
        if(!matrix)return;
        if(rolling){
            // Reuse the oldest row's owned storage and append one genuinely new
            // row. The public fixture remains [frame][band], oldest to newest.
            std::rotate(history.begin(),history.begin()+1,history.end());rollingRow(history.back(),phase);return;
        }
        std::array<float,8> gains;
        for(size_t tone=0;tone<gains.size();++tone)gains[tone]=float(.35+.65*(.5+.5*std::sin(phase*(.6+.1*tone)+tone)));
        // Explicit [frame][band] order, oldest to newest. Eight varying tones.
        for(size_t frame=0;frame<history.size();++frame)for(size_t band=0;band<50;++band){
            float value=.02f;const size_t index=frame*50+band;
            for(size_t tone=0;tone<toneBasis.size();++tone)value+=gains[tone]*toneBasis[tone][index];
            history[frame][band]=value;
        }
    }
    quint64 hash(bool matrix) const {
        quint64 result=1469598103934665603ULL;
        if(matrix){for(const auto& row:history)result=hashBytes(result,row.data(),row.size()*sizeof(float));}
        else result=hashBytes(result,vector.data(),vector.size()*sizeof(float));
        return result;
    }
};
QWidget* rendererFor(GraphPanel& panel,const QString& mode){
    if(mode=="surface3d")return panel.active_spectrogram();
    if(mode=="image2d")return panel.image_view();
    if(mode=="pie")return panel.pie_view();
    if(mode=="metrics")return panel.metrics_view();
    return panel.active_chart();
}
QJsonObject runCase(PerfApplication& app,const QString& mode,const QString& workload,const QString& tracePath,int metricCount,bool interactive=false){
    const bool live=workload.startsWith("live");
    const int targetFps=targetRate(workload);
    const qint64 periodNs=1000000000LL/targetFps;
    const bool vectorMode=mode=="line"||mode=="points"||mode=="bars";
    const bool native2dMode=vectorMode||mode=="pie";
    const QString kind=mode=="metrics"?"ScalarView":(mode=="image2d"||mode=="surface3d")?"SpectrogramView":"FFTView";
    const int caseWarmupMs=interactive?2000:warmupMs,caseSampleMs=interactive?10000:sampleMs;
    // Snapshot the foreground monitor before constructing this owned test window.
    const auto selectedScreen=interactive?foregroundScreen():ForegroundScreen{};
    check(!interactive||(selectedScreen.mapped&&selectedScreen.screen),"foreground native monitor could not be reliably mapped to a Qt screen; test window was not created or activated");
    std::unique_ptr<CurrentDesktopProbe> desktop;if(interactive)desktop=std::make_unique<CurrentDesktopProbe>();
    GraphPanel panel("performanceCompare",new LineChartView,{{"kind",kind},{"display_mode",mode}},nullptr);
    panel.set_render_fps(targetFps);
    if(!interactive){panel.setAttribute(Qt::WA_ShowWithoutActivating);panel.setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);}
    if(interactive)panel.setWindowTitle(QString("Owned graph test | %1 | %2 | warmup").arg(mode,workload));
    panel.resize(1280,800);
    if(interactive&&selectedScreen.screen){
        panel.winId();panel.windowHandle()->setScreen(selectedScreen.screen);
        const auto available=selectedScreen.screen->availableGeometry();
        panel.move(available.topLeft()+QPoint(32,32));
    }
    const bool matrix=mode=="surface3d"||mode=="image2d";
    Signal signal(matrix,interactive&&matrix,targetFps);signal.generate(0,matrix);
    if(mode=="surface3d"){
        panel.active_spectrogram()->set_history_dimensions(400,50);
        panel.active_spectrogram()->set_y_range(0,1.2);
        panel.active_spectrogram()->set_render_fps(targetFps);
    }else if(mode=="image2d")panel.image_view()->set_y_range(false,0,1.2,10);
    else if(mode=="metrics"){
        QList<NumericMetricsView::Input> inputs;
        for(quint16 i=0;i<metricCount;++i)inputs.append({i,QString("Sample %1").arg(i+1)});
        panel.metrics_view()->set_inputs(inputs);
    }else if(mode!="pie"){
        panel.active_chart()->set_auto_resizing(false);panel.active_chart()->set_range(-1,1);panel.active_chart()->set_render_fps(targetFps);
    }
    const auto publish=[&]{
        if(mode=="surface3d")panel.active_spectrogram()->update_frame_history(signal.history);
        else if(mode=="image2d")panel.image_view()->set_matrix(signal.history,true);
        else if(mode=="pie"){
            // Pie requires nonnegative values; retain all 256 samples.
            auto values=signal.vector;for(auto& value:values)value+=1;
            panel.pie_view()->set_values(values);
        }else if(mode=="metrics")for(quint16 i=0;i<metricCount;++i)panel.metrics_view()->set_value(i,QString::number(signal.vector[i*256/metricCount],'g',12));
        else panel.active_chart()->update(signal.vector);
    };
    publish();
    QJsonObject activationIdentity;
    if(interactive){
        panel.showMaximized();panel.raise();panel.activateWindow();
#ifdef Q_OS_WIN
        activationIdentity=activateOwnedWindow(panel);
#endif
    }else panel.show();
    auto* renderer=rendererFor(panel,mode);check(renderer,"active renderer absent");
    QWidget* repaintTarget=renderer;
    QString completionScope;
    std::atomic<quint64> sceneFrames{0},sceneTotalNs{0},sceneMaxNs{0},sceneBegin{0};
    std::atomic<bool> sceneMeasuring{false};
    RendererProbeTelemetry rendererIdentity;
    QMetaObject::Connection beforeConnection,afterConnection;
    quint64 modelPublications=0;
    QMetaObject::Connection publicationConnection;
#ifdef NUMERIC_QML
    ui::quick::SceneReference quick(dynamic_cast<QuickPlotWidget*>(renderer)?renderer:renderer->findChild<QQuickWidget*>("spectrogramQuickView"));
    check(bool(quick),"renderer Quick scene absent");
    if(auto* plot=dynamic_cast<QuickPlotWidget*>(renderer))
        publicationConnection=QObject::connect(plot->presentation_model(),&QuickPlotModel::changed,&panel,[&]{
            if(sceneMeasuring.load())++modelPublications;
        },Qt::DirectConnection);
    beforeConnection=QObject::connect(quick->quickWindow(),&QQuickWindow::beforeRendering,quick.owner(),[&]{sceneBegin=core::perf::now();},Qt::DirectConnection);
    afterConnection=QObject::connect(quick->quickWindow(),&QQuickWindow::afterRendering,quick.owner(),[&]{
        rendererIdentity.capture();
        if(!sceneMeasuring.load())return;
        const auto duration=core::perf::now()-sceneBegin.load();
        ++sceneFrames;sceneTotalNs.fetch_add(duration);
        auto previous=sceneMaxNs.load();while(duration>previous && !sceneMaxNs.compare_exchange_weak(previous,duration)){}
    },Qt::DirectConnection);
    check(quick->rootObject(),"renderer QML root absent");
    auto* surface=mode=="surface3d"?quick->rootObject()->findChild<QQuickItem*>("spectrogramContainer"):nullptr;
    check(mode!="surface3d"||surface,"surface FBO item absent");
    auto* vectorPlot=native2dMode?quick->rootObject()->findChild<QQuickItem*>(mode=="pie"?"numericPiePlot":"numericVectorPlot"):nullptr;
    check(!native2dMode||vectorPlot,"native 2D paint item absent");
    const auto requestRender=[&]{
        if(surface)surface->update();
        else if(vectorPlot)QMetaObject::invokeMethod(vectorPlot,"requestPaint",Qt::DirectConnection);
        else quick->quickWindow()->update();
        quick.owner()->update();
    };
    completionScope="QQuickWindow afterRendering; scene submission, not GPU completion or presentation";
#else
    if(mode=="surface3d")repaintTarget=renderer->findChild<QOpenGLWidget*>("spectrogramContainer");
    else if(mode=="line"||mode=="points"||mode=="bars")repaintTarget=panel.active_chart()->viewport();
    else if(mode=="pie")repaintTarget=panel.pie_view()->viewport();
    else if(mode=="metrics")repaintTarget=renderer->findChild<QWidget*>("metricValue_0");
    check(repaintTarget,"paint target absent");
    app.paintTarget=repaintTarget;
    const auto requestRender=[&]{
        if(mode=="metrics"){
            renderer->update();
            for(auto* child:renderer->findChildren<QWidget*>())if(child->isVisible())child->update();
        }else repaintTarget->update();
    };
    completionScope="selected QWidget paint event completed; not GPU completion or presentation";
    if(mode=="metrics")completionScope="first metric value QLabel paint completed; static redraws full metrics subtree; not a presented frame";
#endif
    struct FocusChecks {quint64 count=0,failures=0;bool foreground=true,active=true,visible=true,maximized=true,screen=true;QJsonObject firstFailureNative;};
    FocusChecks warmupFocus,sampleFocus;
    bool observingWindow=false,windowSamplePhase=false;
    quint64 windowObservationNs=0;
    const auto observeWindow=[&]{
        if(!observingWindow)return;
        const auto started=core::perf::now();auto& checks=windowSamplePhase?sampleFocus:warmupFocus;
        const bool active=panel.isActiveWindow(),visible=panel.isVisible()&&!panel.isMinimized();
        bool maximized=panel.isMaximized()&&!panel.isFullScreen();
        bool screen=panel.windowHandle()&&panel.windowHandle()->screen()==selectedScreen.screen;
        bool foreground=active;
#ifdef Q_OS_WIN
        foreground=nativeForegroundMatches(panel);
        const auto root=ownRoot(panel);
        const auto* selectedNative=selectedScreen.screen?selectedScreen.screen->nativeInterface<QNativeInterface::QWindowsScreen>():nullptr;
        screen=screen&&selectedNative&&MonitorFromWindow(root,MONITOR_DEFAULTTONEAREST)==selectedNative->handle();
        maximized=maximized&&IsZoomed(root)&&!IsIconic(root);
#endif
        ++checks.count;if(!(active&&visible&&maximized&&screen&&foreground)){
            ++checks.failures;if(checks.firstFailureNative.isEmpty())checks.firstFailureNative=nativeWindowIdentity(panel);
        }
        checks.active&=active;checks.visible&=visible;checks.maximized&=maximized;checks.screen&=screen;checks.foreground&=foreground;
        windowObservationNs+=core::perf::now()-started;
    };
    WindowWatch windowWatch;windowWatch.changed=[&](QEvent::Type type){
        if(!observingWindow)return;
        if(type==QEvent::WindowDeactivate||type==QEvent::Hide){
            // These events are delivered before QWidget finishes updating its
            // state. Latch the transition itself, even if focus later returns.
            auto& checks=windowSamplePhase?sampleFocus:warmupFocus;++checks.count;++checks.failures;
            if(type==QEvent::WindowDeactivate)checks.active=false;else checks.visible=false;
            if(checks.firstFailureNative.isEmpty())checks.firstFailureNative=nativeWindowIdentity(panel);
        }
        if(type==QEvent::WindowStateChange){
            bool maximized=panel.isMaximized()&&!panel.isFullScreen();
#ifdef Q_OS_WIN
            const auto root=ownRoot(panel);maximized=maximized&&IsZoomed(root)&&!IsIconic(root);
#endif
            // The transition itself is evidence; a later queued check may see
            // an already restored maximized window and must not erase this loss.
            if(!maximized){auto& checks=windowSamplePhase?sampleFocus:warmupFocus;++checks.count;++checks.failures;checks.maximized=false;
                if(checks.firstFailureNative.isEmpty())checks.firstFailureNative=nativeWindowIdentity(panel);
            }
        }
        QMetaObject::invokeMethod(&panel,observeWindow,Qt::QueuedConnection);
    };
    if(interactive)panel.installEventFilter(&windowWatch);
    struct RendererGeometry {QSize rendererSize,viewportSize;QRectF crop; qreal dpr=0;};
    const auto rendererGeometry=[&]{
        RendererGeometry g;g.rendererSize=renderer->size();g.viewportSize=g.rendererSize;g.dpr=renderer->devicePixelRatioF();
#ifdef NUMERIC_QML
        g.viewportSize=quick->size();g.dpr=quick->quickWindow()->effectiveDevicePixelRatio();
        QQuickItem* area=surface?surface:vectorPlot;
        if(mode=="image2d")area=quick->rootObject()->findChild<QQuickItem*>("numericRaster");
        if(!area)area=quick->rootObject();
        if(area)g.crop=area->mapRectToScene(area->boundingRect());
#else
        g.crop=QRectF(repaintTarget->rect());
#endif
        return g;
    };
    const auto sameGeometry=[](const RendererGeometry& a,const RendererGeometry& b){
        return a.rendererSize==b.rendererSize&&a.viewportSize==b.viewportSize&&a.crop==b.crop&&a.dpr==b.dpr;
    };
    RendererGeometry baselineGeometry,endGeometry;
    bool observingGeometry=false,geometryStable=true;
    quint64 geometryChecks=0,geometryChanges=0;
    const auto observeGeometry=[&]{if(!observingGeometry)return;++geometryChecks;
        if(!sameGeometry(baselineGeometry,rendererGeometry())){geometryStable=false;++geometryChanges;}
    };
    // Disconnect callbacks before their captured locals unwind, including errors.
    QObject geometryCallbackContext;
    RendererSizeWatch rendererSizeWatch;rendererSizeWatch.resized=[&]{
        if(observingGeometry){geometryStable=false;++geometryChanges;}observeGeometry();
    };
    if(interactive){renderer->installEventFilter(&rendererSizeWatch);
#ifdef NUMERIC_QML
        if(quick.owner()!=renderer)quick.owner()->installEventFilter(&rendererSizeWatch);
#endif
    }
    std::vector<QMetaObject::Connection> geometryConnections;
#ifdef NUMERIC_QML
    if(interactive){
        QQuickItem* area=surface?surface:vectorPlot;
        if(mode=="image2d")area=quick->rootObject()->findChild<QQuickItem*>("numericRaster");
        if(!area)area=quick->rootObject();
        // Ancestor movement/resizing can change the scene crop even when the
        // graph item's own dimensions remain unchanged. Latch at delivery.
        for(auto* item=area;item;item=item->parentItem()){
            geometryConnections.push_back(QObject::connect(item,&QQuickItem::widthChanged,&geometryCallbackContext,observeGeometry));
            geometryConnections.push_back(QObject::connect(item,&QQuickItem::heightChanged,&geometryCallbackContext,observeGeometry));
            geometryConnections.push_back(QObject::connect(item,&QQuickItem::xChanged,&geometryCallbackContext,observeGeometry));
            geometryConnections.push_back(QObject::connect(item,&QQuickItem::yChanged,&geometryCallbackContext,observeGeometry));
            geometryConnections.push_back(QObject::connect(item,&QQuickItem::scaleChanged,&geometryCallbackContext,observeGeometry));
            geometryConnections.push_back(QObject::connect(item,&QQuickItem::rotationChanged,&geometryCallbackContext,observeGeometry));
        }
    }
#endif
    const auto captureOwnRenderer=[&]()->QImage{
        if(!interactive)return {};
#ifdef NUMERIC_QML
        const auto framebuffer=quick->grabFramebuffer();
        QQuickItem* area=surface?surface:vectorPlot;
        if(mode=="image2d")area=quick->rootObject()->findChild<QQuickItem*>("numericRaster");
        if(!area)area=quick->rootObject();
        if(framebuffer.isNull()||!area||!area->isVisible()||quick.owner()->width()<=0||quick.owner()->height()<=0)return {};
        const auto logical=area->mapRectToScene(area->boundingRect());
        const double scaleX=double(framebuffer.width())/quick.owner()->width(),scaleY=double(framebuffer.height())/quick.owner()->height();
        const QRect crop(qFloor(logical.x()*scaleX),qFloor(logical.y()*scaleY),qCeil(logical.width()*scaleX),qCeil(logical.height()*scaleY));
        return framebuffer.copy(crop.intersected(framebuffer.rect()));
#else
        if(auto* gl=qobject_cast<QOpenGLWidget*>(repaintTarget))return gl->grabFramebuffer();
        return repaintTarget->grab().toImage();
#endif
    };
    QTimer producer,heartbeat,end;
    producer.setTimerType(Qt::PreciseTimer);producer.setSingleShot(live);
    heartbeat.setTimerType(Qt::PreciseTimer);heartbeat.setInterval(10);
    end.setTimerType(Qt::PreciseTimer);end.setSingleShot(true);
    QElapsedTimer clock;clock.start();
    qint64 nextUpdate=periodNs,nextHeartbeat=10000000;
    std::vector<double> updates,lateness,generation;
    updates.reserve(interactive?1300:300);lateness.reserve(interactive?1300:1000);generation.reserve(interactive?1300:300);
    app.paintDurations.clear();app.paintDurations.reserve(100000);app.paints=0;
    bool measuring=false;
    struct DynamicObservation {qint64 elapsedNs=0;quint64 revision=0,sourceHash=0,rasterHash=0;};
    std::vector<DynamicObservation> dynamicObservations;dynamicObservations.reserve(48);
    quint64 dynamicObservationNs=0;
    qint64 nextDynamicObservation=0,nextWindowObservation=0;
    const auto observeDynamic=[&]{
        if(!interactive)return;
        const auto started=core::perf::now();
        dynamicObservations.push_back({clock.nsecsElapsed(),signal.revision,signal.hash(matrix),mode=="image2d"?imageHash(panel.image_view()->numeric_raster()):0});
        dynamicObservationNs+=core::perf::now()-started;
    };
    quint64 updateRequests=0,repaintRequests=0,missedUpdatePeriods=0;
    const auto armProducer=[&]{producer.start(int(std::max<qint64>(1,(nextUpdate-clock.nsecsElapsed()+999999)/1000000)));};
    QObject::connect(&producer,&QTimer::timeout,&panel,[&]{
        if(live){
            const auto now=clock.nsecsElapsed();
            if(measuring)lateness.push_back(std::max(0.,double(now-nextUpdate)/1e6));
            const auto start=core::perf::now();signal.generate(now/1e9,matrix);
            const auto generated=core::perf::now();publish();const auto finished=core::perf::now();
            if(measuring){generation.push_back((generated-start)/1e6);updates.push_back((finished-generated)/1e6);++updateRequests;}
            nextUpdate+=periodNs;
            if(nextUpdate<=clock.nsecsElapsed()){
                const auto missed=(clock.nsecsElapsed()-nextUpdate)/periodNs+1;
                if(measuring)missedUpdatePeriods+=quint64(missed);
                nextUpdate+=missed*periodNs;
            }
            armProducer();
        }else {requestRender();if(measuring)++repaintRequests;}
    });
    std::vector<double> heartbeatLateness;heartbeatLateness.reserve(interactive?1100:500);
    QObject::connect(&heartbeat,&QTimer::timeout,&panel,[&]{
        const auto now=clock.nsecsElapsed();
        if(measuring)heartbeatLateness.push_back(std::max(0.,double(now-nextHeartbeat)/1e6));
        nextHeartbeat=now+10000000;
        if(interactive&&now>=nextWindowObservation){observeWindow();observeGeometry();nextWindowObservation=now+100000000;}
        if(interactive&&measuring&&now>=nextDynamicObservation){observeDynamic();nextDynamicObservation=now+250000000;}
    });
    if(live)armProducer();else producer.start(0);
    heartbeat.start();
    if(interactive){
        // Let the one authorized activation request settle. No focus requests
        // occur after this bounded setup interval, including when acquisition fails.
        QEventLoop settle;QTimer::singleShot(300,Qt::PreciseTimer,&settle,&QEventLoop::quit);settle.exec();
        observingWindow=true;observeWindow();
    }
    QEventLoop warmup;
    QTimer::singleShot(caseWarmupMs,Qt::PreciseTimer,&warmup,&QEventLoop::quit);warmup.exec();
#ifdef NUMERIC_QML
    check(quick->status()==QQuickWidget::Ready,"QML renderer failed to initialize");
#endif
    if(interactive)observeWindow();
    const int warmupDesktop=interactive?desktop->contains(panel):-1;
    const auto warmupNativeIdentity=interactive?nativeWindowIdentity(panel):QJsonObject{};
    if(interactive){
        panel.setWindowTitle(QString("Owned graph test | %1 | %2 | running 10 s").arg(mode,workload));
        // Finish deferred title/layout work before freezing the viewport oracle.
        QEventLoop layoutSettle;QTimer::singleShot(100,Qt::PreciseTimer,&layoutSettle,&QEventLoop::quit);layoutSettle.exec();
        QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
    }
    const QImage startFramebuffer=captureOwnRenderer();
    baselineGeometry=rendererGeometry();observingGeometry=interactive;observeGeometry();
    const quint64 startFramebufferHash=imageHash(startFramebuffer);
    const QString startFramebufferPath=tracePath+".renderer-start.png",endFramebufferPath=tracePath+".renderer-end.png";
    const bool startFramebufferSaved=interactive&&!startFramebuffer.isNull()&&startFramebuffer.save(startFramebufferPath);
    observeDynamic();
    const auto surfaceBefore=mode=="surface3d"?panel.active_spectrogram()->axis_layout_telemetry().value("paintCount").toULongLong():0;
    quint64 vectorPaintBefore=0,vectorConsumedBefore=0,vectorPaintAfter=0,vectorConsumedAfter=0;
    int vectorPaintEngine=-1;
    int nativePaintSamples=-1;
#ifdef NUMERIC_QML
    if(vectorPlot){
        vectorPaintBefore=vectorPlot->property("paintCount").toULongLong();
        vectorConsumedBefore=vectorPlot->property("consumedPresentationCount").toULongLong();
    }
#endif
    resetProfile();
    const auto measuredStart=clock.nsecsElapsed();
    if(live){nextUpdate=measuredStart+periodNs;armProducer();}
    windowSamplePhase=true;nextDynamicObservation=measuredStart+250000000;nextWindowObservation=measuredStart;
    measuring=true;app.measuring=true;sceneMeasuring=true;core::perf::enabled=true;
    QEventLoop sample;QObject::connect(&end,&QTimer::timeout,&sample,&QEventLoop::quit);
    end.start(caseSampleMs);sample.exec();
    const auto measuredEnd=clock.nsecsElapsed();
    measuring=false;app.measuring=false;sceneMeasuring=false;core::perf::enabled=false;
    observeGeometry();endGeometry=rendererGeometry();
    producer.stop();heartbeat.stop();
    if(live && nextUpdate<=measuredEnd)
        missedUpdatePeriods+=quint64((measuredEnd-nextUpdate)/periodNs+1);
    const auto surfaceAfter=mode=="surface3d"?panel.active_spectrogram()->axis_layout_telemetry().value("paintCount").toULongLong():0;
#ifdef NUMERIC_QML
    if(vectorPlot){
        vectorPaintAfter=vectorPlot->property("paintCount").toULongLong();
        vectorConsumedAfter=vectorPlot->property("consumedPresentationCount").toULongLong();
        vectorPaintEngine=vectorPlot->property("paintEngineType").toInt();
        nativePaintSamples=vectorPlot->property("paintSamples").toInt();
    }
#endif
    const auto geometryPublications=core::perf::counters[size_t(core::perf::Stage::ChartGeometry)].count.load();
    const double seconds=(measuredEnd-measuredStart)/1e9;
#ifdef NUMERIC_QML
    const auto completed=sceneFrames.load();
    const int graphicsApi=int(quick->quickWindow()->rendererInterface()->graphicsApi());
    QObject::disconnect(beforeConnection);QObject::disconnect(afterConnection);
    QObject::disconnect(publicationConnection);
#else
    const auto completed=app.paints;
    const int graphicsApi=mode=="surface3d"?2:0;
#endif
    const auto measuredSceneTotalNs=sceneTotalNs.load(),measuredSceneMaxNs=sceneMaxNs.load();
    if(interactive)observeWindow();
    observingWindow=false;
    const int sampleDesktop=interactive?desktop->contains(panel):-1;
    const auto sampleNativeIdentity=interactive?nativeWindowIdentity(panel):QJsonObject{};
    observeDynamic();
    // Both captures are app-owned renderer pixels outside the measured region;
    // graph-area cropping excludes GraphPanel FPS text and the window title.
    const QImage endFramebuffer=captureOwnRenderer();
    observeGeometry();observingGeometry=false;
    for(const auto& connection:geometryConnections)QObject::disconnect(connection);
    const quint64 endFramebufferHash=imageHash(endFramebuffer);
    const bool endFramebufferSaved=interactive&&!endFramebuffer.isNull()&&endFramebuffer.save(endFramebufferPath);
    if(interactive)panel.setWindowTitle(QString("Owned graph test | %1 | %2 | done").arg(mode,workload));
    const QSize measuredPanelSize=panel.size(),measuredRendererSize=renderer->size();
    const qreal measuredPanelDpr=panel.devicePixelRatioF();
    QRect physicalWindowGeometry;
#ifdef Q_OS_WIN
    RECT physicalRect{};
    if(GetWindowRect(reinterpret_cast<HWND>(panel.winId()),&physicalRect))physicalWindowGeometry={physicalRect.left,physicalRect.top,physicalRect.right-physicalRect.left,physicalRect.bottom-physicalRect.top};
#endif
#ifdef NUMERIC_QML
    const qreal measuredRendererDpr=quick->quickWindow()->effectiveDevicePixelRatio();
#else
    const qreal measuredRendererDpr=renderer->devicePixelRatioF();
#endif
    auto* measuredScreen=panel.windowHandle()?panel.windowHandle()->screen():panel.screen();
    QJsonArray dynamicSamples;quint64 sourceHashChanges=0,rasterHashChanges=0;
    for(size_t i=0;i<dynamicObservations.size();++i){const auto& observation=dynamicObservations[i];
        if(i){sourceHashChanges+=observation.sourceHash!=dynamicObservations[i-1].sourceHash;rasterHashChanges+=observation.rasterHash&&observation.rasterHash!=dynamicObservations[i-1].rasterHash;}
        dynamicSamples.append(QJsonObject{{"since_measurement_ms",(observation.elapsedNs-measuredStart)/1e6},{"source_revision",double(observation.revision)},
            {"source_hash",hashText(observation.sourceHash)},{"owned_raster_hash",hashText(observation.rasterHash)}});
    }
    const bool focusValid=!interactive||(warmupFocus.count>0&&sampleFocus.count>0&&!warmupFocus.failures&&!sampleFocus.failures&&warmupDesktop==1&&sampleDesktop==1&&selectedScreen.mapped);
    const bool geometryValid=!interactive||(geometryStable&&geometryChecks>0&&sameGeometry(baselineGeometry,endGeometry)
        &&!startFramebuffer.isNull()&&startFramebuffer.size()==endFramebuffer.size());
    const QString invalidGeometryPanelPath=tracePath+".invalid-geometry-owned-panel.png";
    const bool invalidGeometryPanelSaved=interactive&&!geometryValid&&panel.grab().save(invalidGeometryPanelPath);
    const bool dynamicValid=!interactive||(geometryValid&&sourceHashChanges>=2&&dynamicObservations.size()>2&&dynamicObservations.back().revision>dynamicObservations.front().revision&&startFramebufferSaved&&endFramebufferSaved
        &&startFramebufferHash!=endFramebufferHash&&(mode!="image2d"||rasterHashChanges>=2));
    check(save_performance_report(tracePath),"cannot save stage trace");
    QJsonObject result{{"mode",mode},{"workload",workload},{"warmup_ms",caseWarmupMs},{"requested_sample_ms",caseSampleMs},{"actual_sample_seconds",seconds},
        {"target_fps",targetFps},{"frame_budget_ms",1000./targetFps},{"supplied_hz",updateRequests/seconds},
        {"scheduled_live_deadlines",double(updateRequests+missedUpdatePeriods)},
        {"panel_logical_size",dimensions(measuredPanelSize)},{"renderer_logical_size",dimensions(measuredRendererSize)},{"dpr",measuredPanelDpr},
        {"renderer_effective_dpr",measuredRendererDpr},{"renderer_pixel_size_from_dpr",dimensions(QSize(qRound(measuredRendererSize.width()*measuredRendererDpr),qRound(measuredRendererSize.height()*measuredRendererDpr)))},
        {"window_frame_geometry",rectangle(panel.frameGeometry())},{"screen_name",measuredScreen?measuredScreen->name():QString{}},
        {"window_physical_geometry_win32",rectangle(physicalWindowGeometry)},
        {"screen_geometry",measuredScreen?rectangle(measuredScreen->geometry()):QJsonArray{}},{"screen_available_geometry",measuredScreen?rectangle(measuredScreen->availableGeometry()):QJsonArray{}},
        {"completed_renderer_frames",double(completed)},{"renderer_fps",completed/seconds},{"completion_scope",completionScope},
        {"geometry_publications",double(geometryPublications)},{"geometry_publications_fps",geometryPublications/seconds},
        {"vector_paint_completions",double(vectorPaintAfter-vectorPaintBefore)},{"vector_paint_fps",(vectorPaintAfter-vectorPaintBefore)/seconds},
        {"vector_unique_presentations_drawn",double(vectorConsumedAfter-vectorConsumedBefore)},
        {"vector_unique_presentations_fps",(vectorConsumedAfter-vectorConsumedBefore)/seconds},
        {"vector_paint_engine_type",vectorPaintEngine},{"vector_paint_engine_opengl2",vectorPaintEngine==int(QPaintEngine::OpenGL2)},
        {"native_paint_samples",nativePaintSamples},
        {"vector_counter_scope","native 2D item atomic endpoints: numericVectorPlot for vectors, numericPiePlot for pie; legacy vector_* keys apply to both; unavailable in Widgets; static explicitly requests raster repaint; scene frames may reuse textures; consumed count advances once for a new presentation revision"},
        {"surface_completed_frames",double(surfaceAfter-surfaceBefore)},{"surface_fps",(surfaceAfter-surfaceBefore)/seconds},
        {"surface_counter_scope","paintCount endpoints; QML feedback can lag or coalesce, never inferred from update requests"},
        {"update_requests",double(updateRequests)},{"repaint_requests",double(repaintRequests)},{"missed_live_periods",double(missedUpdatePeriods)},
        {"live_frame_supplies",double(updateRequests)},{"qml_model_publications",double(modelPublications)},
        {"qml_publications_per_live_supply",updateRequests?modelPublications/double(updateRequests):0.},
        {"qml_publication_scope","QuickPlotModel::changed emissions during sample; surface has no QuickPlotModel; metrics supply invokes set_value once per port; bindings/layout/render are deferred"},
        {"update_cpu",statistics(updates)},{"synthetic_generation_cpu",statistics(generation)},{"live_timer_lateness",statistics(lateness)},
        {"heartbeat_lateness",statistics(heartbeatLateness)},{"widget_paint_cpu",statistics(app.paintDurations)},
        {"scene_render_mean_ms",completed?measuredSceneTotalNs/double(completed)/1e6:0.},
        {"scene_render_max_ms",measuredSceneMaxNs/1e6},{"graphics_api_enum",graphicsApi},{"trace",QFileInfo(tracePath).fileName()},
        {"metric_count",mode=="metrics"?metricCount:0},{"valid",completed>0 && (mode!="surface3d" || surfaceAfter>surfaceBefore)&&focusValid&&dynamicValid}};
    result["interactive_maximized"]=interactive;result["focus_valid"]=focusValid;result["dynamic_valid"]=dynamicValid;
    result["geometry_valid"]=geometryValid;
    if(interactive){
        const auto geometryJson=[](const RendererGeometry& g){return QJsonObject{{"renderer_logical_size",dimensions(g.rendererSize)},
            {"viewport_logical_size",dimensions(g.viewportSize)},{"effective_dpr",g.dpr},
            {"graph_scene_rect",QJsonArray{g.crop.x(),g.crop.y(),g.crop.width(),g.crop.height()}}};};
        result["renderer_geometry_start"]=geometryJson(baselineGeometry);result["renderer_geometry_end"]=geometryJson(endGeometry);
        result["geometry_checks"]=double(geometryChecks);result["geometry_changed_observations"]=double(geometryChanges);
        if(invalidGeometryPanelSaved)result["invalid_geometry_owned_panel_image"]=QFileInfo(invalidGeometryPanelPath).fileName();
        result["geometry_validation_scope"]="Renderer/viewport logical sizes, effective DPR and graph scene crop are frozen after the start proof. QWidget resize and QML geometry signals latch changes; periodic and end checks also compare the baseline. Both nonempty proof images must have identical dimensions before hash changes count as dynamic evidence.";
        const auto focusJson=[](const FocusChecks& f){return QJsonObject{{"checks",double(f.count)},{"failed_checks",double(f.failures)},
            {"always_foreground",f.foreground},{"always_active",f.active},{"always_visible_not_minimized",f.visible},{"always_maximized_not_fullscreen",f.maximized},{"always_on_selected_screen",f.screen},{"first_failure_native_identity",f.firstFailureNative}};};
        result["warmup_window_checks"]=focusJson(warmupFocus);result["sample_window_checks"]=focusJson(sampleFocus);
        result["current_virtual_desktop_after_warmup"]=warmupDesktop;result["current_virtual_desktop_after_sample"]=sampleDesktop;
        result["activation_native_identity"]=activationIdentity;result["warmup_native_identity"]=warmupNativeIdentity;result["sample_native_identity"]=sampleNativeIdentity;
        result["foreground_identity_scope"]="Foreground HWND must belong to this process and equal this test's native GA_ROOT, or resolve through GA_ROOT/GA_ROOTOWNER to that exact root. Qt active state is separately required. Foreign processes and unrelated same-process windows are rejected. One setup-only input-queue attachment may assist activation; it is detached immediately and never repeated during warmup/sample.";
        result["foreground_monitor_device"]=selectedScreen.monitorDevice;result["foreground_monitor_mapped_to_qscreen"]=selectedScreen.mapped;result["screen_routing"]=selectedScreen.routing;
        result["foreground_monitor_mapping_method"]=selectedScreen.mappingMethod;
        result["screen_validation_scope"]="Observed Qt window screen pointer must match the selected screen and MonitorFromWindow(owned native GA_ROOT) must equal that screen's public QWindowsScreen HMONITOR; maximize/minimize checks use the same owned root.";
        result["foreground_monitor_native_geometry"]=rectangle(selectedScreen.nativeGeometry);result["foreground_monitor_native_available_geometry"]=rectangle(selectedScreen.nativeAvailable);
        result["dynamic_samples"]=dynamicSamples;result["source_hash_changes"]=double(sourceHashChanges);result["owned_raster_hash_changes"]=double(rasterHashChanges);
        result["source_revision_start"]=double(dynamicObservations.empty()?0:dynamicObservations.front().revision);result["source_revision_end"]=double(signal.revision);
        result["dynamic_observation_elapsed_ms"]=dynamicObservationNs/1e6;result["window_observation_elapsed_ms"]=windowObservationNs/1e6;
        result["renderer_start_image"]=QFileInfo(startFramebufferPath).fileName();result["renderer_end_image"]=QFileInfo(endFramebufferPath).fileName();
        result["renderer_start_hash"]=hashText(startFramebufferHash);result["renderer_end_hash"]=hashText(endFramebufferHash);
        result["renderer_start_pixel_size"]=dimensions(startFramebuffer.size());result["renderer_end_pixel_size"]=dimensions(endFramebuffer.size());
        result["dynamic_fixture"]=matrix?"rolling [frame][band] oldest-to-newest history; eight visibly drifting frequency ridges and changing amplitudes; oldest row storage rotated, one new row generated per supply":"eight continuously phase-varying tones";
        result["interactive_scope"]="One explicitly selected live graph maximized (not fullscreen) on the foreground monitor/current virtual desktop. Own-window activation requested once; loss of focus, visibility, maximized state or selected screen invalidates acceptance. Renderer-only graph crops exclude title/FPS; captures run outside all FPS counter intervals. Source/raster observations run every 250 ms with separate observer elapsed telemetry.";
    }
    result["renderer_identity"]=rendererIdentity.snapshot();
    const double measuredFps=mode=="surface3d"?(surfaceAfter-surfaceBefore)/seconds:
#ifdef NUMERIC_QML
        native2dMode?(vectorConsumedAfter-vectorConsumedBefore)/seconds:
#endif
        completed/seconds;
    result["target_counter_fps"]=measuredFps;
    result["target_counter_scope"]=mode=="surface3d"?"completed surface frames":
#ifdef NUMERIC_QML
        native2dMode?"unique vector/pie presentations drawn":
#endif
        completionScope;
    result["target_met"]=result.value("valid").toBool()&&live && measuredFps>=targetFps*.95 && updateRequests/seconds>=targetFps*.95
        && missedUpdatePeriods<=.01*double(updateRequests+missedUpdatePeriods);
#ifdef NUMERIC_QML
    result["indicator_fps_sample"]=panel.display_fps();
    result["indicator_scope"]="latest 500 ms Quick scene afterRendering sample; 3D uses completed surface frames; no indicator-induced repaint";
#endif
    app.paintTarget=nullptr;panel.hide();return result;
}
#ifdef NUMERIC_QML
QJsonObject runMixed(const QString& tracePath,int targetFps) {
    const qint64 periodNs=1000000000LL/targetFps;
    std::array<std::atomic<quint64>,7> sceneFrames{};
    std::array<RendererProbeTelemetry,7> rendererIdentity;
    std::atomic<bool> sceneMeasuring{false};
    QList<QMetaObject::Connection> sceneConnections;
    QWidget host;host.setAttribute(Qt::WA_ShowWithoutActivating);
    host.setWindowFlag(Qt::WindowDoesNotAcceptFocus);host.resize(1600,1000);
    auto* grid=new QGridLayout(&host);grid->setContentsMargins(0,0,0,0);
    const QStringList modes{"line","points","bars","pie","image2d","metrics","surface3d"};
    QList<GraphPanel*> panels;QList<QQuickItem*> plots;
    for(int i=0;i<modes.size();++i){
        const auto mode=modes[i];const auto kind=mode=="metrics"?"ScalarView":(mode=="image2d"||mode=="surface3d")?"SpectrogramView":"FFTView";
        auto* panel=new GraphPanel("mixed"+mode,new LineChartView,{{"kind",kind},{"display_mode",mode}},&host);
        panel->set_render_fps(targetFps);grid->addWidget(panel,i/2,i%2);panels.append(panel);
        if(mode=="surface3d"){panel->active_spectrogram()->set_history_dimensions(400,50);panel->active_spectrogram()->set_y_range(0,1.2);}
        else if(mode=="image2d")panel->image_view()->set_y_range(false,0,1.2,10);
        else if(mode=="metrics")panel->metrics_view()->set_inputs({{0,"Tone"}});
        else if(mode!="pie"){panel->active_chart()->set_auto_resizing(false);panel->active_chart()->set_range(-1,1);}
    }
    host.show();Signal signal(true);
    const auto publish=[&](){
        for(int i=0;i<panels.size();++i){auto* panel=panels[i];const auto mode=modes[i];
            if(mode=="surface3d")panel->active_spectrogram()->update_frame_history(signal.history);
            else if(mode=="image2d")panel->image_view()->set_matrix(signal.history,true);
            else if(mode=="metrics")panel->metrics_view()->set_value(0,QString::number(signal.vector[0],'g',12));
            else if(mode=="pie"){auto positive=signal.vector;for(auto& value:positive)value+=1;panel->pie_view()->set_values(positive);}
            else panel->active_chart()->update(signal.vector);
        }
    };
    quint64 supplies=0,missed=0;std::vector<double> costs,generation;
    const auto loop=[&](int duration,bool sample){QElapsedTimer clock;clock.start();qint64 next=0;
        while(clock.elapsed()<duration){const auto now=clock.nsecsElapsed();
            if(now>=next){const auto skipped=(now-next)/periodNs;if(sample)missed+=quint64(skipped);next+=(skipped+1)*periodNs;
                QElapsedTimer cost;cost.start();signal.generate(now/1e9,true);
                const double generated=cost.nsecsElapsed()/1e6;cost.restart();publish();
                if(sample){++supplies;costs.push_back(cost.nsecsElapsed()/1e6);generation.push_back(generated);}}
            QCoreApplication::processEvents(QEventLoop::AllEvents,2);QThread::msleep(1);
        }
        if(sample && next<=clock.nsecsElapsed())missed+=quint64((clock.nsecsElapsed()-next)/periodNs+1);
    };
    loop(1000,false);
    QList<quint64> starts;QList<quint64> surfaceStarts;
    for(int i=0;i<panels.size();++i){auto* renderer=rendererFor(*panels[i],modes[i]);
        ui::quick::SceneReference quick(dynamic_cast<QuickPlotWidget*>(renderer)?renderer:renderer->findChild<QQuickWidget*>());
        check(quick && quick->rootObject(),"mixed renderer QML root absent");
        sceneConnections.append(QObject::connect(quick->quickWindow(),&QQuickWindow::afterRendering,&host,[&,i]{
            rendererIdentity[i].capture();
            if(sceneMeasuring.load(std::memory_order_relaxed))sceneFrames[i].fetch_add(1,std::memory_order_relaxed);
        },Qt::DirectConnection));
        const bool native2d=modes[i]=="line" || modes[i]=="points" || modes[i]=="bars" || modes[i]=="pie";
        auto* plot=native2d?quick->rootObject()->findChild<QQuickItem*>(modes[i]=="pie"?"numericPiePlot":"numericVectorPlot"):nullptr;
        plots.append(plot);starts.append(plot?plot->property("consumedPresentationCount").toULongLong():0);
        surfaceStarts.append(modes[i]=="surface3d"?panels[i]->active_spectrogram()->axis_layout_telemetry().value("paintCount").toULongLong():0);
    }
    resetProfile();core::perf::enabled=true;
    QElapsedTimer sample;sample.start();sceneMeasuring=true;loop(4000,true);sceneMeasuring=false;
    const double seconds=sample.nsecsElapsed()/1e9;
    core::perf::enabled=false;
    const bool producerTargetMet=supplies/seconds>=targetFps*.95 && missed<=.01*double(supplies+missed);
    QJsonArray results;bool valid=true,targetMet=producerTargetMet;
    for(int i=0;i<panels.size();++i){auto* panel=panels[i];const auto unique=plots[i]?plots[i]->property("consumedPresentationCount").toULongLong()-starts[i]:0;
        const auto surface=modes[i]=="surface3d"?panel->active_spectrogram()->axis_layout_telemetry().value("paintCount").toULongLong()-surfaceStarts[i]:0;
        const double fps=panel->display_fps();valid &= fps>0;
        if(modes[i]=="line" || modes[i]=="points" || modes[i]=="bars" || modes[i]=="pie")valid &= plots[i] && unique>0;
        if(modes[i]=="surface3d")valid &= surface>0;
        const double sceneFps=sceneFrames[i].load(std::memory_order_relaxed)/seconds;
        const double targetCounter=plots[i]?unique/seconds:modes[i]=="surface3d"?surface/seconds:sceneFps;
        const bool panelTargetMet=producerTargetMet && targetCounter>=targetFps*.95;targetMet &= panelTargetMet;
        results.append(QJsonObject{{"mode",modes[i]},{"indicator_fps_sample",fps},{"unique_presentations_fps",unique/seconds},
            {"renderer_identity",rendererIdentity[i].snapshot()},
            {"scene_submissions_fps",sceneFps},
            {"surface_completed_fps",surface/seconds},{"target_counter_fps",targetCounter},{"target_met",panelTargetMet},
            {"target_counter_scope",plots[i]?"unique vector/pie presentations drawn":modes[i]=="surface3d"?"completed surface frames":"full sample scene submissions; unique data frames unmeasured"},
            {"panel_size",dimensions(panel->size())},{"dpr",panel->devicePixelRatioF()}});
    }
    for(const auto& connection:sceneConnections)QObject::disconnect(connection);
    check(save_performance_report(tracePath),"mixed stage trace");
    host.hide();return {{"mode","mixed"},{"workload",QString("live%1").arg(targetFps)},{"target_fps",targetFps},{"valid",valid},{"panels",results},{"live_supplies",double(supplies)},
        {"target_met",targetMet},
        {"missed_periods",double(missed)},{"scheduled_live_deadlines",double(supplies+missed)},
        {"frame_budget_ms",1000./targetFps},{"supplied_hz",supplies/seconds},{"update_cpu",statistics(costs)},
        {"synthetic_generation_cpu",statistics(generation)},{"sample_seconds",seconds},{"trace",QFileInfo(tracePath).fileName()},
        {"scope","seven simultaneously visible owned GraphPanels, eight synthetic tones, no DSP/dock/audio/desktop capture; indicator is latest500ms scene sample, vector/pie unique counters and surface completions separate; endpoint counters can include an in-flight warmup revision"}};
}
#endif
}

int main(int argc,char** argv){
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#ifdef NUMERIC_QML
    if(qEnvironmentVariableIsSet("GRAPH_QML_PROFILE"))QQmlDebuggingEnabler::enableDebugging(false);
    ui::quick::configure();
#endif
    PerfApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    QTimer safety;safety.setSingleShot(true);QObject::connect(&safety,&QTimer::timeout,[]{std::_Exit(3);});safety.start(150000);
    try{
        QStringList positional;bool requireTarget=false,interactive=false;
        for(int i=1;i<argc;++i){const QString argument=QString::fromLocal8Bit(argv[i]);
            if(argument=="--require-target")requireTarget=true;
            else if(argument=="--interactive-maximized")interactive=true;
            else {check(!argument.startsWith("--"),"unknown option");positional.append(argument);}
        }
        check(positional.size()>=1&&positional.size()<=4,"usage: graph_performance_compare output.json [mode|all] [static|live60|live90|live120|all] [metric-count:1|8|32] [--require-target] [--interactive-maximized]");
        const QString output=positional[0];
        check(QDir().mkpath(QFileInfo(output).absolutePath()),"cannot create output directory");
        const QString selectedMode=positional.size()>1?positional[1]:"all",selectedWorkload=positional.size()>2?positional[2]:"live120";
        const int metricCount=positional.size()>3?positional[3].toInt():8;
        check(metricCount==1||metricCount==8||metricCount==32,"metric count must be 1, 8 or 32");
        const QStringList modes{"line","points","bars","pie","image2d","metrics","surface3d"},workloads{"static","live60","live90","live120"};
        check(selectedMode=="all"||selectedMode=="mixed"||modes.contains(selectedMode),"unknown mode");
        check(selectedWorkload=="all"||workloads.contains(selectedWorkload),"unknown workload");
        check(!requireTarget || selectedWorkload.startsWith("live"),"target acceptance requires explicit live workload");
        check(!interactive||(positional.size()>=3&&modes.contains(selectedMode)&&selectedWorkload.startsWith("live")),"interactive maximized requires exactly one explicit graph mode and live workload; all/mixed/static are unsupported");
        check(!interactive||app.platformName()=="windows","interactive maximized requires the native Windows platform");
        ui::Language::instance().set("en");ui::Theme::instance().set("dark");
        QJsonArray cases;bool valid=true;
#ifdef NUMERIC_QML
        if(selectedMode=="mixed"){
            check(selectedWorkload.startsWith("live"),"mixed requires explicit live60/live90/live120 workload");
            auto result=runMixed(output+"-trace.json",targetRate(selectedWorkload));valid=result.value("valid").toBool();cases.append(result);
        }
#else
        check(selectedMode!="mixed","mixed fixture requires QML build");
#endif
        for(const auto& mode:modes)if(selectedMode=="all"||mode==selectedMode)for(const auto& workload:workloads)if(selectedWorkload=="all"||workload==selectedWorkload){
            const auto trace=QFileInfo(output).absolutePath()+"/"+QFileInfo(output).completeBaseName()+"-"+mode+"-"+workload+"-trace.json";
            const auto result=runCase(app,mode,workload,trace,metricCount,interactive);valid&=result.value("valid").toBool();cases.append(result);
        }
        auto* screen=app.primaryScreen();
        bool targetMet=true;for(const auto& value:cases)targetMet &= value.toObject().value("target_met").toBool();
        QJsonObject report{{"schema",1},{"cases",cases},{"qt_version",qVersion()},{"platform",app.platformName()},
            {"os",QSysInfo::prettyProductName()},{"architecture",QSysInfo::currentCpuArchitecture()},{"host",QSysInfo::machineHostName()},
            {"cpu_threads",int(std::thread::hardware_concurrency())},{"font",app.font().toString()},
            {"screen_logical_size",screen?dimensions(screen->size()):QJsonArray{}},{"screen_refresh_hz",screen?screen->refreshRate():0.},
            {"screen_dpr",screen?screen->devicePixelRatio():0.},{"vector_samples",256},{"matrix_frames",400},{"matrix_bands",50},{"tones",8},
            {"requested_live_hz",selectedWorkload=="all"?0:targetRate(selectedWorkload)},{"stress_target_fps",120},{"theme","dark"},{"valid",valid},
            {"target_met",targetMet},{"require_target",requireTarget},
            {"target_acceptance","explicit live workload: >=95% requested rate for measured render counter and supplies, <=1% missed producer deadlines; scene-only modes do not prove unique data/GPU/display completion"},
            {"fixture_revision",2},{"matrix_fixture","eight precomputed tone ridge bases; per-frame phase gains and multiply/add only; generation separately timed"},
            {"scope","isolated visible nonactivating GraphPanel; static forces rendering without data mutation; live publishes at case target_fps without extra redraw requests; excludes DSP, audio, docks, desktop capture and GPU/presentation latency"},
            {"cpu_scope","synchronous public data-update call elapsed time; deferred geometry/rendering reported separately in stage traces; generation measured separately"},
            {"comparison_limits","static native vectors and pie explicitly repaint; other Quick scenes can reuse textures while Widgets charts rerasterize; scene before/afterRendering excludes synchronization, bindings and paint preprocessing; metrics Widgets completion counts first value QLabel only, not full subtree; scene time and geometry stages are not an additive full render total"},
            {"instrumentation","bounded opt-in core::perf tracing enabled during sample in both builds; all GUI event deliveries timed, with top16 receiver/type durations; nested deliveries overlap and must not be summed; exports outside sample"},
            {"qml_backend_env",QString::fromLocal8Bit(qgetenv("QSG_RHI_BACKEND"))},{"qt_scale_env",QString::fromLocal8Bit(qgetenv("QT_SCALE_FACTOR"))}};
        report["interactive_maximized"]=interactive;
        if(interactive){
            report["fixture_revision"]=3;
            report["matrix_fixture"]="rolling 400x50 oldest-to-newest history: eight moving Gaussian tone ridges with varying amplitudes; one new row per supply, initialized with varied frequencies/amplitudes at target-rate spacing; generation separately timed";
            report["scope"]="single live graph, owned active/maximized window on foreground screen/current virtual desktop; 300 ms activation settle then 2 s warmup and 10 s sample; no desktop capture, audio, DSP or OS/GPU settings changes";
            report["target_acceptance"]="FPS target plus valid focus/maximized/screen/current-desktop checks in warmup and sample and changed source/owned-renderer pixels; renderer captures outside timed counters. Scene-only modes still do not prove unique presented data frames or GPU/display completion.";
        }
#ifdef NUMERIC_QML
        report["implementation"]="qml";
#else
        report["implementation"]="widgets";
#endif
        QSaveFile file(output);check(file.open(QIODevice::WriteOnly),"cannot open JSON output");
        const auto bytes=QJsonDocument(report).toJson();check(file.write(bytes)==bytes.size() && file.commit(),"cannot write JSON output");
        std::printf("Graph performance comparison: %lld cases, valid=%s, target_met=%s\n",static_cast<long long>(cases.size()),valid?"true":"false",targetMet?"true":"false");return valid && (!requireTarget || targetMet)?0:2;
    }catch(const std::exception& error){std::fprintf(stderr,"graph performance comparison FAIL: %s\n",error.what());return 1;}
}
