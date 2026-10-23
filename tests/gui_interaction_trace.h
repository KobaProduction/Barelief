#pragma once
#include <barelief/quick2d/plot_model.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <core_app/graph_panel.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QAction>
#include <QDoubleSpinBox>
#include <QGraphicsView>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <QElapsedTimer>
#include <barelief/quick/scene_host.h>
#include <QQuickView>
#include <array>
#include <atomic>
#include <memory>
#include <cstdio>
#include <vector>
#include <cmath>
#include <cstring>
#include <core/telemetry.h>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

// Sends events only to widgets owned by this diagnostic process. It never
// moves the system pointer, activates windows or changes the source project.
class GuiInteractionTrace : public QObject {
    QPointer<QWidget> window_;
    QPointer<QGraphicsView> view_;
    QPointer<SchemeEditorWidget> editor_;
    QGraphicsItem* node_=nullptr;
    QTimer timer_;
    QElapsedTimer clock_;
    QJsonArray samples_;
    QJsonArray viewport_samples_;
    qint64 previous_=0;
    int tick_=0;
    bool held_=false;
    QPointF pointer_{120,120};
    QPointer<SpectrogramGraph> surface_;
    bool flight_=qEnvironmentVariable("BARELIEF_TRACE_GESTURES")=="flight";
    int flight_phase_=-1;
    qint64 flight_duration_=100000;
    QList<QPointer<SpectrogramGraph>> flight_surfaces_;
    QList<QJsonObject> camera_bases_;
    double camera_offset_=0;
    QPointF pipeline_origin_;
    struct SceneFrames {
        QPointer<ui::quick::SceneHost> host;
        std::array<std::atomic<quint64>,7> frames{};
        QMetaObject::Connection connection;
    };
    std::vector<std::shared_ptr<SceneFrames>> scene_frames_;
    static constexpr std::array<qint64,8> phase_edges_{0,10000,30000,40000,60000,70000,90000,100000};
    static constexpr std::array<const char*,7> flight_names_{
        "idle_before","camera_flight","idle_after_camera","pipeline_pan","idle_after_pipeline","combined","idle_final"};
    static int flightPhase(qint64 ms,qint64 duration) {
        ms=ms*100000/duration;
        for(int i=0;i<7;++i)if(ms<phase_edges_[i+1])return i;
        return 6;
    }
    static double cameraDistance(const QJsonObject& state) {
        const auto position=state.value("position").toArray();
        return std::sqrt(std::pow(position[0].toDouble(),2)+std::pow(position[1].toDouble(),2)+std::pow(position[2].toDouble(),2));
    }
    bool eventFilter(QObject* receiver,QEvent* event) override {
        // Native pointer movement must not release the replay's synthetic drag.
        // This filter affects only this owned diagnostic viewport, for the replay.
        const auto type=event->type();
        if(view_ && receiver==view_->viewport() && event->spontaneous() &&
           (type==QEvent::MouseMove || type==QEvent::MouseButtonPress || type==QEvent::MouseButtonRelease || type==QEvent::Wheel))return true;
        return QObject::eventFilter(receiver,event);
    }
    static int densityScore(const QList<QGraphicsItem*>& items,const QRectF& area) {
        int score=0;
        for(auto* item:items)if(item->isVisible() && area.intersects(item->sceneBoundingRect()))
            score+=item->type()==QtNodes::NodeGraphicsObject::Type?8:item->type()==QtNodes::ConnectionGraphicsObject::Type?1:0;
        return score;
    }
    void centerDensePipeline() {
        if(auto* editor=editor_.data()) {
            view_=editor->findChild<QGraphicsView*>();
            // The workspace reparents the editor toolbar into its dock chrome.
            if(auto* zoom=window_->findChild<QDoubleSpinBox*>("pipelineZoom"))zoom->setValue(25);
        }
        if(!view_ || !view_->scene())return;
        const auto items=view_->scene()->items();const auto size=view_->mapToScene(view_->viewport()->rect()).boundingRect().size();
        QPointF center=view_->mapToScene(view_->viewport()->rect().center());int best=-1;
        for(auto* candidate:items)if(candidate->type()==QtNodes::NodeGraphicsObject::Type) {
            const auto point=candidate->sceneBoundingRect().center();const QRectF area(point-QPointF(size.width()/2,size.height()/2),size);
            const int score=densityScore(items,area);
            if(score>best){best=score;center=point;}
        }
        // Use the real pan handler so its retained camera center agrees with
        // the viewport. Raw centerOn() leaves the editor's camera state stale.
        const QPointF start=view_->viewport()->rect().center();
        const auto target=start-(center-view_->mapToScene(start.toPoint()))*view_->transform().m11();
        mouse(QEvent::MouseButtonPress,start,Qt::MiddleButton,Qt::MiddleButton);
        mouse(QEvent::MouseMove,target,Qt::NoButton,Qt::MiddleButton);
        mouse(QEvent::MouseButtonRelease,target,Qt::MiddleButton,Qt::NoButton);
    }
    void observeViewport(const char* phaseName) {
        int nodes=0,wires=0;
        if(view_ && view_->scene())for(auto* item:view_->scene()->items(view_->mapToScene(view_->viewport()->rect()))) {
            if(!item->isVisible())continue;
            nodes+=item->type()==QtNodes::NodeGraphicsObject::Type;
            wires+=item->type()==QtNodes::ConnectionGraphicsObject::Type;
        }
        bool foreground=window_->isActiveWindow();
#ifdef Q_OS_WIN
        DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);foreground=pid==GetCurrentProcessId();
#endif
        viewport_samples_.append(QJsonObject{{"phase",phaseName},{"elapsed_ms",double(clock_.elapsed())},{"nodes",nodes},{"connections",wires},
            {"foreground_owned",foreground},{"width",view_?view_->viewport()->width():0},{"height",view_?view_->viewport()->height():0},{"scale",view_?view_->transform().m11():0}});
    }
    void observeScenes() {
        for(auto* host:window_->findChildren<ui::quick::SceneHost*>()) {
            auto state=std::make_shared<SceneFrames>();state->host=host;const auto started=clock_;const auto duration=flight_duration_;
            state->connection=QObject::connect(host->quickWindow(),&QQuickWindow::afterRendering,&timer_,[state,started,duration]{
                state->frames[flightPhase(started.elapsed(),duration)].fetch_add(1,std::memory_order_relaxed);
            },Qt::DirectConnection);
            scene_frames_.push_back(std::move(state));
        }
    }
    void changeFlightPhase(int next) {
        release(Qt::MiddleButton);
        for(auto surface:flight_surfaces_)if(surface)surface->quick_cancel_input();
        for(int i=0;i<flight_surfaces_.size();++i)if(flight_surfaces_[i])flight_surfaces_[i]->set_camera_state(camera_bases_[i]);
        camera_offset_=0;
        flight_phase_=next;
        mark(flight_names_[next]);
        // Each phase starts on the dense graph, including after a dock resize.
        centerDensePipeline();
        observeViewport(flight_names_[next]);
        if(next==3 || next==5) {
            pipeline_origin_=view_->viewport()->rect().center();pointer_=pipeline_origin_;
            mouse(QEvent::MouseButtonPress,pointer_,Qt::MiddleButton,Qt::MiddleButton);held_=true;
        }
        std::fprintf(stderr,"TRACE_FLIGHT_PHASE %s\n",flight_names_[next]);std::fflush(stderr);
    }
    void flyCamera(double seconds) {
        const double offset=(1-std::cos(seconds*2*3.141592653589793/3))/2;
        for(int i=0;i<flight_surfaces_.size();++i)if(auto surface=flight_surfaces_[i]) {
            const double distance=cameraDistance(camera_bases_[i]);
            surface->quick_wheel(0,600*distance*.2*(offset-camera_offset_),false);
        }
        camera_offset_=offset;
    }
    void flightStep() {
        const auto ms=clock_.elapsed();const auto next=flightPhase(ms,flight_duration_);
        if(next!=flight_phase_)changeFlightPhase(next);
        const double seconds=(ms-phase_edges_[next]*flight_duration_/100000)/1000.;
        if(next==1 || next==5)flyCamera(seconds);
        if((next==3 || next==5) && view_) {
            if(auto* zoom=window_->findChild<QDoubleSpinBox*>("pipelineZoom"))zoom->setValue(25-2.5*(1-std::cos(seconds*2*3.141592653589793/3)));
            pointer_=pipeline_origin_+QPointF(8*std::sin(seconds*2.1),5*std::sin(seconds*1.7));
            mouse(QEvent::MouseMove,pointer_,Qt::NoButton,Qt::MiddleButton);
        }
    }
    static int sceneNode(ui::quick::SceneHost* host) {
        for(auto* parent=host->parentWidget();parent;parent=parent->parentWidget())
            if(parent->property("graphNode").isValid())return parent->property("graphNode").toInt();
        return 0;
    }
    QJsonArray flightReports() const {
        QJsonArray result;const auto elapsed=clock_.elapsed();
        for(int i=0;i<7;++i) {
            const auto end=i==6?elapsed:std::min(elapsed,phase_edges_[i+1]*flight_duration_/100000);
            const double seconds=std::max<qint64>(0,end-phase_edges_[i]*flight_duration_/100000)/1000.;
            QJsonArray scenes;
            for(const auto& state:scene_frames_)if(state->host && state->frames[i].load()) {
                const auto frames=state->frames[i].load();auto* view=qobject_cast<QQuickView*>(state->host->quickWindow());
                scenes.append(QJsonObject{{"node",sceneNode(state->host)},{"source",view?view->source().toString():QString{}},
                    {"completed",double(frames)},{"fps",seconds>0?frames/seconds:0}});
            }
            result.append(QJsonObject{{"phase",flight_names_[i]},{"seconds",seconds},{"scenes",scenes}});
        }
        return result;
    }
    static const char* phase(int tick) {
        if(tick<64)return "pipeline_pan";
        if(tick<96)return "node_click";
        if(tick<160)return "node_drag";
        return "chart_pan";
    }
    void mouse(QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons) {
        if(!view_)return;
        QMouseEvent event(type,point,QPointF(view_->viewport()->mapToGlobal(point.toPoint())),button,buttons,Qt::NoModifier);
        QCoreApplication::sendEvent(view_->viewport(),&event);
    }
    void release(Qt::MouseButton button) {
        if(held_)mouse(QEvent::MouseButtonRelease,pointer_,button,Qt::NoButton);
        held_=false;
    }
    void pipelinePan(int tick) {
        if(tick==0){pointer_={120,120};mouse(QEvent::MouseButtonPress,pointer_,Qt::MiddleButton,Qt::MiddleButton);held_=true;}
        pointer_+=QPointF(tick%16<8?2:-2,1);
        mouse(QEvent::MouseMove,pointer_,Qt::NoButton,Qt::MiddleButton);
        if(tick==63)release(Qt::MiddleButton);
    }
    QPointF nodePosition() const {
        return view_->mapFromScene(node_->sceneBoundingRect().topLeft()+QPointF(30,15));
    }
    void nodeClick(int tick) {
        if(!view_ || !node_ || tick%8)return;
        pointer_=nodePosition();
        mouse(QEvent::MouseButtonPress,pointer_,Qt::LeftButton,Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease,pointer_,Qt::LeftButton,Qt::NoButton);
    }
    void nodeDrag(int tick) {
        if(!view_ || !node_)return;
        if(tick==96){pointer_=nodePosition();mouse(QEvent::MouseButtonPress,pointer_,Qt::LeftButton,Qt::LeftButton);held_=true;}
        pointer_+=QPointF(tick%16<8?2:-2,0);
        mouse(QEvent::MouseMove,pointer_,Qt::NoButton,Qt::LeftButton);
        if(tick==159)release(Qt::LeftButton);
    }
    bool chartPan(int tick) {
        for(auto* chart:window_->findChildren<QuickPlotWidget*>()) {
            if(!chart->isVisible())continue;
            auto* model=chart->presentation_model();const auto kind=model->state().value("kind").toString();
            if(kind!="vector" && kind!="image")continue;
            if(tick==160)model->navigate("zoom",0.5,0.5,0.5,3);
            model->navigate("pan",tick%16<8?0.01:-0.01,0.005,1,3);return true;
        }
        return surfacePan(tick);
    }
    bool surfacePan(int tick) {
        if(!surface_)for(auto* panel:window_->findChildren<GraphPanel*>())
            if(panel->isVisible() && panel->active_spectrogram()){surface_=panel->active_spectrogram();break;}
        if(!surface_)return false;
        if(tick==160)surface_->quick_pointer(0,200,200,Qt::RightButton,Qt::RightButton,Qt::NoModifier);
        surface_->quick_pointer(1,200+(tick%16)*2,200,Qt::NoButton,Qt::RightButton,Qt::NoModifier);
        if(tick==223)releaseSurface();
        return true;
    }
    void releaseSurface() {
        if(surface_)surface_->quick_pointer(2,200,200,Qt::RightButton,Qt::NoButton,Qt::NoModifier);
    }
    void step() {
        const auto now=clock_.nsecsElapsed();const auto tick=tick_++%224;
        QElapsedTimer dispatch;dispatch.start();bool performed=true;
        if(flight_)flightStep();
        else if(tick<64)pipelinePan(tick);
        else if(tick<96)nodeClick(tick);
        else if(tick<160)nodeDrag(tick);
        else performed=chartPan(tick);
        samples_.append(QJsonObject{{"phase",flight_?flight_names_[flight_phase_]:phase(tick)},{"elapsed_ms",now/1e6},{"interval_ms",(now-previous_)/1e6},
            {"dispatch_ms",dispatch.nsecsElapsed()/1e6},{"target_found",performed}});
        previous_=now;
    }
    void chooseNode() {
        if(!view_ || !view_->scene())return;
        for(auto* item:view_->scene()->items()) {
            if(item->type()!=QtNodes::NodeGraphicsObject::Type)continue;
            const auto point=view_->mapFromScene(item->sceneBoundingRect().topLeft()+QPointF(30,15));
            if(view_->viewport()->rect().contains(point)){node_=item;return;}
        }
    }
public:
    static void mark(const char* name) {
#if BARELIEF_DEV_TELEMETRY
        if(core::telemetry::active())TracyCMessage(name,std::strlen(name));
#else
        (void)name;
#endif
    }
    bool populated_pipeline() const {
        if(!flight_)return true;
        for(const auto& sample:viewport_samples_) {
            const auto viewport=sample.toObject();
            if(viewport.value("nodes").toInt()<5 || viewport.value("connections").toInt()<5)return false;
            if(viewport.value("phase")=="pipeline_pan")
                if(!viewport.value("foreground_owned").toBool())return false;
        }
        return viewport_samples_.size()==7;
    }
    explicit GuiInteractionTrace(QWidget* window,int duration=100000,SchemeEditorWidget* editor=nullptr):window_(window),editor_(editor),flight_duration_(std::max(1,duration)) {
        view_=editor?editor->findChild<QGraphicsView*>():window->findChild<QGraphicsView*>();if(flight_)centerDensePipeline();chooseNode();
        if(flight_ && view_)view_->viewport()->installEventFilter(this);
        timer_.setTimerType(Qt::PreciseTimer);timer_.setInterval(16);
        QObject::connect(&timer_,&QTimer::timeout,&timer_,[this]{step();});
        if(flight_) {
            for(auto* panel:window_->findChildren<GraphPanel*>())
                if(panel->isVisible() && panel->active_spectrogram()){
                    auto* surface=panel->active_spectrogram();surface->reset_camera();
                    surface->quick_wheel(0,-600*cameraDistance(surface->camera_state())*.4,false);
                    flight_surfaces_.append(surface);camera_bases_.append(surface->camera_state());
                }
        }
        clock_.start();
        if(flight_)observeScenes();
        timer_.start();
    }
    ~GuiInteractionTrace(){timer_.stop();for(const auto& state:scene_frames_)QObject::disconnect(state->connection);}
    bool stop(const QString& output) {
        timer_.stop();release(flight_ || tick_%224<64?Qt::MiddleButton:Qt::LeftButton);releaseSurface();
        if(view_)view_->viewport()->removeEventFilter(this);
        for(auto surface:flight_surfaces_)if(surface)surface->quick_cancel_input();
        for(const auto& state:scene_frames_)QObject::disconnect(state->connection);
        QJsonArray cameras;
        for(int i=0;i<flight_surfaces_.size();++i)if(flight_surfaces_[i])
            cameras.append(QJsonObject{{"baseline",camera_bases_[i]},{"final",flight_surfaces_[i]->camera_state()}});
        QSaveFile file(output);
        if(!file.open(QIODevice::WriteOnly))return false;
        file.write(QJsonDocument(QJsonObject{{"pipeline_found",bool(view_)},{"node_found",node_!=nullptr},
            {"samples",samples_},{"phases",flight_?flightReports():QJsonArray{}},{"viewports",viewport_samples_},
            {"camera_motion","forward_backward"},{"camera_distance_fraction",QJsonArray{0.8,1.0}},{"cameras",cameras},
            {"pipeline_zoom_percent",QJsonArray{20,25}}}).toJson());
        return file.commit();
    }
};
