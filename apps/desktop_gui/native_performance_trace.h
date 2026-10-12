#pragma once
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/graph_panel.h>
#include <core_app/gui_event_profile.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <visualization/quick2d/plot_model.h>
#include <core/performance.h>
#include <pipeline/numeric_engine.h>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickWidget>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QScreen>
#include <QTimer>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QEvent>
#include <QPaintEvent>
#include <QStringList>
#include <QThread>
#include <QWindow>
#include <QMutex>
#include <QMutexLocker>
#include <atomic>
#include <array>
#include <memory>
#include <vector>
#include <cstdio>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <QtGui/qscreen_platform.h>
#include <shobjidl.h>
#endif

// Opt-in composition diagnostic. It observes the application's actual graph,
// capture device and render preferences; normal startup does not install it.
namespace native_performance_trace {
inline void milestone(const char* name,qint64 elapsedMs){std::fprintf(stderr,"CORE_APP_TRACE milestone=%s main_elapsed_ms=%lld\n",name,static_cast<long long>(elapsedMs));std::fflush(stderr);}
inline QJsonArray sizeJson(QSize size){return {size.width(),size.height()};}
inline QJsonArray rectJson(QRect rect){return {rect.x(),rect.y(),rect.width(),rect.height()};}
#ifdef Q_OS_WIN
inline HWND rootWindow(const QWidget& window){const auto h=reinterpret_cast<HWND>(window.winId());const auto root=GetAncestor(h,GA_ROOT);return root?root:h;}
inline bool foregroundMatches(const QWidget& window){
    const auto foreground=GetForegroundWindow(),root=rootWindow(window);DWORD pid=0,ownPid=0;
    if(!foreground||!root)return false;GetWindowThreadProcessId(foreground,&pid);GetWindowThreadProcessId(root,&ownPid);
    return pid==GetCurrentProcessId()&&ownPid==GetCurrentProcessId()&&
        (foreground==root||GetAncestor(foreground,GA_ROOT)==root||GetAncestor(foreground,GA_ROOTOWNER)==root);
}
inline QJsonObject describeWindow(HWND handle){
    DWORD pid=0;const DWORD thread=handle?GetWindowThreadProcessId(handle,&pid):0;RECT rect{};
    const bool hasRect=handle&&GetWindowRect(handle,&rect);wchar_t className[256]{};if(handle)GetClassNameW(handle,className,256);
    return {{"hwnd",QString::number(reinterpret_cast<quintptr>(handle),16)},{"pid",double(pid)},{"thread",double(thread)},
        {"class",QString::fromWCharArray(className)},{"is_zoomed",bool(handle&&IsZoomed(handle))},
        {"is_iconic",bool(handle&&IsIconic(handle))},{"is_visible",bool(handle&&IsWindowVisible(handle))},
        {"monitor",QString::number(reinterpret_cast<quintptr>(handle?MonitorFromWindow(handle,MONITOR_DEFAULTTONEAREST):nullptr),16)},
        {"window_rect_win32",hasRect?QJsonArray{int(rect.left),int(rect.top),int(rect.right-rect.left),int(rect.bottom-rect.top)}:QJsonArray{}},
        {"window_rect_scope","GetWindowRect coordinates in the caller's DPI context; the pre-QApplication snapshot can be DPI virtualized."}};
}
#endif
inline QJsonObject foregroundSnapshot(){
#ifdef Q_OS_WIN
    const auto foreground=GetForegroundWindow();auto result=describeWindow(foreground);
    result["root"]=describeWindow(foreground?GetAncestor(foreground,GA_ROOT):nullptr);
    result["root_owner"]=describeWindow(foreground?GetAncestor(foreground,GA_ROOTOWNER):nullptr);
    result["current_pid"]=double(GetCurrentProcessId());result["current_thread"]=double(GetCurrentThreadId());return result;
#else
    return {};
#endif
}
class Trace final:public QObject {
    struct PaintSummary {
        QPointer<QWidget> widget;QString className,objectName,ancestors,address;
        bool quickTexture=false;QRect widgetRect,lastRegionBounds;qreal dpr=0;
        quint64 count=0,regionArea=0,maximumRegionArea=0,regionRects=0,truncatedRegions=0,firstNs=0,lastNs=0;
        quint64 layoutRequests=0,updateRequests=0,metaCalls=0;
        double deviceArea=0;
    };
    static constexpr size_t paintCapacity=512,paintRegionRectLimit=64;
    std::array<PaintSummary,paintCapacity> paints_{};size_t paintCount_=0;
    quint64 paintOverflow_=0,paintObserverNs_=0,ordinaryPaints_=0,texturePaints_=0;
    struct Graph {
        QPointer<GraphPanel> panel;ui::quick::SceneReference quick;QPointer<QQuickItem> vector;
        QString mode;QSize panelSize,rendererSize;QRectF crop; qreal dpr=0;
        quint64 surfaceBefore=0,vectorBefore=0,consumedBefore=0;
        quint64 surfaceAfter=0,paintAfter=0,consumedAfter=0,sceneAfter=0;
        std::atomic<quint64> scene{0};QJsonObject gpu;QMutex gpuMutex;bool geometryStable=true;
        std::vector<QMetaObject::Connection> connections;
    };
    CoreApplication& app_;QString path_;QPointer<MainWindow> window_;QPointer<QScreen> screen_;
    std::vector<std::unique_ptr<Graph>> graphs_;std::atomic<bool> measuring_{false};
    bool observing_=false,focusValid_=true,geometryValid_=true,mapped_=false;
    bool finished_=false;quint64 mainEntryNs_=0,warmupStartNs_=0,actualWarmupNs_=0;int warmupMs_=1000;
    quint64 focusChecks_=0,start_=0,end_=0,frames_=0,history_=0,snapshots_=0,ordinal_=0;
    QSize windowSize_;QJsonObject activation_,screenJson_,entryForeground_,sampleStartWindow_;QTimer watch_;
#ifdef Q_OS_WIN
    HMONITOR monitor_=nullptr;IVirtualDesktopManager* desktop_=nullptr;bool uninitialize_=false;
#endif
    qint64 mainElapsedMs() const{return qint64((core::perf::now()-mainEntryNs_)/1000000);}
    int remainingMs(int deadline) const{return int(std::max<qint64>(0,deadline-mainElapsedMs()));}
#ifdef Q_OS_WIN
    QJsonObject desktopIdentity(HWND handle) const{
        GUID id{};const HRESULT status=desktop_&&handle?desktop_->GetWindowDesktopId(handle,&id):E_NOINTERFACE;
        wchar_t text[40]{};if(SUCCEEDED(status))StringFromGUID2(id,text,40);
        return {{"query_status",QString::number(quint32(status),16)},{"guid",QString::fromWCharArray(text)}};
    }
#endif
    QJsonObject windowSnapshot() const{
        QJsonObject result{{"foreground",foregroundSnapshot()}};if(!window_){result["own_window_missing"]=true;return result;}
        result["logical_size"]=sizeJson(window_->size());result["logical_geometry"]=rectJson(window_->geometry());
        result["qt_active"]=window_->isActiveWindow();result["qt_maximized"]=window_->isMaximized();result["qt_fullscreen"]=window_->isFullScreen();
        result["qt_visible"]=window_->isVisible();result["qt_dpr"]=window_->devicePixelRatioF();
        result["qt_screen"]=window_->windowHandle()&&window_->windowHandle()->screen()?window_->windowHandle()->screen()->name():QString{};
#ifdef Q_OS_WIN
        const auto root=rootWindow(*window_);result["own_root"]=describeWindow(root);result["owned_foreground"]=foregroundMatches(*window_);
        result["own_virtual_desktop"]=desktopIdentity(root);result["foreground_virtual_desktop"]=desktopIdentity(GetForegroundWindow());
        BOOL current=FALSE;const HRESULT desktopStatus=desktop_?desktop_->IsWindowOnCurrentVirtualDesktop(root,&current):E_NOINTERFACE;
        result["current_virtual_desktop_status"]=QString::number(quint32(desktopStatus),16);result["on_current_virtual_desktop"]=SUCCEEDED(desktopStatus)&&current;
        result["selected_native_monitor_match"]=MonitorFromWindow(root,MONITOR_DEFAULTTONEAREST)==monitor_;
#endif
        return result;
    }
    static QQuickItem* graphArea(const Graph& graph){
        if(!graph.quick||!graph.quick->rootObject())return nullptr;
        const auto name=graph.mode=="surface3d"?"spectrogramContainer":graph.mode=="image2d"?"numericRaster":graph.mode=="pie"?"numericPiePlot":"numericVectorPlot";
        auto* item=graph.quick->rootObject()->findChild<QQuickItem*>(name);return item?item:graph.quick->rootObject();
    }
    static quint64 surfaceCount(const Graph& graph){auto* surface=graph.panel&&graph.mode=="surface3d"?graph.panel->active_spectrogram():nullptr;
        return surface?surface->axis_layout_telemetry().value("paintCount").toULongLong():0;}
    void recordWidgetEvent(QWidget* widget,QEvent* event){
        const auto started=core::perf::now();const bool texture=qobject_cast<QQuickWidget*>(widget)!=nullptr;
        const bool painting=event->type()==QEvent::Paint;
        if(painting){if(texture)++texturePaints_;else ++ordinaryPaints_;}
        size_t index=0;while(index<paintCount_&&paints_[index].widget!=widget)++index;
        if(index==paintCount_){
            if(index==paintCapacity){++paintOverflow_;paintObserverNs_+=core::perf::now()-started;return;}
            auto& summary=paints_[paintCount_++];summary.widget=widget;summary.quickTexture=texture;
            summary.address=QString::number(reinterpret_cast<quintptr>(widget),16);
            summary.className=QString::fromLatin1(widget->metaObject()->className());summary.objectName=widget->objectName().left(128);
            QStringList ancestors;
            for(auto* parent=widget->parentWidget();parent&&ancestors.size()<6;parent=parent->parentWidget())
                ancestors.append(QString::fromLatin1(parent->metaObject()->className())+"/"+parent->objectName().left(96));
            summary.ancestors=ancestors.join(" <- ");
        }
        auto& summary=paints_[index];
        if(!painting){
            summary.layoutRequests+=event->type()==QEvent::LayoutRequest;summary.updateRequests+=event->type()==QEvent::UpdateRequest;
            summary.metaCalls+=event->type()==QEvent::MetaCall;paintObserverNs_+=core::perf::now()-started;return;
        }
        const auto& paint=*static_cast<QPaintEvent*>(event);quint64 area=0;size_t visited=0;
        for(const QRect& rect:paint.region()){
            if(visited==paintRegionRectLimit)break;++visited;
            area+=quint64(std::max(0,rect.width()))*quint64(std::max(0,rect.height()));
        }
        if(!summary.count)summary.firstNs=started;
        ++summary.count;summary.lastNs=started;summary.widgetRect=widget->rect();summary.dpr=widget->devicePixelRatioF();
        summary.lastRegionBounds=paint.region().boundingRect();summary.regionRects+=quint64(paint.region().rectCount());
        summary.truncatedRegions+=paint.region().rectCount()>int(paintRegionRectLimit);
        summary.regionArea+=area;summary.maximumRegionArea=std::max(summary.maximumRegionArea,area);
        summary.deviceArea+=double(area)*summary.dpr*summary.dpr;
        paintObserverNs_+=core::perf::now()-started;
    }
    void observe(bool endpointGeometry=false){
        if(!observing_)return;if(!window_){focusValid_=false;geometryValid_=false;return;}++focusChecks_;
        bool valid=window_->isActiveWindow()&&window_->isVisible()&&!window_->isMinimized()&&window_->isMaximized()&&!window_->isFullScreen()
            &&window_->windowHandle()&&window_->windowHandle()->screen()==screen_;
#ifdef Q_OS_WIN
        const auto root=rootWindow(*window_);BOOL current=FALSE;
        valid=valid&&foregroundMatches(*window_)&&IsZoomed(root)&&!IsIconic(root)&&MonitorFromWindow(root,MONITOR_DEFAULTTONEAREST)==monitor_
            &&desktop_&&SUCCEEDED(desktop_->IsWindowOnCurrentVirtualDesktop(root,&current))&&current;
#else
        valid=false;
#endif
        focusValid_&=valid;
        if(measuring_||endpointGeometry){
            geometryValid_&=window_->size()==windowSize_;
            for(auto& graph:graphs_){
                auto* area=graphArea(*graph);
                graph->geometryStable&=graph->panel&&graph->panel->isVisible()&&graph->panel->display_mode()==graph->mode
                    &&graph->panel->size()==graph->panelSize&&graph->quick&&graph->quick->isVisible()&&graph->quick->size()==graph->rendererSize
                    &&graph->quick->quickWindow()->effectiveDevicePixelRatio()==graph->dpr&&area&&area->mapRectToScene(area->boundingRect())==graph->crop;
            }
        }
    }
    bool eventFilter(QObject* object,QEvent* event) override {
        if(QThread::currentThread()!=app_.thread())return false;
        if(measuring_.load()&&(event->type()==QEvent::Paint||event->type()==QEvent::LayoutRequest||event->type()==QEvent::UpdateRequest||event->type()==QEvent::MetaCall))
            if(auto* widget=qobject_cast<QWidget*>(object);widget&&(event->type()==QEvent::Paint||widget->isVisible()))recordWidgetEvent(widget,event);
        if(observing_&&object==window_&&(event->type()==QEvent::Hide||event->type()==QEvent::WindowDeactivate))focusValid_=false;
        if(observing_&&object==window_&&event->type()==QEvent::WindowStateChange&&(!window_->isMaximized()||window_->isFullScreen()))focusValid_=false;
        if(measuring_&&event->type()==QEvent::Resize){
            if(object==window_)geometryValid_=false;
            for(auto& graph:graphs_)if(object==graph->panel||object==graph->quick.owner())graph->geometryStable=false;
        }
        return false;
    }
    void collectGraphs(){
        for(auto* panel:window_->findChildren<GraphPanel*>()){
            if(!panel->isVisible())continue;
            auto graph=std::make_unique<Graph>();graph->panel=panel;graph->mode=panel->display_mode();
            QList<QWidget*> hosts;
            for(auto* host:panel->findChildren<QQuickWidget*>())if(host->objectName()!="plotTextureHost")hosts.append(host);
            for(auto* host:panel->findChildren<QuickPlotWidget*>())hosts.append(host);
            for(auto* host:hosts)if(host->isVisible()&&host->objectName()!="graphSettingsQuick"){
                ui::quick::SceneReference quick(host);
                // Settings roots are excluded by their graph-area identity.
                if(!quick->rootObject())continue;
                if(quick->rootObject()->findChild<QQuickItem*>("numericRaster")||quick->rootObject()->findChild<QQuickItem*>("numericVectorPlot")
                   ||quick->rootObject()->findChild<QQuickItem*>("numericPiePlot")||quick->rootObject()->findChild<QQuickItem*>("spectrogramContainer")){graph->quick=quick;break;}
            }
            if(graph->quick){
                graph->vector=graph->quick->rootObject()->findChild<QQuickItem*>(graph->mode=="pie"?"numericPiePlot":"numericVectorPlot");
                auto* raw=graph.get();
                graph->connections.push_back(connect(graph->quick->quickWindow(),&QQuickWindow::afterRendering,this,[this,raw]{
                    QMutexLocker lock(&raw->gpuMutex);
                    if(raw->gpu.isEmpty())if(auto* context=QOpenGLContext::currentContext()){
                        const auto text=[&](GLenum key){const auto* value=context->functions()->glGetString(key);return value?QString::fromLatin1(reinterpret_cast<const char*>(value)):QString{};};
                        raw->gpu={{"vendor",text(GL_VENDOR)},{"renderer",text(GL_RENDERER)},{"version",text(GL_VERSION)},
                            {"samples",context->format().samples()},{"swap_interval",context->format().swapInterval()},
                            {"render_thread_is_gui",QThread::currentThread()==app_.thread()}};
                    }
                    if(measuring_.load())++raw->scene;
                },Qt::DirectConnection));
            }
            graphs_.push_back(std::move(graph));
        }
    }
    void begin(){
        if(finished_)return;
        milestone("sample_begin",mainElapsedMs());
        if(warmupStartNs_)actualWarmupNs_=core::perf::now()-warmupStartNs_;
        if(!window_){focusValid_=false;finish();return;}
        collectGraphs();windowSize_=window_->size();
        sampleStartWindow_=windowSnapshot();
        for(auto& graph:graphs_){
            graph->panelSize=graph->panel->size();graph->surfaceBefore=surfaceCount(*graph);
            if(graph->quick){
                graph->rendererSize=graph->quick->size();graph->dpr=graph->quick->quickWindow()->effectiveDevicePixelRatio();
                if(auto* area=graphArea(*graph)){graph->crop=area->mapRectToScene(area->boundingRect());
                    for(auto* item=area;item;item=item->parentItem())for(auto signal:{&QQuickItem::widthChanged,&QQuickItem::heightChanged,&QQuickItem::xChanged,&QQuickItem::yChanged})
                        graph->connections.push_back(connect(item,signal,this,[this]{observe();}));
                }
            }
            if(graph->vector){graph->vectorBefore=graph->vector->property("paintCount").toULongLong();graph->consumedBefore=graph->vector->property("consumedPresentationCount").toULongLong();}
        }
        using namespace core::perf;enabled=false;
        for(auto& counter:counters){counter.count=0;counter.total=0;counter.maximum=0;for(auto& bin:counter.histogram)bin=0;}
        for(auto& event:events)event.ready=false;event_count=0;slow_gui_event_count=0;
        gui_event_profile::groups={};gui_event_profile::group_count=0;gui_event_profile::overflow=0;
        frames_=history_=snapshots_=ordinal_=0;start_=now();measuring_=true;gui_event_profile::enabled=true;enabled=true;
        observe();
    }
    void finish(){
        if(finished_)return;finished_=true;milestone("export_begin",mainElapsedMs());
        end_=core::perf::now();measuring_=false;core::perf::enabled=false;gui_event_profile::enabled=false;
        // Freeze every renderer endpoint before COM observers can pump messages.
        for(auto& graph:graphs_){
            graph->surfaceAfter=surfaceCount(*graph);
            graph->paintAfter=graph->vector?graph->vector->property("paintCount").toULongLong():0;
            graph->consumedAfter=graph->vector?graph->vector->property("consumedPresentationCount").toULongLong():0;
            graph->sceneAfter=graph->scene.load();
        }
        observe(true);watch_.stop();observing_=false;
        const bool sampleStarted=start_!=0;
        if(!sampleStarted){start_=end_;focusValid_=false;geometryValid_=false;if(warmupStartNs_)actualWarmupNs_=end_-warmupStartNs_;}
        const double elapsed=(end_-start_)/1e9;
        const auto sampleEndWindow=windowSnapshot();
        QJsonArray graphJson;
        // Freeze renderer counters before JSON export; no framebuffer captures
        // are made during measurement or included as completed graph frames.
        for(auto& graph:graphs_){
            if(!graph->panel){geometryValid_=false;for(const auto& connection:graph->connections)disconnect(connection);
                graphJson.append(QJsonObject{{"mode",graph->mode},{"destroyed_during_sample",true},{"geometry_stable",false}});continue;}
            const auto surfaceAfter=graph->surfaceAfter,paintAfter=graph->paintAfter,consumedAfter=graph->consumedAfter;
            const bool counterValid=graph->panel->display_mode()==graph->mode&&surfaceAfter>=graph->surfaceBefore
                &&paintAfter>=graph->vectorBefore&&consumedAfter>=graph->consumedBefore
                &&(graph->mode!="surface3d"||graph->panel->active_spectrogram());
            const auto surface=counterValid?surfaceAfter-graph->surfaceBefore:0;
            const auto paints=counterValid?paintAfter-graph->vectorBefore:0;
            const auto consumed=counterValid?consumedAfter-graph->consumedBefore:0;
            for(const auto& connection:graph->connections)disconnect(connection);
            auto* dock=graph->panel->parentWidget();while(dock&&!qobject_cast<WorkspaceDock*>(dock))dock=dock->parentWidget();
            graphJson.append(QJsonObject{{"object_name",graph->panel->objectName()},{"mode",graph->mode},{"preferences",QJsonObject::fromVariantMap(graph->panel->preferences())},
                {"panel_size",sizeJson(graph->panelSize)},{"panel_size_end",sizeJson(graph->panel->size())},
                {"renderer_size",sizeJson(graph->rendererSize)},{"renderer_size_end",graph->quick?sizeJson(graph->quick->size()):QJsonArray{}},{"effective_dpr",graph->dpr},
                {"effective_dpr_end",graph->quick?graph->quick->quickWindow()->effectiveDevicePixelRatio():0.},
                {"geometry_stable",graph->geometryStable},{"counters_valid",counterValid},{"target_fps",graph->panel->render_fps()},{"visible",graph->panel->isVisible()},
                {"scene_submissions",double(graph->sceneAfter)},{"scene_submissions_per_second",graph->sceneAfter/elapsed},
                {"surface_frames",double(surface)},{"surface_fps",surface/elapsed},{"vector_paints",double(paints)},
                {"vector_unique_revisions_drawn",double(consumed)},{"vector_unique_fps",consumed/elapsed},{"gpu",[&]{QMutexLocker lock(&graph->gpuMutex);return graph->gpu;}()},
                {"dock_fps_label",dock?qobject_cast<WorkspaceDock*>(dock)->renderFPS():QString{}}});
            geometryValid_&=graph->geometryStable&&counterValid;
        }
        QJsonArray statistics,trace,groups,ordinaryPaintSummary,quickTexturePaintSummary;
        for(size_t i=0;i<paintCount_;++i){const auto& paint=paints_[i];
            const QJsonObject summary{{"widget_address",paint.address},{"class",paint.className},{"object_name",paint.objectName},{"ancestors",paint.ancestors},
                {"alive_at_export",!paint.widget.isNull()},{"paint_events",double(paint.count)},{"widget_rect_at_last_paint",rectJson(paint.widgetRect)},
                {"visible_layout_request_events",double(paint.layoutRequests)},{"visible_update_request_events",double(paint.updateRequests)},{"visible_meta_call_events",double(paint.metaCalls)},
                {"last_paint_region_bounds",rectJson(paint.lastRegionBounds)},{"dpr_at_last_paint",paint.dpr},
                {"summed_region_area_logical_pixels",double(paint.regionArea)},{"summed_region_area_device_pixels",paint.deviceArea},
                {"maximum_region_area_logical_pixels",double(paint.maximumRegionArea)},{"region_rectangle_count",double(paint.regionRects)},
                {"regions_exceeding_rectangle_limit",double(paint.truncatedRegions)},
                {"first_paint_since_sample_ms",paint.count?(paint.firstNs-start_)/1e6:0.},{"last_paint_since_sample_ms",paint.count?(paint.lastNs-start_)/1e6:0.}};
            if(paint.quickTexture)quickTexturePaintSummary.append(summary);else ordinaryPaintSummary.append(summary);
        }
        using namespace core::perf;
        for(size_t i=0;i<counters.size();++i){const auto& c=counters[i];const auto count=c.count.load();if(!count)continue;
            statistics.append(QJsonObject{{"stage",names[i]},{"count",double(count)},{"mean_ms",c.total.load()/double(count)/1e6},
                {"max_ms",c.maximum.load()/1e6},{"p50_upper_ms",percentile_upper_ns(c,50)/1e6},{"p95_upper_ms",percentile_upper_ns(c,95)/1e6},{"p99_upper_ms",percentile_upper_ns(c,99)/1e6}});
        }
        const auto count=event_count.load();for(size_t i=0;i<std::min<uint64_t>(count,event_capacity);++i){const auto& event=events[i];if(!event.ready.load(std::memory_order_acquire))continue;
            trace.append(QJsonObject{{"name",names[size_t(event.stage)]},{"ph","X"},{"pid",1},{"tid",double(event.thread)},{"ts",event.start/1000.},{"dur",event.duration/1000.}});
        }
        for(size_t i=0;i<gui_event_profile::group_count;++i){const auto& g=gui_event_profile::groups[i];groups.append(QJsonObject{{"receiver",g.receiver.data()},{"event_type",g.type},
            {"count",double(g.count)},{"inclusive_ms",g.inclusive/1e6},{"exclusive_ms",g.exclusive/1e6},{"inclusive_cpu_cycles",double(g.inclusive_cycles)},{"exclusive_cpu_cycles",double(g.exclusive_cycles)},{"maximum_ms",g.maximum/1e6}});}
        auto* status=window_?window_->findChild<QLabel*>("configurationSaveStatus"):nullptr;
        int previewInterval=-1;
        if(!app_.presenter().is_remote()){
            auto* engine=&app_.pipeline();
            QMetaObject::invokeMethod(engine,[&]{previewInterval=engine->numeric_preview_interval_ms();},Qt::BlockingQueuedConnection);
        }
        QJsonObject report{{"preview_interval_ms",previewInterval},{"engine",app_.presenter().client().snapshot().diagnostics},{"kind","actual_core_app_opt_in_trace"},{"sample_started",sampleStarted},{"sample_seconds",elapsed},{"warmup_seconds",actualWarmupNs_/1e9},{"requested_warmup_seconds",warmupMs_/1000.},{"main_elapsed_ms_at_export",double(mainElapsedMs())},{"total_lifetime_limit_seconds",30},{"sample_start_ns",double(start_)},{"sample_end_ns",double(end_)},
            {"short_sample_diagnostic",actualWarmupNs_==0||elapsed<3.},{"fps_acceptance_scope","Diagnostic trace only; startup-limited short samples and unsettled focus/layout do not establish stable FPS."},
            {"focus_valid",focusValid_&&mapped_&&focusChecks_>0},{"focus_checks",double(focusChecks_)},{"geometry_valid",geometryValid_},
            {"window_size",sizeJson(windowSize_)},{"screen",screenJson_},{"activation",activation_},{"graphs",graphJson},
            {"foreground_at_main_entry",entryForeground_},{"window_at_sample_start",sampleStartWindow_},{"window_at_sample_end",sampleEndWindow},
            {"numeric_frames_delivered",double(frames_)},{"history_rows_delivered",double(history_)},{"history_snapshots_delivered",double(snapshots_)},{"latest_history_ordinal",double(ordinal_)},
            {"apply_status",app_.presenter().apply_status()},{"graph_request_pending",app_.presenter().graph_request_pending()},
            {"save_status_text",status?status->text():QString{}},{"save_status_tooltip",status?status->toolTip():QString{}},
            {"statistics",statistics},{"traceEvents",trace},{"gui_event_groups",groups},{"gui_event_groups_overflow",double(gui_event_profile::overflow)},
            {"ordinary_widget_paints",ordinaryPaintSummary},{"quick_texture_widget_paints",quickTexturePaintSummary},
            {"ordinary_widget_paint_events",double(ordinaryPaints_)},{"quick_texture_widget_paint_events",double(texturePaints_)},
            {"widget_paint_summary_capacity",double(paintCapacity)},{"widget_paint_summary_overflow_events",double(paintOverflow_)},
            {"widget_paint_observer_elapsed_ms",paintObserverNs_/1e6},
            {"widget_paint_scope","GUI-thread delivered Paint events for this application's QWidget objects; QQuickWidget events are categorized separately as texture widgets. Visible QWidget LayoutRequest/UpdateRequest/MetaCall events are counted per identity too. Regions are logical widget coordinates, cumulative areas count repeated pixels across events, and up to64 rectangles per event are summed (truncation reported). These are event deliveries, not original update() callers or unique backing-store dirty area. At most512 widget identities, six ancestors and bounded names are retained; all summaries are exported after measurement."},
            {"dropped_events",double(count>event_capacity?count-event_capacity:0)},
            {"scope","Actual application capture/pipeline/workspace/preferences. Scene submissions may reuse graph textures; surface and vector revision counts are separate. Interval-stage first observations may carry timestamps from warmup. No target-FPS acceptance is inferred from dock text."}};
        QSaveFile file(path_);bool saved=false;if(file.open(QIODevice::WriteOnly)){const auto bytes=QJsonDocument(report).toJson();saved=file.write(bytes)==bytes.size()&&file.commit();}
        milestone("export_complete",mainElapsedMs());qInfo("Actual application trace: %s saved=%d",qPrintable(path_),saved);app_.exit(saved&&sampleStarted?0:2);
    }
public:
    Trace(CoreApplication& app,QString path,qint64 elapsedMs,QJsonObject entryForeground):QObject(&app),app_(app),path_(std::move(path)),mainEntryNs_(core::perf::now()-quint64(std::max<qint64>(0,elapsedMs))*1000000),entryForeground_(std::move(entryForeground)){
        app_.installEventFilter(this);
        milestone("trace_install",mainElapsedMs());
        if(remainingMs(28000)<1500){std::fprintf(stderr,"CORE_APP_TRACE warning=insufficient_remaining_sample_budget remaining_ms=%d\n",remainingMs(28000));std::fflush(stderr);}
#ifdef Q_OS_WIN
        const auto foreground=GetForegroundWindow();monitor_=foreground?MonitorFromWindow(foreground,MONITOR_DEFAULTTONEAREST):nullptr;
        for(auto* screen:app.screens()){const auto* native=screen->nativeInterface<QNativeInterface::QWindowsScreen>();if(native&&native->handle()==monitor_){screen_=screen;mapped_=true;break;}}
        const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);uninitialize_=SUCCEEDED(initialized);
        const CLSID clsid{0xaa509086,0x5ca9,0x4c25,{0x8f,0x95,0x58,0x9d,0x3c,0x07,0xb4,0x8a}};
        const IID iid{0xa5cd92ff,0x29be,0x454c,{0x8d,0x04,0xd8,0x28,0x79,0xfb,0x3f,0x1b}};
        CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,iid,reinterpret_cast<void**>(&desktop_));
        bool entryHandleOk=false;const auto entryHandle=entryForeground_.value("hwnd").toString().toULongLong(&entryHandleOk,16);
        if(entryHandleOk){entryForeground_["virtual_desktop_queried_after_constructor"]=desktopIdentity(reinterpret_cast<HWND>(quintptr(entryHandle)));}
#endif
        connect(&app.presenter(),&GuiPresenter::numeric_frame_ready,this,[this](const auto&){if(measuring_)++frames_;});
        connect(&app.presenter(),&GuiPresenter::numeric_history_ready,this,[this](const auto& row){if(measuring_){++history_;ordinal_=std::max(ordinal_,quint64(row.accepted_row_ordinal));}});
        connect(&app.presenter(),&GuiPresenter::numeric_history_snapshot_ready,this,[this](const auto&,quint64 ordinal,quint64){if(measuring_){++snapshots_;ordinal_=std::max(ordinal_,ordinal);}});
        QTimer::singleShot(0,this,[this]{
            milestone("setup_begin",mainElapsedMs());
            for(auto* widget:app_.topLevelWidgets())if(auto* main=qobject_cast<MainWindow*>(widget)){window_=main;break;}
            const bool passive=qEnvironmentVariableIntValue("CORE_APP_TRACE_PASSIVE")!=0;
            if(passive&&window_){screen_=window_->screen();mapped_=bool(screen_);}
            if(!window_||!mapped_){qCritical("Trace requires mapped foreground monitor and main window");finish();return;}
            activation_["passive_observer"]=passive;
            if(!passive){
            window_->winId();window_->windowHandle()->setScreen(screen_);window_->move(screen_->availableGeometry().topLeft()+QPoint(20,20));
            window_->showMaximized();window_->raise();window_->activateWindow();
#ifdef Q_OS_WIN
            activation_["before"]=windowSnapshot();SetLastError(ERROR_SUCCESS);
            const bool activated=bool(SetForegroundWindow(rootWindow(*window_)));const DWORD activationError=GetLastError();
            activation_["set_foreground_return"]=activated;activation_["set_foreground_last_error"]=double(activationError);
            if(!foregroundMatches(*window_)){
                const auto target=rootWindow(*window_),foreground=GetForegroundWindow();DWORD ownPid=0;
                GetWindowThreadProcessId(target,&ownPid);
                const DWORD ownThread=GetCurrentThreadId(),foregroundThread=foreground?GetWindowThreadProcessId(foreground,nullptr):0;
                if(ownPid==GetCurrentProcessId()&&foregroundThread&&foregroundThread!=ownThread){
                    activation_["own_thread"]=double(ownThread);activation_["foreground_thread"]=double(foregroundThread);
                    SetLastError(ERROR_SUCCESS);const bool attached=bool(AttachThreadInput(ownThread,foregroundThread,TRUE));const DWORD attachError=GetLastError();
                    activation_["input_queues_attached"]=attached;activation_["attach_last_error"]=double(attachError);
                    if(attached){struct Detach{DWORD own,other;~Detach(){AttachThreadInput(own,other,FALSE);}} detach{ownThread,foregroundThread};
                        BringWindowToTop(target);SetActiveWindow(target);SetLastError(ERROR_SUCCESS);
                        const bool attachedActivated=bool(SetForegroundWindow(target));const DWORD attachedError=GetLastError();
                        activation_["attached_set_foreground_return"]=attachedActivated;activation_["attached_set_foreground_last_error"]=double(attachedError);
                    }
                }
            }
            activation_["owned_foreground_after_setup"]=foregroundMatches(*window_);
            activation_["after"]=windowSnapshot();
            activation_["last_error_scope"]="Captured immediately after each call; SetForegroundWindow does not promise a meaningful GetLastError diagnostic.";
#endif
            }
            screenJson_={{"name",screen_->name()},{"geometry",rectJson(screen_->geometry())},{"available_geometry",rectJson(screen_->availableGeometry())}};
            watch_.setInterval(100);watch_.setTimerType(Qt::PreciseTimer);connect(&watch_,&QTimer::timeout,this,[this]{observe();});
            milestone("setup_complete",mainElapsedMs());
            if(remainingMs(28000)<3000){
                // Slow real project startup must leave a measured diagnostic
                // interval instead of spending its remaining lifetime warming up.
                // Activation is requested once above; failed/unsettled focus is
                // still recorded and never reacquired during this short sample.
                observing_=true;warmupMs_=0;warmupStartNs_=0;actualWarmupNs_=0;
                observe();watch_.start();begin();return;
            }
            QTimer::singleShot(std::min(300,remainingMs(27800)),this,[this]{
                if(finished_)return;observing_=true;observe();watch_.start();
                warmupMs_=std::clamp(remainingMs(28000)-1000,0,1000);
                warmupStartNs_=core::perf::now();
                QTimer::singleShot(warmupMs_,Qt::PreciseTimer,this,[this]{begin();});
            });
        });
        // Budget includes CoreApplication construction since main entry. DLL
        // startup and blocked GUI dispatch remain covered by the launcher's
        // independent thirty-second process watchdog.
        QTimer::singleShot(remainingMs(28000),Qt::PreciseTimer,this,[this]{finish();});
        QTimer::singleShot(remainingMs(28500),Qt::PreciseTimer,this,[this]{app_.exit(2);});
    }
    ~Trace() override {
        app_.removeEventFilter(this);
        core::perf::enabled=false;gui_event_profile::enabled=false;
        for(auto& graph:graphs_)for(const auto& connection:graph->connections)disconnect(connection);
#ifdef Q_OS_WIN
        if(desktop_)desktop_->Release();if(uninitialize_)CoUninitialize();
#endif
    }
};
inline void install(CoreApplication& app,qint64 startupElapsedMs=0,QJsonObject entryForeground={}){
    const auto path=qEnvironmentVariable("CORE_APP_TRACE_FILE");if(path.isEmpty())return;
    new Trace(app,path,startupElapsedMs,std::move(entryForeground));
}
}
