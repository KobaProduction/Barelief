#include "visualization/spectrograms/spectrogram_graph.h"
#include "visualization/spectrograms/spectrogram_graph_modifier.h"
#include "visualization/spectrograms/spectrogram_scene_geometry.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QOpenGLWidget>
#include <QOpenGLContext>
#include <QMatrix4x4>
#include <QComboBox>
#include <QTest>
#include <QPointer>
#include <QSlider>
#include <QJsonArray>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <limits>
#include <cmath>
#include <memory>
#include "performance_report.h"
#include "visualization/surface_settings.h"

static int checkHistoryAxes(const QString &directory) {
    if(!QDir().mkpath(directory)) return 1;
    SpectrogramGraph graph;graph.set_history_dimensions(7,3);
    graph.initialize({240,180},{1200,900});graph.resize(1000,640);graph.show();QTest::qWait(100);
    auto *native=graph.findChild<QOpenGLWidget *>("spectrogramContainer");
    if(!native || !native->isValid()) return 1;
    for(int frames:{1,4,10}) {
        std::vector<std::vector<float>> input(frames,std::vector<float>(3));
        input[0][0]=.4f;input[frames/2][2]=1.3f;input.back()[1]=1.8f;
        std::vector<std::vector<float>> reference(3,std::vector<float>(frames));
        for(int t=0;t<frames;++t) for(int b=0;b<3;++b) reference[b][t]=input[t][b];
        for(const auto ranges:{QJsonArray{0,100,0,100},QJsonArray{25,75,20,80}}) {
            graph.set_presentation_state({{"ranges",ranges}});
            graph.update(reference);QTest::qWait(70);const auto expected=native->grabFramebuffer();
            graph.update({});QTest::qWait(40);
            graph.update_frame_history(input);QTest::qWait(70);const auto actual=native->grabFramebuffer();
            if(actual!=expected || native->property("historyCapacity").toInt()!=7 ||
                native->property("historyBands").toInt()!=3 ||
                native->property("validHistoryColumns").toInt()!=std::min(frames,7) ||
                (ranges[0].toInt()==0 && (native->property("surfacePeakFrame").toInt()!=6 ||
                    native->property("surfacePeakBand").toInt()!=1 ||
                    std::abs(native->property("surfaceMaximumHeight").toFloat()-.8f)>1e-5f))) {
                qCritical("Frame-major impulse mapping differs from native-compatible band-major mapping");return 1;
            }
            actual.save(directory+QString("/impulses-%1-%2.png").arg(frames).arg(ranges[0].toInt()));
        }
    }
    graph.set_presentation_state({});graph.update_frame_history({{.2f,.3f,.4f,.5f,.6f},{1.f}});QTest::qWait(70);
    if(native->property("validHistoryColumns").toInt()!=0) return 1;
    const int full[4]={0,100,0,100}, cropped[4]={25,75,20,80};
    const SpectrogramSceneGeometry scene(12,5,full), crop(12,5,cropped);
    const auto a=scene.point(2,1,.4f), time=scene.point(8,1,.4f), frequency=scene.point(2,4,.4f);
    if(!(time.x()>a.x()) || time.z()!=a.z() || !(frequency.z()<a.z()) || frequency.x()!=a.x() ||
        crop.xMin!=2.75f || crop.xMax!=8.25f || crop.zMin!=.8f || crop.zMax!=3.2f ||
        std::abs(crop.point(crop.xMin,1,1).x()+crop.scaleX)>1e-5f ||
        std::abs(crop.point(crop.xMax,1,1).x()-crop.scaleX)>1e-5f ||
        std::abs(crop.point(crop.xMax,crop.zMax,1).z()+crop.scaleZ)>1e-5f) return 1;
    std::vector<std::vector<float>> ages{{.3f,.6f,.9f}};
    if(spectrogramHistorySample(ages,0,0,7)!=.9f || spectrogramHistorySample(ages,0,1,7)!=.6f ||
        spectrogramHistorySample(ages,0,3,7)!=0) return 1;
    ages[0].push_back(1.2f);
    if(spectrogramHistorySample(ages,0,0,7)!=1.2f || spectrogramHistorySample(ages,0,1,7)!=.9f) return 1;
    graph.set_history_dimensions(400,50);
    std::vector<std::vector<float>> input(400,std::vector<float>(50));
    for(int t:{20,180,300}) for(int b:{12,25,38}) input[t][b]=1.8f;
    graph.update(input);QTest::qWait(70);native->grabFramebuffer().save(directory+"/layout-before.png");
    graph.update_frame_history(input);QTest::qWait(70);native->grabFramebuffer().save(directory+"/layout-after.png");
    if(native->property("surfacePeakFrame").toInt()!=300 || native->property("surfacePeakAge").toInt()!=99 || native->property("surfacePeakBand").toInt()!=12) return 1;
    qInfo("Axes PASS: newest age0, appended frames age previous samples, oldest capacity-1; frame-major1/4/10 x3 equivalence; padding/overflow/age-range crop; sourceframe300 maps age99 in cap400; Y/Z unchanged");
    return 0;
}

static int checkSelectionOverlay(QApplication &app,const QString &directory) {
    if(!QDir().mkpath(directory)) return 1;
    const auto original=app.palette();
    for(bool light:{false,true}) {
        auto palette=original;const QColor paper=light ? QColor(245,245,245) : QColor(24,24,24);
        palette.setColor(QPalette::Window,paper);palette.setColor(QPalette::Base,paper);
        palette.setColor(QPalette::Text,light ? Qt::black : Qt::white);app.setPalette(palette);
        SpectrogramGraph graph;graph.set_history_dimensions(24,9);graph.initialize({240,180},{1200,900});
        graph.resize(1000,640);graph.show();
        auto *native=graph.findChild<QOpenGLWidget *>("spectrogramContainer");
        auto *mode=graph.findChild<QComboBox *>("spectrogramSelection");if(!native || !mode) return 1;
        const auto top=QQuaternion::fromEulerAngles(-90,0,0);
        graph.set_camera_state({{"version",2},{"position",QJsonArray{0,4,0}},
            {"quaternion",QJsonArray{top.scalar(),top.x(),top.y(),top.z()}}});
        auto options=SurfaceSettings::defaults();options["light"]=false;options["mesh_grid"]=false;
        options["gradient_stops"]=QJsonArray{QJsonObject{{"position",0},{"color","#2255bb"}},QJsonObject{{"position",1},{"color","#2255bb"}}};
        graph.set_surface_settings(options);graph.update_frame_history(std::vector<std::vector<float>>(24,std::vector<float>(9,.8f)));
        mode->setCurrentIndex(0);QTest::qWait(100);
        QMatrix4x4 projection,view;projection.perspective(45,float(native->width())/native->height(),.005f,100);
        const auto actualCamera=graph.camera_state();const auto q=actualCamera["quaternion"].toArray(),eye=actualCamera["position"].toArray();
        view.rotate(QQuaternion(q[0].toDouble(),q[1].toDouble(),q[2].toDouble(),q[3].toDouble()).conjugated());
        view.translate(-QVector3D(eye[0].toDouble(),eye[1].toDouble(),eye[2].toDouble()));
        const auto scene=graph.projected_scene_geometry();
        const auto p=projection*view*QVector4D(scene.point(8,4,.8f),1);
        const QPoint point(qRound((p.x()/p.w()+1)*native->width()/2),qRound((1-p.y()/p.w())*native->height()/2));
        auto pixels=native->grabFramebuffer();const auto dpr=native->devicePixelRatioF();
        const auto color=pixels.pixelColor(qRound(point.x()*dpr),qRound(point.y()*dpr));
        if(color!=QColor("#2255bb")) {qCritical("Custom GPU gradient did not match configured stops");return 1;}
        mode->setCurrentIndex(1);QTest::mouseClick(native,Qt::LeftButton,Qt::NoModifier,point);QTest::qWait(70);
        if(!native->property("selectionMarkerVisible").toBool() || native->property("selectedAge").toInt()!=8 ||
            native->property("selectedBand").toInt()!=4 || std::abs(native->property("selectedValue").toFloat()-.8f)>1e-5) {
            qCritical()<<"Selection mismatch"<<native->property("selectedAge")<<native->property("selectedBand")<<native->property("selectedValue");return 1;
        }
        pixels=native->grabFramebuffer();const auto center=native->property("selectionMarkerPosition").toPointF();
        if(pixels.pixelColor(qRound(center.x()*dpr),qRound(center.y()*dpr))!=QColor(255,210,0)) {
            qCritical("Selection marker is hidden behind the filled surface");return 1;
        }
        pixels.save(directory+(light ? "/point-light.png" : "/point-dark.png"));
        const auto camera=graph.camera_state();
        // Capture the marker once, then move while holding LMB. Frozen source
        // data and camera remain unchanged throughout the selection gesture.
        graph.set_paused(true);
        graph.update_frame_history(std::vector<std::vector<float>>(24,std::vector<float>(9,.9f)));
        const auto destination=projection*view*QVector4D(scene.point(17,7,.8f),1);
        const QPointF destinationPoint((destination.x()/destination.w()+1)*native->width()/2,
                                      (1-destination.y()/destination.w())*native->height()/2);
        QTest::mousePress(native,Qt::LeftButton,Qt::NoModifier,center.toPoint());
        if(!native->property("selectionDragging").toBool())return 1;
        QMouseEvent drag(QEvent::MouseMove,destinationPoint,native->mapToGlobal(destinationPoint.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(native,&drag);QTest::qWait(70);
        if(native->property("selectedAge").toInt()!=17 || native->property("selectedBand").toInt()!=7 ||
           std::abs(native->property("selectedValue").toFloat()-.8f)>1e-5 || graph.camera_state()!=camera)return 1;
        const auto rows=native->property("selectionReadoutRows").toStringList();const auto readout=native->property("selectionReadoutRect").toRect();
        if(rows.size()!=3 || native->property("selectionReadoutLines").toStringList().join("")!=rows.join("") ||
           !native->rect().contains(readout))return 1;
        QTest::mouseRelease(native,Qt::LeftButton,Qt::NoModifier,destinationPoint.toPoint());
        if(native->property("selectionDragging").toBool())return 1;
        QTest::mouseMove(native,point);QTest::qWait(40);
        if(native->property("selectedAge").toInt()!=17 || native->property("selectedBand").toInt()!=7)return 1;
        graph.set_paused(false);
        std::vector<std::vector<float>> sectionData(24,std::vector<float>(9));
        for(int frame=0;frame<24;++frame)for(int band=0;band<9;++band)sectionData[frame][band]=.01f*band+.001f*frame;
        graph.update_frame_history(sectionData);
        SpectrogramAxisMetadata coordinates;coordinates.time_coordinates=true;coordinates.column_coordinates=true;
        coordinates.column_title_key="Frequency, Hz (Z)";
        for(int age=0;age<24;++age)coordinates.ages.push_back(age*.125);
        for(int band=0;band<9;++band)coordinates.columns.push_back(70.1+band*band*.25);
        if(!graph.set_axis_metadata(coordinates))return 1;
        for(int selection:{2,3}) {
            mode->setCurrentIndex(selection);QTest::qWait(70);
            if(native->property("sliceSampleCount").toInt()!=(selection==2 ? 9 : 24)) return 1;
            for(bool reverseTime:{false,true})for(bool reverseBands:{false,true}) {
                options["reverse_time"]=reverseTime;options["reverse_bands"]=reverseBands;
                graph.set_surface_settings(options);QTest::qWait(40);
                const auto endpoints=native->property("sliceEndpointIndices").toPoint();
                const auto values=native->property("sliceEndpointCoordinates").toPointF();
                const int last=selection==2?8:23;
                if(!((endpoints==QPoint(0,last)) || (endpoints==QPoint(last,0))) ||
                   values.x()!=(selection==2?coordinates.column(endpoints.x()):coordinates.age(endpoints.x())) ||
                   values.y()!=(selection==2?coordinates.column(endpoints.y()):coordinates.age(endpoints.y())))return 1;
                const auto samples=native->property("sliceEndpointValues").toPointF();
                const auto expectedValue=[&](int index){return selection==2?sectionData[24-1-17][index]:sectionData[24-1-index][7];};
                if(samples.x()!=expectedValue(endpoints.x()) || samples.y()!=expectedValue(endpoints.y()))return 1;
                const auto geometry=graph.projected_scene_geometry();
                const auto endpoint=[&](int index){return projection*view*QVector4D(geometry.point(
                    selection==2?17:index,selection==2?index:7,geometry.signalTick(0)),1);};
                const auto left=endpoint(endpoints.x()),right=endpoint(endpoints.y());
                const double dx=right.x()/right.w()-left.x()/left.w();
                const double dy=right.y()/right.w()-left.y()/left.w();
                if(dx < -1e-6 || (std::abs(dx)<=1e-6 && dy>1e-6))return 1;
            }
            options["reverse_time"]=false;options["reverse_bands"]=false;graph.set_surface_settings(options);QTest::qWait(40);
            pixels=native->grabFramebuffer();const auto panel=native->property("slicePanelRect").toRectF();
            if(pixels.pixelColor(qRound((panel.right()-8)*dpr),qRound((panel.top()+8)*dpr))!=paper) {
                qCritical("Opaque slice overlay is hidden by the scene");return 1;
            }
            pixels.save(directory+QString("/slice-%1-%2.png").arg(selection).arg(light ? "light" : "dark"));
            const auto corner=panel.topLeft()+QPointF(2,2);
            QTest::mousePress(native,Qt::LeftButton,Qt::NoModifier,corner.toPoint());
            const QPointF moved=corner-QPointF(20,15);
            QMouseEvent resize(QEvent::MouseMove,moved,native->mapToGlobal(moved.toPoint()),
                               Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
            QApplication::sendEvent(native,&resize);
            QTest::mouseRelease(native,Qt::LeftButton,Qt::NoModifier,moved.toPoint());QTest::qWait(50);
            const auto enlarged=native->property("slicePanelRect").toRectF();
            const auto saved=graph.presentation_state();
            if(enlarged.width()<=panel.width() || enlarged.height()<=panel.height() ||
               saved["slice_size"].toArray().size()!=2) {qCritical("Slice resize failed");return 1;}
            graph.set_presentation_state(saved);QTest::qWait(40);
            if(native->property("slicePanelRect").toRectF()!=enlarged) {qCritical("Slice size restore failed");return 1;}

        }
        const auto uploads=native->property("geometryUploads");
        options["mesh_grid"]=true;options["mesh_color"]="#ff00ff";options["mesh_width"]=2;options["mesh_contrast"]=.5;
        graph.set_surface_settings(options);QTest::qWait(70);
        if(graph.camera_state()!=camera || native->property("geometryUploads")!=uploads ||
            native->property("meshActualWidth").toFloat()<=0) return 1;
    }
    // Exercise the camera event contract, independently of physical touchpad recognition.
    SpectrogramGraph cameraGraph;cameraGraph.initialize({240,180},{1200,900});cameraGraph.show();QTest::qWait(70);
    auto *cameraView=cameraGraph.findChild<QOpenGLWidget *>("spectrogramContainer");
    auto position=[&] {const auto a=cameraGraph.camera_state()["position"].toArray();return QVector3D(a[0].toDouble(),a[1].toDouble(),a[2].toDouble());};
    const auto before=position();const auto pose=cameraGraph.camera_state();
    QNativeGestureEvent orbit(Qt::PanNativeGesture,QPointingDevice::primaryPointingDevice(),2,{},{},{},0,{60,0});
    QApplication::sendEvent(cameraView,&orbit);
    if(std::abs(position().length()-before.length())>1e-5 || position()==before || cameraGraph.camera_state()["quaternion"]==pose["quaternion"]) {
        qCritical("Orbit must rotate pose while retaining radius");return 1;
    }
    const auto radius=position().length();
    QNativeGestureEvent pinch(Qt::ZoomNativeGesture,QPointingDevice::primaryPointingDevice(),2,{},{},{},.25,{});
    QApplication::sendEvent(cameraView,&pinch);
    if(std::abs(position().length()-radius/1.25)>1e-5) {qCritical("Pinch zoom contract failed");return 1;}
    app.setPalette(original);qInfo("Overlay PASS light/dark: age8 band4 pick, visible yellow marker pixels, opaque9-band/24-age slices, custom GPU stops, option changes retain camera/geometry");
    return 0;
}

static int checkSurfaceSettings(QApplication &app,const QString &directory) {
    if(!QDir().mkpath(directory)) return 1;
    SpectrogramGraph graph;graph.set_history_dimensions(8,3);
    auto settings=SurfaceSettings::defaults();settings["future_option"]="preserve";
    graph.set_surface_settings(settings);
    graph.set_presentation_state({{"gradient",0},{"model3d",false},{"future_presentation",42}});
    if(graph.surface_settings()!=settings || graph.presentation_state()["future_presentation"]!=42) return 1;
    graph.initialize({240,180},{1200,900});graph.resize(1000,640);graph.show();QTest::qWait(100);
    auto *native=graph.findChild<QOpenGLWidget *>("spectrogramContainer");
    if(!native || !native->isValid() || !native->property("rendererReady").toBool()) return 1;
    auto stable=[&](int capacity,int valid,float first,float latest) {
        QTest::qWait(60);
        return native->property("historyCapacity").toInt()==capacity && native->property("historyBands").toInt()==3 &&
            native->property("vertexCount").toInt()==6*(capacity-1)*2 &&
            native->property("validHistoryColumns").toInt()==valid &&
            std::abs(native->property("firstDisplayedValue").toFloat()-first)<1e-5 &&
            std::abs(native->property("latestDisplayedValue").toFloat()-latest)<1e-5;
    };
    if(!stable(8,0,0,0)) {qCritical("Configured empty history did not retain a full zero surface");return 1;}
    const auto scale=native->property("sceneScaleZ");
    graph.update({{1.5f},{.8f},{.2f}});
    if(!stable(8,1,0,1.5f) || native->property("sceneScaleZ")!=scale) {qCritical("One-column history was not right aligned in fixed space");return 1;}
    std::vector<std::vector<float>> data(3,std::vector<float>(12));
    for(auto &row:data) for(int i=0;i<12;++i) row[i]=i*.1f;
    graph.update(data);
    if(!stable(8,8,.4f,1.1f) || native->property("sceneScaleZ")!=scale) return 1;
    graph.set_history_dimensions(16,3);
    if(!stable(16,12,0,1.1f)) return 1;
    graph.findChild<QSlider *>("axisMinX")->setValue(25);graph.findChild<QSlider *>("axisMaxX")->setValue(75);
    QTest::qWait(60);if(std::abs(native->property("sceneScaleZ").toFloat()-1.f)>1e-5f) return 1;
    graph.set_presentation_state({});graph.update({});
    if(!stable(16,0,0,0)) {qCritical("Clear reduced configured history geometry");return 1;}
    if(graph.findChild<QWidget *>("spectrogramModel3d") || graph.findChild<QWidget *>("spectrogramGradient")) return 1;
    qInfo("Fixed history 0/1/N/overflow/capacity-change/ranges/clear and central-only options passed");
    settings["background_mode"]="custom";settings["background_color"]="#223344";graph.set_surface_settings(settings);QTest::qWait(60);
    if(native->grabFramebuffer().pixelColor(2,2)!=QColor("#223344")) return 1;
    if(native->property("sceneWallColor").value<QColor>()!=QColor("#223344")) return 1;
    auto lightPalette=app.palette();lightPalette.setColor(QPalette::Window,QColor(245,245,245));app.setPalette(lightPalette);
    settings["background_mode"]="auto";graph.set_surface_settings(settings);QTest::qWait(60);
    if(native->grabFramebuffer().pixelColor(2,2)!=QColor(245,245,245)) {qCritical("Light auto background stayed black");return 1;}
    if(native->property("sceneWallColor").value<QColor>()!=QColor(245,245,245)) {qCritical("Light walls did not follow neutral scene background");return 1;}
    native->grabFramebuffer().save(directory+"/light-neutral.png");
    lightPalette.setColor(QPalette::Window,QColor(31,31,31));app.setPalette(lightPalette);
    settings=SurfaceSettings::defaults();settings["mesh_grid"]=false;graph.set_surface_settings(settings);
    graph.set_history_dimensions(96,32);data.assign(32,std::vector<float>(96));
    for(int b=0;b<32;++b) for(int t=0;t<96;++t) {
        const float x=t/95.f,z=b/31.f;
        data[b][t]=.1f+1.7f*std::exp(-40*((x-.35f)*(x-.35f)+(z-.6f)*(z-.6f)))+
            .9f*std::exp(-80*((x-.75f)*(x-.75f)+(z-.25f)*(z-.25f)));
    }
    graph.update(data);QTest::qWait(80);const auto off=native->grabFramebuffer();
    const auto uploads=native->property("geometryUploads");
    core::perf::enabled=true;settings["shadows"]=true;graph.set_surface_settings(settings);QTest::qWait(80);
    const auto on=native->grabFramebuffer();
    if(!native->property("shadowReady").toBool() || !native->property("shadowsActive").toBool() ||
        native->property("shadowPasses").toULongLong()==0 || native->property("geometryUploads")!=uploads) {
        qCritical("Real shadow-map pass is missing or rebuilt unchanged geometry");return 1;
    }
    int changes=0;for(int y=0;y<on.height();++y) for(int x=0;x<on.width();++x) if(on.pixel(x,y)!=off.pixel(x,y)) ++changes;
    if(changes<20) {qCritical("Enabled depth-map shadows did not affect visible surface (%d pixels)",changes);return 1;}
    off.save(directory+"/shadow-off.png");on.save(directory+"/shadow-on.png");
    qInfo("Real depth-map shadows: %d changed pixels, fixed geometry uploads",changes);
    settings["shadows"]=false;settings["light"]=false;settings["brightness"]=.5;graph.set_surface_settings(settings);QTest::qWait(60);
    if(native->property("shadowsActive").toBool() || graph.surface_settings()["brightness"]!=.5) return 1;
    auto saved=graph.surface_settings();SpectrogramGraph restored;restored.set_surface_settings(saved);
    if(restored.surface_settings()!=saved) return 1;
    if(!save_performance_report(directory+"/shadow-check.json")) return 1;
    qInfo("Surface options, light/custom/auto background, shadow toggle and state roundtrip passed");
    return 0;
}

static int profileRenderer(QApplication &, const QString &directory) {
    if(!QDir().mkpath(directory)) return 1;
    SpectrogramGraph graph;graph.set_history_dimensions(400,50);graph.initialize({240,180},{2000,1200});graph.show();
    auto *native=graph.findChild<QOpenGLWidget *>("spectrogramContainer");
    std::vector<std::vector<float>> data(50,std::vector<float>(400));
    for(int phase=0;phase<3;++phase) {
        const bool cameraOnly=phase==2;
        if(qEnvironmentVariableIntValue("QT_VIS_PROFILE_SHADOWS")) {
            auto settings=SurfaceSettings::defaults();settings["shadows"]=phase!=0;graph.set_surface_settings(settings);
        }
        graph.resize(phase==0 ? QSize(820,620) : QSize(1600,1000));QTest::qWait(200);
        core::perf::enabled=false;
        for(auto &counter:core::perf::counters) {
            counter.count=0;counter.total=0;counter.maximum=0;
            for(auto &bin:counter.histogram) bin=0;
        }
        core::perf::event_count=0;for(auto &event:core::perf::events) event.ready=false;
        const auto uploads=native->property("geometryUploads").toULongLong();
        const auto frames=native->property("renderedFrames").toULongLong();
        QElapsedTimer clock,heartbeat;clock.start();heartbeat.start();
        QEventLoop loop;QTimer update;update.setTimerType(Qt::PreciseTimer);update.setInterval(16);
        QObject::connect(&update,&QTimer::timeout,&loop,[&] {
            core::perf::duration(core::perf::Stage::GuiHeartbeat,heartbeat.nsecsElapsed());heartbeat.restart();
            const auto time=clock.elapsed()/1000.f;
            if(cameraOnly) {
                const auto q=QQuaternion::fromEulerAngles(-35,30+20*std::sin(time),0);
                const auto p=q.rotatedVector({0,0,5});
                graph.set_camera_state({{"version",2},{"position",QJsonArray{p.x(),p.y(),p.z()}},
                    {"quaternion",QJsonArray{q.scalar(),q.x(),q.y(),q.z()}}});
            } else {
                // Six changing spectral peaks, without audio or DSP. Same 50x400
                // history/band dimensions as the user's current config.
                std::array<std::array<float,6>,400> centers{};std::array<float,6> amplitudes{};
                for(int tone=0;tone<6;++tone) {
                    amplitudes[tone]=.1f+.07f*(1+std::sin(time+tone));
                    for(size_t t=0;t<400;++t) centers[t][tone]=4+tone*7+2*std::sin(time*.7f+tone+t*.008f);
                }
                for(size_t b=0;b<data.size();++b) for(size_t t=0;t<data[b].size();++t) {
                    float value=.05f;
                    for(int tone=0;tone<6;++tone) {
                        const float center=centers[t][tone];
                        value+=amplitudes[tone]*std::exp(-.5f*(b-center)*(b-center));
                    }
                    data[b][t]=value;
                }
                graph.update(data);
            }
        });
        core::perf::enabled=true;update.start();QTimer::singleShot(qEnvironmentVariableIntValue("QT_VIS_PROFILE_MS")>0 ? std::clamp(qEnvironmentVariableIntValue("QT_VIS_PROFILE_MS"),500,10000) : 10000,&loop,&QEventLoop::quit);loop.exec();update.stop();
        const auto seconds=clock.elapsed()/1000.;
        const auto rendered=native->property("renderedFrames").toULongLong()-frames;
        const auto uploaded=native->property("geometryUploads").toULongLong()-uploads;
        const QString name=phase==0 ? "small-changing" : phase==1 ? "large-changing" : "large-camera";
        qInfo("PHASE %s FPS=%.2f frames=%llu uploads=%llu size=%dx%d DPR=%.2f queue=1",
            qPrintable(name),rendered/seconds,rendered,uploaded,native->width(),native->height(),native->devicePixelRatioF());
        if(!save_performance_report(directory+"/"+name+".json",{QJsonObject{{"phase",name},{"fps",rendered/seconds},
            {"frames",double(rendered)},{"uploads",double(uploaded)},{"width",native->width()},
            {"height",native->height()},{"dpr",native->devicePixelRatioF()},{"queue_bound",1}}})) return 1;
        if(cameraOnly && uploaded) return 1;
    }
    return 0;
}

// Git baseline 93139d1, using its unchanged modifier and public native API.
// No pipeline, audio device, user configuration or playback is involved.
static int compareBaseline(QApplication &app, const QString &directory) {
    if (!QDir().mkpath(directory)) return 1;
    SpectrogramGraph current;
    current.initialize({240, 180}, {1200, 900});
    current.resize(1040, 640);
    current.show();
    QTest::qWait(150);
    auto *widget = current.findChild<QOpenGLWidget *>("spectrogramContainer");
    if (!widget || !widget->isValid() || !widget->property("rendererReady").toBool()) return 1;
    Q3DSurface baseline;
    QLabel selected;
    QSlider minX, maxX, minZ, maxZ;
    SpectrogramGraphModifier modifier(&baseline, &selected, &app);
    modifier.setAxisMinSliderX(&minX); modifier.setAxisMaxSliderX(&maxX);
    modifier.setAxisMinSliderZ(&minZ); modifier.setAxisMaxSliderZ(&maxZ);
    modifier.enable3DModel(); modifier.setGreenToRedGradient(); modifier.toggleModeItem();
    baseline.resize(widget->size());
    baseline.show();
    const QJsonObject pose{{"horizontal", -45}, {"vertical", 45}, {"zoom", 155},
                           {"target", QJsonArray{0, 0, 0}}};
    baseline.scene()->activeCamera()->setCameraPosition(-45, 45, 155);
    // Qt uses a 6-unit base distance and zoom scales the scene by a
    // viewport-dependent factor. Convert that scale to physical eye distance.
    const auto orientation = QQuaternion::fromEulerAngles(-45, 45, 0);
    const float distance = 6.f / (1.55f * std::min(1.f, .625f * widget->width() / widget->height()));
    const auto position = orientation.rotatedVector({0, 0, distance});
    current.set_camera_state({{"version", 2}, {"position", QJsonArray{position.x(), position.y(), position.z()}},
        {"quaternion", QJsonArray{orientation.scalar(), orientation.x(), orientation.y(), orientation.z()}}});
    QJsonArray datasets;
    for (const auto &name : {QString("plane"), QString("peaks"), QString("bands")}) {
        std::vector<std::vector<float>> data(32, std::vector<float>(96));
        for (size_t band = 0; band < data.size(); ++band)
            for (size_t frame = 0; frame < data[band].size(); ++frame) {
                const float x = float(frame) / 95, z = float(band) / 31;
                data[band][frame] = name == "plane" ? .2f + .5f * x + .3f * z :
                    name == "bands" ? .2f + 1.3f * std::pow(std::sin(z * 12), 2) :
                    .1f + 1.7f * std::exp(-40 * ((x-.35f)*(x-.35f)+(z-.6f)*(z-.6f))) +
                    .9f * std::exp(-80 * ((x-.75f)*(x-.75f)+(z-.25f)*(z-.25f)));
            }
        modifier.update(data); current.update(data);
        QTest::qWait(150);
        if (!baseline.renderToImage(0, widget->size()).save(directory + "/" + name + "-baseline.png") ||
            !widget->grabFramebuffer().save(directory + "/" + name + "-current.png")) return 1;
        datasets.append(name);
    }
    QJsonObject manifest{{"baseline", "93139d1 / Q3DSurface / unchanged modifier"}, {"camera", pose},
                         {"width", widget->width()}, {"height", widget->height()}, {"datasets", datasets},
                         {"note", "Same physical eye/orientation, viewport and 45-degree projection. Qt zoom scale converted to eye distance. Geometry, themes and axes use each renderer's own defaults."}};
    QFile file(directory + "/comparison.json");
    if (!file.open(QIODevice::WriteOnly)) return 1;
    file.write(QJsonDocument(manifest).toJson());
    qInfo("Silent native baseline comparison saved to %s", qPrintable(directory));
    return 0;
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    auto test_palette = app.palette();
    test_palette.setColor(QPalette::Window, QColor(31, 31, 31));
    test_palette.setColor(QPalette::Text, QColor(240, 240, 240));
    app.setPalette(test_palette);
    if (app.platformName() == "offscreen" || app.platformName() == "minimal") {
        qCritical("Native session required");
        return 2;
    }
    if (app.arguments().size() == 3 && app.arguments()[1] == "--compare-baseline")
        return compareBaseline(app, app.arguments()[2]);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--profile")
        return profileRenderer(app, app.arguments()[2]);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--surface-settings")
        return checkSurfaceSettings(app,app.arguments()[2]);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--axes")
        return checkHistoryAxes(app.arguments()[2]);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--selection")
        return checkSelectionOverlay(app,app.arguments()[2]);
    const int cycles=app.arguments().contains("--quick") ? 4 : 24;
    for (int iteration = 0; iteration < cycles; ++iteration) {
        auto window = std::make_unique<QMainWindow>();
        auto *dock = new QDockWidget("Spectrogram lifecycle", window.get());
        auto *graph = new SpectrogramGraph(dock);
        graph->set_camera_state({{"horizontal", 35},
                                 {"vertical", 60},
                                 {"zoom", 200},
                                 {"target", QJsonArray{0.2, -0.3, 0.4}},
                                 {"speed", 2.5}});
        const auto staged_camera = graph->camera_state();
        if (!graph->initialize({240, 180}, {800, 600})) {
            qCritical("Native check failed at source line %d", __LINE__);
            return 1;
        }
        dock->setWidget(graph);
        window->addDockWidget(Qt::LeftDockWidgetArea, dock);
        window->resize(800, 600);
        window->show();
        QTest::qWait(100);
        auto *native = graph->findChild<QOpenGLWidget *>("spectrogramContainer");
        if (!native || !native->isValid() || !native->property("rendererReady").toBool()) {
            qCritical("Native check failed at source line %d", __LINE__);
            return 1;
        }
        const auto initialized_camera = graph->camera_state();
        const auto staged_position = staged_camera["position"].toArray();
        const auto initialized_position = initialized_camera["position"].toArray();
        const auto staged_q = staged_camera["quaternion"].toArray();
        const auto initialized_q = initialized_camera["quaternion"].toArray();
        const QQuaternion expected_q(staged_q[0].toDouble(), staged_q[1].toDouble(), staged_q[2].toDouble(),
                                     staged_q[3].toDouble());
        const QQuaternion actual_q(initialized_q[0].toDouble(), initialized_q[1].toDouble(),
                                   initialized_q[2].toDouble(), initialized_q[3].toDouble());
        bool preserved = std::abs(QQuaternion::dotProduct(expected_q, actual_q)) > 1 - 1e-6f &&
                         initialized_camera["speed"] == staged_camera["speed"];
        for (int component = 0; component < 3; ++component)
            preserved = preserved && std::abs(staged_position[component].toDouble() -
                                              initialized_position[component].toDouble()) < 1e-6;
        if (!preserved) {
            qCritical("Migrated camera pose or speed changed during initialization/initializeGL");
            {
                qCritical("Native check failed at source line %d", __LINE__);
                return 1;
            }
        }
        if (iteration == 0) {
            const int full[4]{0,100,0,100}, crop[4]{25,75,0,100};
            SpectrogramSceneGeometry fullScene(96,32,full), cropScene(96,32,crop);
            auto near=[](QVector3D a,QVector3D b){ return (a-b).length()<1e-5f; };
            if (!near(fullScene.point(0,0,0),{2,-1,2.f/3}) ||
                !near(fullScene.point(96,32,2),{-2,1,-2.f/3}) ||
                !near(cropScene.point(48,16,1),{0,0,0}) ||
                std::abs(cropScene.scaleZ-4.f/3)>1e-5f) {
                qCritical("Qt baseline reversed axes/range/center/aspect numeric mapping failed");return 1;
            }
            graph->update({{1.6f,1.6f},{1.6f,1.6f}}); QTest::qWait(50);
            if(native->property("vertexCount").toInt()!=6 || native->property("wireframeEdgeCount").toInt()!=0) {
                qCritical("Plane geometry or user-requested crease-only edges failed");return 1;
            }
            qInfo("Qt-derived geometry mapping and crease-only flat plane passed");
        }
        if (iteration == 0)
            graph->reset_camera();
        std::vector<std::vector<float>> data(32, std::vector<float>(120));
        for (size_t b = 0; b < data.size(); ++b)
            for (size_t t = 0; t < data[b].size(); ++t)
                data[b][t] = 0.5f + 0.25f * std::sin(b * .1 + t * .2);
        const auto before_surface = native->property("geometryUploads").toULongLong();
        graph->update(data);
        for (int wait = 0; wait < 40 && native->property("geometryUploads").toULongLong() == before_surface; ++wait)
            QTest::qWait(5);
        if (native->property("surfaceMaximumHeight").toFloat() < -0.26f ||
            native->property("surfaceMaximumHeight").toFloat() > -0.24f) {
            qCritical("Normalized spectrum amplitudes failed: height=%g vertices=%d frames=%llu",
                      native->property("surfaceMaximumHeight").toDouble(), native->property("vertexCount").toInt(),
                      native->property("renderedFrames").toULongLong());
            {
                qCritical("Native check failed at source line %d", __LINE__);
                return 1;
            }
        }
        if (iteration == 0)
            QTest::qWait(1200);
        const auto rendered_surface = native->grabFramebuffer();
        if (rendered_surface.isNull()) {
            qCritical("Native check failed at source line %d", __LINE__);
            return 1;
        }
        int colored_pixels = 0;
        for (int y = 0; y < rendered_surface.height(); y += 3)
            for (int x = 0; x < rendered_surface.width(); x += 3) {
                const auto c = rendered_surface.pixelColor(x, y);
                if (std::max({c.red(), c.green(), c.blue()}) - std::min({c.red(), c.green(), c.blue()}) > 12 &&
                    std::max({c.red(), c.green(), c.blue()}) > 40)
                    ++colored_pixels;
            }
        if (colored_pixels < 100) {
            qCritical("GPU surface has geometry but no visible gradient pixels");
            {
                qCritical("Native check failed at source line %d", __LINE__);
                return 1;
            }
        }
        if (iteration == 0)
            rendered_surface.save(QCoreApplication::applicationDirPath() + "/../native-gradients-axes.png");
        if(iteration==0) {
            const auto top=QQuaternion::fromEulerAngles(-90,0,0);
            graph->set_camera_state({{"version",2},{"position",QJsonArray{0,4,0}},
                {"quaternion",QJsonArray{top.scalar(),top.x(),top.y(),top.z()}}});
            QMatrix4x4 projection,view;projection.perspective(45,float(native->width())/native->height(),.005f,100);
            view.rotate(top.conjugated());view.translate(0,-4,0);
            const int full[4]{0,100,0,100};SpectrogramSceneGeometry scene(120,32,full);
            const auto p=projection*view*QVector4D(scene.point(60,16,data[16][60]),1);
            QTest::mouseClick(native,Qt::LeftButton,Qt::NoModifier,
                QPoint(qRound((p.x()/p.w()+1)*native->width()/2),qRound((1-p.y()/p.w())*native->height()/2)));
            auto *mode=graph->findChild<QComboBox *>("spectrogramSelection");
            mode->setCurrentIndex(2);QTest::qWait(50);
            if(native->property("sliceSampleCount").toInt()!=32) {qCritical("Time slice did not contain all bands");return 1;}
            mode->setCurrentIndex(3);QTest::qWait(50);
            if(native->property("sliceSampleCount").toInt()!=120) {qCritical("Frequency slice did not contain all history frames");return 1;}
            native->grabFramebuffer().save(QCoreApplication::applicationDirPath()+"/../native-restored-slice.png");
            mode->setCurrentIndex(0);QTest::qWait(40);
            if(native->property("sliceSampleCount").toInt()!=0) return 1;
            qInfo("Selected native cross-sections: 32 bands / 120 history frames passed");
        }
        for (auto *slider : graph->findChildren<QSlider *>()) {
            slider->setValue(slider->maximum());
            slider->setValue(slider->minimum());
        }
        graph->update({{1, 2}, {3}});
        graph->update({{std::numeric_limits<float>::quiet_NaN(), 0}, {0, 0}});
        graph->update({});
        graph->update(data);
        graph->set_camera_state(
            {{"version", 2}, {"position", QJsonArray{0, -1, 0}}, {"quaternion", QJsonArray{1, 0, 0, 0}}});
        if (graph->camera_state()["position"].toArray()[1].toDouble() != -1) {
            qCritical("Native check failed at source line %d", __LINE__);
            return 1;
        }
        QTest::qWait(40);
        const auto uploads = native->property("geometryUploads").toULongLong();
        native->setFocus();
        QTest::keyPress(native, Qt::Key_W);
        for (int wait = 0; wait < 40 && graph->camera_state()["position"].toArray()[2].toDouble() >= 0; ++wait)
            QTest::qWait(5);
        QTest::keyRelease(native, Qt::Key_W);
        if (graph->camera_state()["position"].toArray()[2].toDouble() >= 0) {
            qCritical("Native check failed at source line %d", __LINE__);
            return 1;
        }
        if (native->property("geometryUploads").toULongLong() != uploads) {
            qCritical("Camera rebuilt data geometry");
            {
                qCritical("Native check failed at source line %d", __LINE__);
                return 1;
            }
        }
        // Keyboard translation follows horizontal heading, ignoring camera pitch.
        graph->set_camera_state({{"version",2},{"position",QJsonArray{0,0,0}},
            {"quaternion",QJsonArray{.70710678,0,.70710678,0}}});
        QTest::keyPress(native,Qt::Key_W);QTest::qWait(60);QTest::keyRelease(native,Qt::Key_W);
        auto moved=graph->camera_state()["position"].toArray();
        if(moved[0].toDouble()>=0 || std::abs(moved[1].toDouble())>1e-6 || std::abs(moved[2].toDouble())>1e-5)return 1;
        const auto beforeCtrl=graph->camera_state();
        QTest::keyPress(native,Qt::Key_Control);QTest::qWait(40);QTest::keyRelease(native,Qt::Key_Control);
        if(graph->camera_state()!=beforeCtrl)return 1;
        QTest::keyPress(native,Qt::Key_Space);QTest::qWait(60);
        const auto raised=graph->camera_state()["position"].toArray()[1].toDouble();
        QTest::keyPress(native,Qt::Key_Control);QTest::qWait(60);
        const auto lowered=graph->camera_state()["position"].toArray()[1].toDouble();
        QTest::keyRelease(native,Qt::Key_Control);QTest::qWait(60);
        QTest::keyRelease(native,Qt::Key_Space);
        if(raised<=0 || lowered>=raised || graph->camera_state()["position"].toArray()[1].toDouble()<=lowered)return 1;
        for(const bool shiftFirst:{true,false}) {
            graph->set_camera_state({{"version",2},{"position",QJsonArray{0,0,0}},
                {"quaternion",QJsonArray{1,0,0,0}}});
            if(shiftFirst)QTest::keyPress(native,Qt::Key_Shift);
            QTest::keyPress(native,Qt::Key_Control);
            QTest::keyPress(native,Qt::Key_Space,Qt::ControlModifier);
            if(!shiftFirst)QTest::keyPress(native,Qt::Key_Shift,Qt::ControlModifier);
            QTest::qWait(80);
            const auto fastY=graph->camera_state()["position"].toArray()[1].toDouble();
            QTest::keyRelease(native,Qt::Key_Shift,Qt::ControlModifier);QTest::qWait(80);
            const auto slowY=graph->camera_state()["position"].toArray()[1].toDouble();
            QTest::keyRelease(native,Qt::Key_Space,Qt::ControlModifier);QTest::keyRelease(native,Qt::Key_Control);
            if(fastY>=0 || slowY>=fastY || std::abs(fastY)<1.5*std::abs(slowY-fastY))return 1;
        }
        const auto before_float = native->property("renderedFrames").toULongLong();
        dock->setFloating(true);
        QTest::qWait(60);
        if (!native->isValid() || !native->property("rendererReady").toBool()) {
            qCritical("Native check failed at source line %d", __LINE__);
            return 1;
        }
        if (native->property("renderedFrames").toULongLong() <= before_float) {
            qCritical("Reparented renderer stopped painting after context recreation");
            {
                qCritical("Native check failed at source line %d", __LINE__);
                return 1;
            }
        }
        const auto before_dock = native->property("renderedFrames").toULongLong();
        dock->setFloating(false);
        QTest::qWait(60);
        if (!native->isValid() || !native->property("rendererReady").toBool()) {
            qCritical("Native check failed at source line %d", __LINE__);
            return 1;
        }
        if (native->property("renderedFrames").toULongLong() <= before_dock) {
            qCritical("Native check failed at source line %d", __LINE__);
            return 1;
        }
        window->resize(828, 628);
        QTest::qWait(35);
        if (native->grabFramebuffer().isNull()) {
            qCritical("Native check failed at source line %d", __LINE__);
            return 1;
        }
        QPointer<QOpenGLWidget> owned = native;
        graph->update(data);
        window.reset();
        if (owned) {
            qCritical("GL widget survived owner");
            {
                qCritical("Native check failed at source line %d", __LINE__);
                return 1;
            }
        }
        QTest::qWait(20);
    }
    qInfo("Native GL renderer: finite/ragged/empty snapshots, ranges, below/through camera, dock/float context and "
          "teardown: %d cycles passed",cycles);
    return 0;
}
