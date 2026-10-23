#include "performance_report.h"
#include <core_app/graph_panel.h>
#include <core_app/plot_composition.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/numeric_view_adapter.h>
#include <desktop/numeric_project_document.h>
#include <pipeline/numeric_engine.h>
#include <barelief/charts/freq.h>
#include <barelief/axis_tick_labels.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <barelief/spectrograms/spectrogram_selection.h>
#include <barelief/quick/quick_ui.h>
#include <barelief/render_preparation_policy.h>
#include <barelief/ui_text.h>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QQuickWidget>
#include <QQuickItem>
#include <barelief/quick3d/surface_quick_item.h>
#include <barelief/quick3d/surface_overlay_item.h>
#include <QPaintEngine>
#include <QStandardItemModel>
#include <QThread>
#include <QTimer>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace pipeline;
using namespace signal_contracts;
namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
bool wait_for(const std::function<bool()>& predicate,int milliseconds=5000){
    QElapsedTimer timer;timer.start();
    while(!predicate()&&timer.elapsed()<milliseconds){QCoreApplication::processEvents();QThread::msleep(2);}
    return predicate();
}
struct Capture {IAudioSource::AudioHandler audio;IAudioSource::StatusHandler status;bool running=false;};
class Source final:public IAudioSource {
    std::shared_ptr<Capture> capture;
public:
    explicit Source(std::shared_ptr<Capture> value):capture(std::move(value)){}
    void configure(const QJsonObject&)override{}
    void start()override{capture->running=true;capture->status(true,{});}
    void stop()override{capture->running=false;}
    bool is_running()const override{return capture->running;}
    AudioSourceKind source()const override{return AudioSourceKind::SystemOutput;}
    void set_audio_handler(AudioHandler value)override{capture->audio=std::move(value);}
    void set_status_handler(StatusHandler value)override{capture->status=std::move(value);}
    void set_lifecycle_handler(LifecycleHandler)override{}
};
bool enabled(QComboBox* combo,int index){
    const auto model=qobject_cast<QStandardItemModel*>(combo->model());
    return model&&index>=0&&model->item(index)&&model->item(index)->isEnabled();
}
void select(QComboBox* combo,const char* mode){
    const auto index=combo->findData(mode);require(enabled(combo,index),"requested axis mode disabled despite valid source metadata");combo->setCurrentIndex(index);
}
void click_owned(SpectrogramGraph* surface,QPointF position){
    surface->quick_pointer(0,position.x(),position.y(),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    surface->quick_pointer(2,position.x(),position.y(),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
}
}

static void save_native_frame(QQuickWidget* host,SpectrogramGraph* surface,const QDir& artifacts,const QString& name) {
auto image=host->grabFramebuffer();require(!image.isNull(),"native framebuffer unavailable");require(image.save(artifacts.filePath(name+".png")),"save own framebuffer");
            QFile file(artifacts.filePath(name+".json"));require(file.open(QIODevice::WriteOnly),"telemetry output");file.write(QJsonDocument(QJsonObject::fromVariantMap(surface->axis_layout_telemetry())).toJson());}

static void feed_native_pcm(const std::shared_ptr<Capture>& capture,std::uint64_t block) {

            AudioFrame frame;frame.sample_rate=48000;frame.sequence=block+1;frame.generation=1;frame.timestamp_ns=std::int64_t(block*256*1000000000ULL/48000);frame.channel_count=1;frame.frame_count=256;frame.samples.resize(256);frame.sample_position=(frame.sequence?frame.sequence-1:0)*256;
            for(std::size_t i=0;i<256;++i)for(int tone=0;tone<8;++tone){
                const auto phase=6.283185307179586*double(73+113*tone)*double(block*256+i)/48000.;
                const auto amplitude=.015+.005*std::sin(double(block)/37.+tone);
                frame.samples[i]+=float(amplitude*std::sin(phase));
            }
            capture->audio(frame);
}

static void native_worker_lifecycle(GraphPanel& panel,SpectrogramGraph* surface,const auto& pumpFor) {
        if(presentation::RenderPreparationPolicy::mesh!=presentation::MeshPreparation::Inline) {
            // A completed older packet may be shown, but the latest ingress
            // must catch up even when no further data arrives to wake the view.
            require(surface->set_axis_metadata({}),"worker numeric metadata reset");
            surface->set_history_dimensions(400,50);
            for(int value=1;value<=12;++value) {
                surface->update_frame_history(std::vector<std::vector<float>>(400,std::vector<float>(50,float(value))));
                QCoreApplication::processEvents();
            }
            require(wait_for([&]{return surface->axis_layout_telemetry().value("latestDisplayedValue").toFloat()==12.f;}),
                    "worker stranded final ingress snapshot");
            surface->update_frame_history({});
            surface->set_history_dimensions(8,3);
            surface->update_frame_history(std::vector<std::vector<float>>(8,std::vector<float>(3,23.f)));
            panel.hide();pumpFor(50);panel.show();
            require(wait_for([&]{const auto t=surface->axis_layout_telemetry();return t.value("latestDisplayedValue").toFloat()==23.f && t.value("historyCapacity").toInt()==8 && t.value("historyBands").toInt()==3;}),
                    "worker accepted stale clear/resize packet or failed show");
            pumpFor(150);const auto settled=surface->axis_layout_telemetry();pumpFor(100);
            require(surface->axis_layout_telemetry().value("paintCount")==settled.value("paintCount"),"worker idle repaint loop");
            // Leave an owned job potentially in flight during normal teardown.
            surface->set_history_dimensions(400,50);
            surface->update_frame_history(std::vector<std::vector<float>>(400,std::vector<float>(50,31.f)));
            QCoreApplication::processEvents();
        }}

static void native_idle_and_worker(GraphPanel& panel,SpectrogramGraph* surface) {
        // Drain the final coalesced submission/feedback, then verify that the
        // rate-capped submission policy does not become an idle render loop.
        auto pumpFor=[](int milliseconds) {
            QElapsedTimer timer;timer.start();
            while(timer.elapsed()<milliseconds){QCoreApplication::processEvents();QThread::msleep(2);}
        };
        pumpFor(150);
        const auto idleBaseline=surface->axis_layout_telemetry();
        pumpFor(200);
        const auto idleEnd=surface->axis_layout_telemetry();
        require(idleEnd.value("paintCount")==idleBaseline.value("paintCount") &&
                idleEnd.value("geometryUploads")==idleBaseline.value("geometryUploads") &&
                idleEnd.value("renderSubmissions")==idleBaseline.value("renderSubmissions"),
                "renderer kept submitting or painting without changed data/camera");
        // Axes/ranges/samples are projected in one GUI delivery. The frame pump
        // intentionally coalesces its setters until that delivery completes;
        // publishing synchronously would expose a partially updated snapshot.
        const auto submissionsBefore=idleEnd.value("renderSubmissions").toULongLong();
        surface->set_y_range(0,2.5);
        surface->set_y_range(0,3);
        require(surface->axis_layout_telemetry().value("renderSubmissions").toULongLong()==submissionsBefore,
                "presentation setters submitted a partially projected snapshot synchronously");
        require(wait_for([&]{
            const auto telemetry=surface->axis_layout_telemetry();
            const auto ticks=telemetry.value("valueAxisTickValues").toList();
            return telemetry.value("renderSubmissions").toULongLong()>submissionsBefore &&
                   telemetry.value("paintCount").toULongLong()>idleEnd.value("paintCount").toULongLong() &&
                   !ticks.empty() && std::abs(ticks.front().toDouble())<1e-6 &&
                   std::abs(ticks.back().toDouble()-3.)<1e-6;
        }),"coalesced final presentation range never reached native axes");
        pumpFor(150);
        const auto isolatedEnd=surface->axis_layout_telemetry();
        pumpFor(150);
        require(surface->axis_layout_telemetry().value("renderSubmissions")==isolatedEnd.value("renderSubmissions") &&
                surface->axis_layout_telemetry().value("paintCount")==isolatedEnd.value("paintCount"),
                "isolated presentation delivery caused an idle submission/paint loop");
        native_worker_lifecycle(panel,surface,pumpFor);
}

static void native_docking_lifecycle(GraphPanel& panel,QWidget& dockHost,SpectrogramGraph* surface,QQuickWidget* host,const void* owner,std::uint64_t revision,const auto& amplitude,const auto& rendered,const auto& save) {
        panel.resize(760,500);rendered();save("resized");panel.hide();
        QCoreApplication::processEvents();panel.show();rendered();save("shown-again");
        panel.setParent(&dockHost);panel.move(0,0);panel.resize(900,650);dockHost.show();panel.show();rendered();save("reparented");
        require(surface->projection_storage_identity()==owner && surface->history_projection_revision()==revision && surface->y_range()==amplitude,"hide/show or dock reparent changed source data");
        require(surface->axis_layout_telemetry().value("rendererReady").toBool() && host->property("qmlErrors").toStringList().isEmpty(),"dock reparent lost native renderer");
}

static void native_selection(SpectrogramGraph* surface,SurfaceQuickItem* renderer,const auto& physical,const void* owner,std::uint64_t revision,const auto& rendered,const auto& save,int& age,int& band) {
        // Pick a real visible sample through this test widget's event filter.
        bool picked=false;for(int y=renderer->height()/5;y<renderer->height()*4/5&&!picked;y+=std::max(1,int(renderer->height()/10)))for(int x=renderer->width()/5;x<renderer->width()*4/5&&!picked;x+=std::max(1,int(renderer->width()/10))){
            click_owned(surface,{double(x),double(y)});picked=wait_for([&]{return surface->axis_layout_telemetry().value("selectedBand").isValid();},80);
        }
        require(picked,"could not select a visible native sample");rendered();age=surface->axis_layout_telemetry().value("selectedAge").toInt();band=surface->axis_layout_telemetry().value("selectedBand").toInt();
        const auto readout=surface->axis_layout_telemetry().value("selectionReadoutRows").toStringList();require(readout.size()==3,"point readout must contain all three coordinates");
        const QLocale locale("en_US");const auto precision=surface->surface_settings().value("axis_precision").toInt();
        auto timeTitle=ui::text(physical.time_title_key.toUtf8().constData());
        if(timeTitle.endsWith(" (X)"))timeTitle.chop(4);
        const auto timeValue=axisReadoutLabel(physical.age(age),locale,precision);
        const auto timeRow=physical.age_axis?ui::text("Time delay (s): %1").arg(timeValue):timeTitle+": "+timeValue;
        const auto frequencyRow=ui::text(spectrogramCoordinateName(physical.column_title_key))+": "+axisReadoutLabel(physical.column(band),locale,precision);
        if(readout[0]!=timeRow || readout[1]!=frequencyRow)
            std::fprintf(stderr,"selection actual=%s expected=%s\n",qPrintable(readout.join(" | ")),qPrintable(timeRow+" | "+frequencyRow));
        require(readout[0]==timeRow&&readout[1]==frequencyRow,"point coordinate values differ from source axes");
        auto state=surface->presentation_state();state["selection"]=2;surface->set_presentation_state(state);rendered();
        require(surface->axis_layout_telemetry().value("sliceSampleCount").toInt()==50,"time slice missing");const auto indices=surface->axis_layout_telemetry().value("sliceEndpointIndices").toPoint();const auto coordinates=surface->axis_layout_telemetry().value("sliceEndpointCoordinates").toPointF();
        require(std::abs(coordinates.x()-physical.column(indices.x()))<1e-6&&std::abs(coordinates.y()-physical.column(indices.y()))<1e-6,"time slice coordinates differ from native frequency axis");save("physical-time-slice");
        state["selection"]=3;surface->set_presentation_state(state);rendered();require(surface->axis_layout_telemetry().value("sliceSampleCount").toInt()==400,"frequency slice missing");
        auto* nativeOverlay=surface->findChild<QQuickWidget*>("spectrogramQuickView")->rootObject()->findChild<SurfaceOverlayItem*>("spectrogramOverlay");
        require(nativeOverlay && nativeOverlay->renderTarget()==QQuickPaintedItem::FramebufferObject && nativeOverlay->paintEngineType()==QPaintEngine::OpenGL2,
                "native slice overlay did not reach accelerated Qt painter");
        const auto timeIndices=surface->axis_layout_telemetry().value("sliceEndpointIndices").toPoint();const auto delays=surface->axis_layout_telemetry().value("sliceEndpointCoordinates").toPointF();
        require(std::abs(delays.x()-physical.age(timeIndices.x()))<1e-6&&std::abs(delays.y()-physical.age(timeIndices.y()))<1e-6,"frequency slice coordinates differ from native time axis");save("physical-frequency-slice");
        require(surface->projection_storage_identity()==owner&&surface->history_projection_revision()==revision,"axis selectors copied or changed matrix data");
}

static void native_text_cache(GraphPanel& panel,SpectrogramGraph* surface,const auto& physical,const QVariantMap& physicalTelemetry,const auto& rendered,const auto& save) {
        panel.changePreference("frequency_axis_mode","coordinates");panel.changePreference("time_axis_mode","coordinates");surface->reset_camera();rendered();require(surface->projected_axis_metadata()==physical,"physical selector roundtrip changed source axes");
        const auto axisCacheBefore=surface->axis_layout_telemetry();
        ui::Language::instance().set("ru");rendered();const auto russianAxes=surface->axis_layout_telemetry();
        require(russianAxes.value("axisTextLayoutBuilds").toULongLong()>axisCacheBefore.value("axisTextLayoutBuilds").toULongLong() &&
                russianAxes.value("axisGlyphBuilds").toULongLong()>axisCacheBefore.value("axisGlyphBuilds").toULongLong() &&
                russianAxes.value("axisTitleTexts")!=axisCacheBefore.value("axisTitleTexts"),"language change left stale cached axis labels");
        ui::Language::instance().set("en");rendered();
        auto precisionOptions=surface->surface_settings();const auto originalPrecision=precisionOptions.value("axis_precision");
        precisionOptions["axis_precision"]=3;const auto beforePrecision=surface->axis_layout_telemetry().value("axisTextLayoutBuilds").toULongLong();
        surface->set_surface_settings(precisionOptions);rendered();
        require(surface->axis_layout_telemetry().value("axisTextLayoutBuilds").toULongLong()>beforePrecision,"precision change left stale cached ticks");
        precisionOptions["axis_precision"]=originalPrecision;surface->set_surface_settings(precisionOptions);rendered();
        const auto originalPose=surface->camera_state();const auto wideRotation=QQuaternion::fromEulerAngles(-45,45,0);
        const auto widePosition=wideRotation.rotatedVector({0,0,8});
        surface->set_camera_state({{"version",2},{"position",QJsonArray{widePosition.x(),widePosition.y(),widePosition.z()}},
            {"quaternion",QJsonArray{wideRotation.scalar(),wideRotation.x(),wideRotation.y(),wideRotation.z()}}});
        rendered();require(surface->axis_layout_telemetry().value("axisTitlesDrawn").toInt()==3,"wide native view lost retained axis title badges");save("wide-cached-titles");
        surface->set_camera_state(originalPose);rendered();
        const auto restored=surface->axis_layout_telemetry();save("physical-roundtrip");require(restored.value("bandAxisTickValues")==physicalTelemetry.value("bandAxisTickValues")&&restored.value("timeAxisTickValues")==physicalTelemetry.value("timeAxisTickValues"),"native ticks failed physical roundtrip");
        require(restored.value("geometryUploadVerified").toBool() && restored.value("vertexCount").toInt()>0,"native Quick VBO and IBO exact readback failed");
}

static void native_camera_poses(SpectrogramGraph* surface,const QVariantMap& baseline,const auto& cameraGeometryUploads,const auto& cameraTopologyUploads,const void* owner,std::uint64_t revision,const auto& amplitude,const auto& rendered,const auto& save) {
        const QStringList stableKeys{"timeAxisSections","bandAxisSections","valueAxisSections","timeAxisTickValues","bandAxisTickValues","valueAxisTickValues","axisPolicyRevision","gridRevision","horizontalGridLines","horizontalGridLinesTotal","axisTextLayoutBuilds","axisGlyphBuilds","axisTitleTexts"};
        int pose=0;for(const auto orientation:std::array<QPointF,6>{QPointF{0,30},{35,15},{80,40},{130,10},{220,60},{310,-15}}){
            const auto beforeCamera=surface->camera_state();
            const auto rotation=QQuaternion::fromEulerAngles(-float(orientation.y()),-float(orientation.x()),0);
            const float distance=2.5f+.25f*pose;
            const auto position=rotation.rotatedVector({0,0,distance});
            surface->set_camera_state({{"version",2},{"position",QJsonArray{position.x(),position.y(),position.z()}},
                {"quaternion",QJsonArray{rotation.scalar(),rotation.x(),rotation.y(),rotation.z()}}});
            require(surface->camera_state()!=beforeCamera,"programmatic camera pose was ignored");
            const auto actualPosition=surface->camera_state().value("position").toArray();
            require(actualPosition.size()==3&&std::abs(QVector3D(actualPosition[0].toDouble(),actualPosition[1].toDouble(),actualPosition[2].toDouble()).length()-distance)<1e-5,"camera distance was ignored");
            rendered();const auto current=surface->axis_layout_telemetry();
            for(const auto& key:stableKeys)require(current.value(key)==baseline.value(key),qPrintable("camera changed axis/grid policy: "+key));
            require(current.value("geometryUploads")==cameraGeometryUploads && current.value("topologyUploads")==cameraTopologyUploads,
                    "camera-only render reuploaded vertices or triangle topology");
            require(surface->projection_storage_identity()==owner&&surface->history_projection_revision()==revision&&surface->y_range()==amplitude,"camera mutated matrix or amplitude");save(QString("camera-%1").arg(++pose));
        }
}

static void native_camera_motion(GraphPanel& panel,SpectrogramGraph* surface,const QVariantMap& baseline,const auto& rendered) {
        int cameraSidebarNotifications=0;
        const auto cameraSidebarConnection=QObject::connect(&panel,&GraphPanel::state_changed,&panel,[&]{++cameraSidebarNotifications;});
        surface->quick_pointer(0,200,200,Qt::RightButton,Qt::RightButton,Qt::NoModifier);
        for(int motion=1;motion<=80;++motion)surface->quick_pointer(1,200+motion,200+motion/4,Qt::NoButton,Qt::RightButton,Qt::NoModifier);
        surface->quick_pointer(2,280,220,Qt::RightButton,Qt::NoButton,Qt::NoModifier);
        rendered();
        QObject::disconnect(cameraSidebarConnection);
        const auto movedTelemetry=surface->axis_layout_telemetry();
        require(cameraSidebarNotifications==0 && panel.preferences().value("camera").toMap()==surface->camera_state().toVariantMap(),"native mouse motion rebuilt sidebar or lost persisted pose");
        for(const auto* key:{"geometryUploads","topologyUploads","gridRevision","axisPolicyRevision","axisTextLayoutBuilds","axisGlyphBuilds","axisTitleTexts"})
            require(movedTelemetry.value(key)==baseline.value(key),"native mouse motion rebuilt data geometry or axis layout");
}

static auto native_physical_axes(GraphPanel& panel,SpectrogramGraph* surface,const domain::NumericProcessingGraph& graph,QVariantMap& physicalTelemetry,const auto& rendered) {
        panel.changePreference("frequency_axis_mode","coordinates");panel.changePreference("time_axis_mode","coordinates");rendered();
        const auto physical=surface->projected_axis_metadata();require(physical.column_coordinates&&physical.columns.size()==50&&physical.column(49)>1000,"Hertz selector still displays Mel positions");
        require(graph.execution_interval.numerator>0 && graph.execution_interval.denominator>0,"invalid analysis cadence fixture");
        const double expectedHistoryDelay=399.*double(graph.execution_interval.numerator)/double(graph.execution_interval.denominator);
        // Fractional deadlines are quantized to source samples. Comparing the
        // interval endpoints admits at most two source sample periods, rather
        // than assuming a historical fixed 60 Hz analysis rate.
        const auto sourceFs=std::get<NumericContract>(graph.nodes.front().outputs.front().contract.view()).clock.sample_rate;
        require(sourceFs>0,"default source clock missing");
        const double delayTolerance=2./double(sourceFs)+1.e-9;
        require(physical.time_coordinates&&physical.ages.size()==400&&physical.age(0)==0&&
                std::abs(physical.age(399)-expectedHistoryDelay)<=delayTolerance,
                "seconds selector differs from exact graph cadence/source-sample timing");
        physicalTelemetry=surface->axis_layout_telemetry();require(physicalTelemetry.value("bandAxisTitle")==physical.column_title_key&&physicalTelemetry.value("timeAxisTitle")==physical.time_title_key,"native renderer titles differ from projected physical axes");
        const auto frequencyTicks=physicalTelemetry.value("bandAxisTickValues").toList(),delayTicks=physicalTelemetry.value("timeAxisTickValues").toList();
        const auto maximumTick=[](const QVariantList& values){double maximum=-1;for(const auto& value:values)maximum=std::max(maximum,value.toDouble());return maximum;};
        require(maximumTick(frequencyTicks)>1000 && std::abs(maximumTick(delayTicks)-expectedHistoryDelay)<=delayTolerance,
                "native physical ticks differ from exact graph cadence or remain positions");
return physical;
}

template<class Feed,class Render,class Save>
struct NativeStreamingMeasurement {
 NumericEngine& engine;const EndpointIdentity& history;presentation::NumericViewAdapter& adapter;
 GraphPanel& panel;SpectrogramGraph* surface;QQuickWidget* host;SurfaceQuickItem* renderer;
 const QDir& artifacts;const Feed& feedBlock;const Render& rendered;const Save& save;
 std::uint64_t streamPaints{},streamUploads{},streamTopology{},streamSubmissions{},startOrdinal{},nextBlock{},presentationUpdates{},lastOrdinal{};
 double measuredSeconds{};
 void prepare() {
        // Bounded streaming measurement: changing eight-tone PCM advances the
        // actual executor, History, projection adapter and native Quick FBO.
        // No camera-only update loop, readback in the timed region, desktop
        // capture, input automation or promise of a particular hardware FPS.
        renderer->backend()->setProperty("verifyGeometryUpload",false);
        // GUI cadence is applied separately from surface appearance settings,
        // exactly as the application composition does for global render rate.
        panel.set_render_fps(120);
        require(panel.render_fps()==120,"streaming render target setter did not apply");
        rendered();
        const auto streamStart=surface->axis_layout_telemetry();
        streamPaints=streamStart.value("paintCount").toULongLong();
        streamUploads=streamStart.value("geometryUploads").toULongLong();
        streamTopology=streamStart.value("topologyUploads").toULongLong();
        streamSubmissions=streamStart.value("renderSubmissions").toULongLong();
        startOrdinal=0;
        {
            DeliveredNumericFrame current;std::uint64_t currentFilled=0;
            require(engine.query_numeric(history,current,&startOrdinal,&currentFilled)==Status::ok && currentFilled==400,
                    "streaming baseline History unavailable");
        }
        nextBlock=1600;presentationUpdates=0;lastOrdinal=startOrdinal;
        // Instrument only this bounded streaming interval. Fixed-capacity
        // counters/trace are reset before processing resumes; export happens
        // after disabling collection, outside DSP/render timing.
        core::perf::enabled=false;
        for(auto& counter:core::perf::counters) {
            counter.count.store(0);counter.total.store(0);counter.maximum.store(0);
            for(auto& bin:counter.histogram)bin.store(0);
        }
        core::perf::event_count.store(0);core::perf::slow_gui_event_count=0;
        for(auto& event:core::perf::events)event.ready.store(false);
        core::perf::enabled=true;
 }
 void measure() {
        QElapsedTimer streamClock;streamClock.start();
        qint64 nextPresentationNs=0;
        while(streamClock.elapsed()<3000) {
            const auto now=streamClock.nsecsElapsed();
            const auto dueBlocks=std::uint64_t(now)*48000ULL/(256ULL*1000000000ULL);
            // Bound catch-up work per event-loop turn; scheduling pressure is
            // visible in emitted blocks/rates rather than hidden in burst FPS.
            for(int burst=0;nextBlock<1600+dueBlocks && burst<32;++burst)feedBlock(nextBlock++);
            if(now>=nextPresentationNs) {
                nextPresentationNs=now+1000000000LL/120;
                DeliveredNumericFrame streamed;std::uint64_t streamedOrdinal=0,streamedFilled=0;
                require(engine.query_numeric(history,streamed,&streamedOrdinal,&streamedFilled)==Status::ok,
                        "streaming History became unavailable");
                if(streamedOrdinal>lastOrdinal) {
                    require(streamedFilled==400 && adapter.accept_history_snapshot(streamed,streamedOrdinal,streamedFilled)==Status::ok &&
                            adapter.render(history,0,panel.render_target(true))==Status::ok,"streaming native source projection failed");
                    lastOrdinal=streamedOrdinal;++presentationUpdates;
                }
            }
            QCoreApplication::processEvents();QThread::msleep(1);
        }
        measuredSeconds=double(streamClock.nsecsElapsed())/1.e9;
        core::perf::enabled=false;
 }
 void report() {
        const auto streamEnd=surface->axis_layout_telemetry();
        require(save_performance_report(artifacts.filePath("streaming-profile.json")),"streaming profile export");
        const auto painted=streamEnd.value("paintCount").toULongLong()-streamPaints;
        const auto uploaded=streamEnd.value("geometryUploads").toULongLong()-streamUploads;
        const auto submissions=streamEnd.value("renderSubmissions").toULongLong()-streamSubmissions;
        require(submissions>0 && submissions<=std::uint64_t(std::ceil(measuredSeconds*120))+2,
                "dirty streaming submissions exceeded configured GUI cadence");
        require(presentationUpdates>1 && lastOrdinal>startOrdinal && painted>0 && uploaded>0,
                "streaming measurement did not process and render changing source data");
        require(streamEnd.value("topologyUploads").toULongLong()==streamTopology,
                "same-size streaming data reuploaded triangle topology");
        // Dynamic bytes include crease indices; fixed triangle indices must
        // not be uploaded again for these same-size matrices.
        require(streamEnd.value("geometryUploadedBytes").toULongLong()==480000ULL+
                2ULL*streamEnd.value("wireframeEdgeCount").toULongLong()*sizeof(std::uint32_t),
                "streaming upload bytes include duplicated vertices or static topology");
        const QJsonObject performance{
            {"scenario","native_quick_actual_pcm_history_stream"},{"measurement_seconds",measuredSeconds},
            {"source_sample_rate",48000},{"source_block_samples",256},{"requested_gui_fps",120},{"applied_gui_fps",panel.render_fps()},
            {"measured_render_fps",double(painted)/measuredSeconds},{"measured_geometry_upload_hz",double(uploaded)/measuredSeconds},
            {"painted_frames",double(painted)},{"geometry_uploads",double(uploaded)},{"render_submissions",double(submissions)},
            {"presentation_updates",double(presentationUpdates)},{"source_blocks",double(nextBlock-1600)},
            {"processed_history_rows",double(lastOrdinal-startOrdinal)},
            {"topology_uploads",double(streamEnd.value("topologyUploads").toULongLong()-streamTopology)},
            {"viewport_width",host->width()},{"viewport_height",host->height()},
            {"gpu_readback_during_measurement",false},{"telemetry",QJsonObject::fromVariantMap(streamEnd)}};
        QFile performanceFile(artifacts.filePath("streaming-performance.json"));
        require(performanceFile.open(QIODevice::WriteOnly),"streaming performance output");
        require(performanceFile.write(QJsonDocument(performance).toJson())>0,"streaming performance write");
        std::printf("native streaming: %.2f measured render FPS, %.2f geometry uploads/s over %.3fs; requested GUI120, actual PCM/History updates=%llu\n",
                    double(painted)/measuredSeconds,double(uploaded)/measuredSeconds,measuredSeconds,
                    (unsigned long long)presentationUpdates);
        save("streaming-final");
 }
};

static auto native_position_baseline(GraphPanel& panel,SpectrogramGraph* surface,const auto& rendered,const auto& save) {
        save("physical");panel.changePreference("frequency_axis_mode","positions");panel.changePreference("time_axis_mode","positions");rendered();
        const auto positions=surface->projected_axis_metadata();require(!positions.column_coordinates&&positions.column(49)==49&&positions.age(399)==399,"positions selector not applied");
        auto baseline=surface->axis_layout_telemetry();require(baseline.value("valueAxisSections").toInt()>=4&&baseline.value("horizontalGridLines").toInt()>=5,"height grid is missing or under-divided");save("positions");
        require(baseline.value("axisTextLayoutBuilds").toULongLong()>0 && baseline.value("axisGlyphBuilds").toULongLong()>0 &&
                baseline.value("axisTitleStorage")=="gpu-atlas" && baseline.value("axisTitleTexts").toStringList().size()==3,
                "native axes did not prepare retained text/title resources");
        require(baseline.value("uniqueVertexCount").toULongLong()==400*50 &&
                baseline.value("surfaceIndexCount").toULongLong()==399*49*6 &&
                baseline.value("vertexCount")==baseline.value("surfaceIndexCount") &&
                baseline.value("geometryVertexBytes").toULongLong()==400*50*6*sizeof(float) &&
                baseline.value("geometryIndexBytes").toULongLong()==399*49*6*sizeof(std::uint32_t),
                "native indexed geometry byte/count contract differs from actual 400x50 source");
        require(baseline.value("geometryUploadVerified").toBool(),"native VBO and IBO readback failed");
return baseline;
}

static void prepare_native_panel(GraphPanel& panel,presentation::NumericViewAdapter& adapter,const EndpointIdentity& history,DeliveredNumericFrame& frame,std::uint64_t ordinal,std::uint64_t filled,const domain::NumericProcessingGraph& graph,SpectrogramGraph*& surface,QQuickWidget*& host,SurfaceQuickItem*& renderer) {
        panel.setAttribute(Qt::WA_ShowWithoutActivating);panel.setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);panel.resize(1280,800);
        require(std::none_of(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),[](const auto& binding){return binding.to==8;}),"default spectrogram has no coordinate authorization wires");
        panel.set_axis_permissions({true,true});
        surface=panel.active_spectrogram();require(surface,"native 3D surface");surface->set_y_range(0,2);
        auto options=surface->surface_settings();options["axis_grid"]=true;options["floor_grid"]=true;options["shadows"]=false;surface->set_surface_settings(options);
        require(adapter.accept_history_snapshot(frame,ordinal,filled)==Status::ok&&adapter.render(history,0,panel.render_target(true))==Status::ok,"actual source projection");frame={};
        panel.show();host=surface->findChild<QQuickWidget*>("spectrogramQuickView");
        renderer=host && host->rootObject()?host->rootObject()->findChild<SurfaceQuickItem*>():nullptr;require(host && renderer,"native QML surface item missing");
        renderer->backend()->setProperty("verifyGeometryUpload",true);renderer->update();
        require(wait_for([&]{const auto state=surface->axis_layout_telemetry();return host->status()==QQuickWidget::Ready&&state.value("paintCount").toULongLong()>0&&state.value("rendererReady").toBool()&&state.value("axisLabelsReady").toBool();}),"native renderer or axis shaders never became ready");
        require(host->property("qmlErrors").toStringList().isEmpty(),"native QML scene has errors");
}

static void prepare_native_engine(NumericEngine& engine,const auto& capture,const domain::NumericProcessingGraph& graph,bool& reply,bool& accepted) {
QByteArray encoded;require(bool(desktop::encode_numeric_graph(graph,encoded)),"encode actual default graph");
        engine.set_reply_handler([&](const EngineReply& value){reply=true;accepted=value.accepted;});
        EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(encoded).object();engine.submit(command);
        require(wait_for([&]{return reply&&capture->running;})&&accepted&&bool(capture->audio),"apply actual graph and capture adapter");
}

static void await_native_render(SpectrogramGraph* surface,SurfaceQuickItem* renderer) {
for(int fence=0;fence<2;++fence){const auto before=surface->axis_layout_telemetry().value("paintCount").toULongLong();renderer->update();require(wait_for([&]{return surface->axis_layout_telemetry().value("paintCount").toULongLong()!=before;}),"axis change never reached native Quick FBO renderer");}}

static std::pair<int,int> native_projection_scenarios(NumericEngine& engine,const EndpointIdentity& history,presentation::NumericViewAdapter& adapter,GraphPanel& panel,SpectrogramGraph* surface,QQuickWidget* host,SurfaceQuickItem* renderer,QWidget& dockHost,const QDir& artifacts,const domain::NumericProcessingGraph& graph,const void* owner,std::uint64_t revision,const QPair<float,float>& amplitude,const auto& feedBlock,const auto& rendered,const auto& save) {
        QVariantMap physicalTelemetry;const auto physical=native_physical_axes(panel,surface,graph,physicalTelemetry,rendered);
        const auto baseline=native_position_baseline(panel,surface,rendered,save);
        const auto cameraGeometryUploads=baseline.value("geometryUploads"),cameraTopologyUploads=baseline.value("topologyUploads");
        native_camera_motion(panel,surface,baseline,rendered);
        native_camera_poses(surface,baseline,cameraGeometryUploads,cameraTopologyUploads,owner,revision,amplitude,rendered,save);
        native_text_cache(panel,surface,physical,physicalTelemetry,rendered,save);
        int age=0,band=0;native_selection(surface,renderer,physical,owner,revision,rendered,save,age,band);
        native_docking_lifecycle(panel,dockHost,surface,host,owner,revision,amplitude,rendered,save);
        NativeStreamingMeasurement measurement{engine,history,adapter,panel,surface,host,renderer,artifacts,feedBlock,rendered,save};
        measurement.prepare();measurement.measure();measurement.report();
        native_idle_and_worker(panel,surface);
        panel.hide();dockHost.hide();
        return {age,band};
}
static int native_mode_option(int argc,char** argv) {
    if(argc>2) {
        const auto mode=QString::fromLocal8Bit(argv[2]);
        if(mode!="worker" && mode!="inline" && mode!="pooled")return 2;
        presentation::RenderPreparationPolicy::mesh=mode=="worker"?presentation::MeshPreparation::Worker:
            mode=="inline"?presentation::MeshPreparation::Inline:presentation::MeshPreparation::Pooled;
        presentation::RenderPreparationPolicy::snapshot_pool=true;
    }
    return 0;
}
static void arm_native_safety(QTimer& safety,QApplication& app) {
    QObject::connect(&safety,&QTimer::timeout,&app,[]{std::fprintf(stderr,"native axes safety timeout\n");std::_Exit(3);});safety.start(45000);
}
static void configure_native_dock_host(QWidget& dockHost) {
    dockHost.setAttribute(Qt::WA_ShowWithoutActivating);dockHost.setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);dockHost.resize(900,650);
}
int main(int argc,char** argv){
    if(const auto result=native_mode_option(argc,argv))return result;
    ui::quick::configure();
    QApplication app(argc,argv);
    if(QGuiApplication::platformName()=="offscreen"||QGuiApplication::platformName()=="minimal"){
        std::fprintf(stderr,"native axes test requires a native OpenGL platform\n");return 2;
    }
    QTimer safety;arm_native_safety(safety,app);
    try{
        ui::Language::instance().set("en");
        require(argc==2 || argc==3,"provide an artifact directory and optional mesh mode");QDir artifacts(QString::fromLocal8Bit(argv[1]));require(artifacts.mkpath("."),"artifact directory");
        auto capture=std::make_shared<Capture>();NumericEngine engine([capture](auto){return std::make_unique<Source>(capture);});
        const auto graph=numeric_default_graph();bool reply=false,accepted=false;
        prepare_native_engine(engine,capture,graph,reply,accepted);
        // Deterministic changing eight-tone PCM; never speakers or OS capture.
        auto feedBlock=[&](std::uint64_t block){feed_native_pcm(capture,block);
        };
        for(std::uint64_t block=0;block<1600;++block)feedBlock(block);
        const EndpointIdentity history{"main",7,domain::PortDirection::Output,0};DeliveredNumericFrame frame;std::uint64_t ordinal=0,filled=0;
        require(engine.query_numeric(history,frame,&ordinal,&filled)==Status::ok&&filled==400&&ordinal>400,"actual History did not reach full timestamp coverage");
        presentation::NumericViewAdapter adapter({1,{400,50,1048576,{true,32768}},{true,32768}});require(adapter.bind(history)==Status::ok,"bind actual History");
        QWidget dockHost;
        configure_native_dock_host(dockHost);
        GraphPanel panel("nativeAxes",new LineChartView,{{"kind","SpectrogramView"},{"display_mode","surface3d"}},nullptr,nullptr,
            presentation::plot_capabilities(presentation::PlotDescriptor{{"image2d","surface3d"},{},false}));
        SpectrogramGraph* surface=nullptr;QQuickWidget* host=nullptr;SurfaceQuickItem* renderer=nullptr;
        prepare_native_panel(panel,adapter,history,frame,ordinal,filled,graph,surface,host,renderer);
        auto save=[&](const QString& name){save_native_frame(host,surface,artifacts,name);};
        require(panel.physical_frequency() && panel.physical_time(),"physical selector capabilities unavailable");
        // A new Quick scene/context after docking can recreate the renderer and reset its counter.
        auto rendered=[&]{await_native_render(surface,renderer);};
        const auto owner=surface->projection_storage_identity();const auto revision=surface->history_projection_revision();const auto amplitude=surface->y_range();
        const auto [age,band]=native_projection_scenarios(engine,history,adapter,panel,surface,host,renderer,dockHost,artifacts,graph,owner,revision,amplitude,feedBlock,rendered,save);
        std::printf("native axes PASS actual default graph: History=%llu/%llu, sample=(%d,%d), physical/index selectors, six camera poses, both slices; own framebuffer artifacts only\n",(unsigned long long)filled,(unsigned long long)ordinal,age,band);return 0;
    }catch(const std::exception& error){core::perf::enabled=false;std::fprintf(stderr,"native axes FAIL: %s\n",error.what());return 1;}
}
