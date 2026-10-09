#include <core_app/graph_panel.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/numeric_view_adapter.h>
#include <desktop/numeric_project_document.h>
#include <pipeline/numeric_engine.h>
#include <visualization/charts/freq.h>
#include <visualization/axis_tick_labels.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <visualization/spectrograms/spectrogram_selection.h>
#include <visualization/ui_text.h>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QOpenGLWidget>
#include <QStandardItemModel>
#include <QThread>
#include <QTimer>
#include <array>
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
void click_owned(QOpenGLWidget* renderer,QPointF position){
    // Synchronous events to our widget only. Never desktop input or focus.
    QMouseEvent down(QEvent::MouseButtonPress,position,position,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QCoreApplication::sendEvent(renderer,&down);
    QMouseEvent up(QEvent::MouseButtonRelease,position,position,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::sendEvent(renderer,&up);
}
}

int main(int argc,char** argv){
    QApplication app(argc,argv);
    if(QGuiApplication::platformName()=="offscreen"||QGuiApplication::platformName()=="minimal"){
        std::fprintf(stderr,"native axes test requires a native OpenGL platform\n");return 2;
    }
    QTimer safety;QObject::connect(&safety,&QTimer::timeout,&app,[]{std::fprintf(stderr,"native axes safety timeout\n");std::_Exit(3);});safety.start(45000);
    try{
        ui::Language::instance().set("en");
        require(argc==2,"provide an artifact directory");QDir artifacts(QString::fromLocal8Bit(argv[1]));require(artifacts.mkpath("."),"artifact directory");
        auto capture=std::make_shared<Capture>();NumericEngine engine([capture](auto){return std::make_unique<Source>(capture);});
        const auto graph=numeric_default_graph();QByteArray encoded;require(bool(desktop::encode_numeric_graph(graph,encoded)),"encode actual default graph");
        bool reply=false,accepted=false;engine.set_reply_handler([&](const EngineReply& value){reply=true;accepted=value.accepted;});
        EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(encoded).object();engine.submit(command);
        require(wait_for([&]{return reply&&capture->running;})&&accepted&&bool(capture->audio),"apply actual graph and capture adapter");
        // Deterministic changing eight-tone PCM; never speakers or OS capture.
        for(std::uint64_t block=0;block<1600;++block){
            AudioFrame frame;frame.sample_rate=48000;frame.sequence=block+1;frame.generation=1;frame.timestamp_ns=std::int64_t(block*256*1000000000ULL/48000);frame.left.resize(256);frame.right.resize(256);
            for(std::size_t i=0;i<256;++i)for(int tone=0;tone<8;++tone){
                const auto phase=6.283185307179586*double(73+113*tone)*double(block*256+i)/48000.;
                const auto amplitude=.015+.005*std::sin(double(block)/37.+tone);
                frame.left[i]+=float(amplitude*std::sin(phase));frame.right[i]+=float(amplitude*std::cos(phase));
            }
            capture->audio(frame);
        }
        const EndpointIdentity history{"main",7,domain::PortDirection::Output,0};DeliveredNumericFrame frame;std::uint64_t ordinal=0,filled=0;
        require(engine.query_numeric(history,frame,&ordinal,&filled)==Status::ok&&filled==400&&ordinal>400,"actual History did not reach full timestamp coverage");
        presentation::NumericViewAdapter adapter({1,{400,50,1048576,{true,32768}},{true,32768}});require(adapter.bind(history)==Status::ok,"bind actual History");
        GraphPanel panel("nativeAxes",new LineChartView,{{"kind","SpectrogramView"},{"display_mode","surface3d"}},nullptr);
        panel.setAttribute(Qt::WA_ShowWithoutActivating);panel.setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);panel.resize(1280,800);
        auto* surface=panel.active_spectrogram();require(surface,"native 3D surface");surface->set_y_range(0,2);
        auto options=surface->surface_settings();options["axis_grid"]=true;options["floor_grid"]=true;options["shadows"]=false;surface->set_surface_settings(options);
        require(adapter.accept_history_snapshot(frame,ordinal,filled)==Status::ok&&adapter.render(history,0,{nullptr,surface,nullptr,true})==Status::ok,"actual source projection");frame={};
        panel.show();auto* renderer=surface->findChild<QOpenGLWidget*>();require(renderer,"native renderer widget missing");
        require(wait_for([&]{return renderer->isValid()&&surface->axis_layout_telemetry().value("paintCount").toULongLong()>0;}),"native renderer never painted");
        auto save=[&](const QString& name){auto image=renderer->grabFramebuffer();require(!image.isNull(),"native framebuffer unavailable");require(image.save(artifacts.filePath(name+".png")),"save own framebuffer");
            QFile file(artifacts.filePath(name+".json"));require(file.open(QIODevice::WriteOnly),"telemetry output");file.write(QJsonDocument(QJsonObject::fromVariantMap(surface->axis_layout_telemetry())).toJson());};
        auto* frequency=panel.findChild<QComboBox*>("frequencyAxisMode");auto* time=panel.findChild<QComboBox*>("timeAxisMode");require(frequency&&time,"actual axis selectors missing");
        auto rendered=[&]{const auto before=surface->axis_layout_telemetry().value("paintCount").toULongLong();renderer->update();require(wait_for([&]{return surface->axis_layout_telemetry().value("paintCount").toULongLong()>before;}),"axis change never reached native paintGL");};
        const auto owner=surface->projection_storage_identity();const auto revision=surface->history_projection_revision();const auto amplitude=surface->y_range();
        select(frequency,"coordinates");select(time,"coordinates");rendered();
        const auto physical=surface->projected_axis_metadata();require(physical.column_coordinates&&physical.columns.size()==50&&physical.column(49)>1000,"Hertz selector still displays Mel positions");
        require(physical.time_coordinates&&physical.ages.size()==400&&physical.age(0)==0&&physical.age(399)>6&&physical.age(399)<7,"seconds selector does not use actual History timing");
        auto physicalTelemetry=surface->axis_layout_telemetry();require(physicalTelemetry.value("bandAxisTitle")=="Frequency, Hz (Z)"&&physicalTelemetry.value("timeAxisTitle")=="Time delay, seconds (X)","native renderer titles not physical");
        const auto frequencyTicks=physicalTelemetry.value("bandAxisTickValues").toList(),delayTicks=physicalTelemetry.value("timeAxisTickValues").toList();
        const auto maximumTick=[](const QVariantList& values){double maximum=-1;for(const auto& value:values)maximum=std::max(maximum,value.toDouble());return maximum;};
        require(maximumTick(frequencyTicks)>1000&&maximumTick(delayTicks)>6&&maximumTick(delayTicks)<7,"native physical tick values are still positions");
        save("physical");select(frequency,"positions");select(time,"positions");rendered();
        const auto positions=surface->projected_axis_metadata();require(!positions.column_coordinates&&positions.column(49)==49&&positions.age(399)==399,"positions selector not applied");
        auto baseline=surface->axis_layout_telemetry();require(baseline.value("valueAxisSections").toInt()>=4&&baseline.value("horizontalGridLines").toInt()>=5,"height grid is missing or under-divided");save("positions");
        const QStringList stableKeys{"timeAxisSections","bandAxisSections","valueAxisSections","timeAxisTickValues","bandAxisTickValues","valueAxisTickValues","axisPolicyRevision","gridRevision","horizontalGridLines","horizontalGridLinesTotal"};
        int pose=0;for(const auto orientation:std::array<QPointF,6>{QPointF{0,30},{35,15},{80,40},{130,10},{220,60},{310,-15}}){
            const auto beforeCamera=surface->camera_state();
            const auto rotation=QQuaternion::fromEulerAngles(-float(orientation.y()),-float(orientation.x()),0);
            const float distance=2.5f+.25f*pose;
            const auto position=rotation.rotatedVector({0,0,distance});
            surface->set_camera_state({{"version",2},{"position",QJsonArray{position.x(),position.y(),position.z()}},
                {"quaternion",QJsonArray{rotation.scalar(),rotation.x(),rotation.y(),rotation.z()}}});
            require(surface->camera_state()!=beforeCamera,"programmatic camera pose was ignored");
            const auto actualPosition=surface->camera_state()["position"].toArray();
            require(actualPosition.size()==3&&std::abs(QVector3D(actualPosition[0].toDouble(),actualPosition[1].toDouble(),actualPosition[2].toDouble()).length()-distance)<1e-5,"camera distance was ignored");
            rendered();const auto current=surface->axis_layout_telemetry();
            for(const auto& key:stableKeys)require(current.value(key)==baseline.value(key),qPrintable("camera changed axis/grid policy: "+key));
            require(surface->projection_storage_identity()==owner&&surface->history_projection_revision()==revision&&surface->y_range()==amplitude,"camera mutated matrix or amplitude");save(QString("camera-%1").arg(++pose));
        }
        select(frequency,"coordinates");select(time,"coordinates");surface->reset_camera();rendered();require(surface->projected_axis_metadata()==physical,"physical selector roundtrip changed source axes");
        const auto restored=surface->axis_layout_telemetry();require(restored.value("bandAxisTickValues")==physicalTelemetry.value("bandAxisTickValues")&&restored.value("timeAxisTickValues")==physicalTelemetry.value("timeAxisTickValues"),"native ticks failed physical roundtrip");
        // Pick a real visible sample through this test widget's event filter.
        bool picked=false;for(int y=renderer->height()/5;y<renderer->height()*4/5&&!picked;y+=std::max(1,renderer->height()/10))for(int x=renderer->width()/5;x<renderer->width()*4/5&&!picked;x+=std::max(1,renderer->width()/10)){
            click_owned(renderer,{double(x),double(y)});picked=wait_for([&]{return renderer->property("selectedBand").isValid();},80);
        }
        require(picked,"could not select a visible native sample");rendered();const auto age=renderer->property("selectedAge").toInt(),band=renderer->property("selectedBand").toInt();
        const auto readout=renderer->property("selectionReadoutRows").toStringList();require(readout.size()==3,"point readout must contain all three coordinates");
        const QLocale locale("en_US");const auto precision=surface->surface_settings()["axis_precision"].toInt();
        require(readout[0]==ui::text("Time delay (s): %1").arg(axisReadoutLabel(physical.age(age),locale,precision))&&readout[1]==ui::text("Frequency (Hz)")+": "+axisReadoutLabel(physical.column(band),locale,precision),"point coordinate values differ from source axes");
        auto state=surface->presentation_state();state["selection"]=2;surface->set_presentation_state(state);rendered();
        require(renderer->property("sliceSampleCount").toInt()==50,"time slice missing");const auto indices=renderer->property("sliceEndpointIndices").toPoint();const auto coordinates=renderer->property("sliceEndpointCoordinates").toPointF();
        require(std::abs(coordinates.x()-physical.column(indices.x()))<1e-6&&std::abs(coordinates.y()-physical.column(indices.y()))<1e-6,"time slice coordinates differ from native frequency axis");save("physical-time-slice");
        state["selection"]=3;surface->set_presentation_state(state);rendered();require(renderer->property("sliceSampleCount").toInt()==400,"frequency slice missing");
        const auto timeIndices=renderer->property("sliceEndpointIndices").toPoint();const auto delays=renderer->property("sliceEndpointCoordinates").toPointF();
        require(std::abs(delays.x()-physical.age(timeIndices.x()))<1e-6&&std::abs(delays.y()-physical.age(timeIndices.y()))<1e-6,"frequency slice coordinates differ from native time axis");save("physical-frequency-slice");
        require(surface->projection_storage_identity()==owner&&surface->history_projection_revision()==revision,"axis selectors copied or changed matrix data");
        panel.resize(760,500);rendered();save("resized");panel.hide();
        std::printf("native axes PASS actual default graph: History=%llu/%llu, sample=(%d,%d), physical/index selectors, six camera poses, both slices; own framebuffer artifacts only\n",(unsigned long long)filled,(unsigned long long)ordinal,age,band);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"native axes FAIL: %s\n",error.what());return 1;}
}
