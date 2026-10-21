#include <core_app/plot_composition.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <barelief/numeric_piano_view.h>
#include "core_app/graph_panel.h"
#include "barelief/charts/freq.h"
#include "barelief/spectrograms/spectrogram_graph.h"
#include "barelief/axis_control_widget.h"
#include "barelief/ui_text.h"
#include "barelief/amplitude_range.h"
#include "barelief/auto_range_interval_control.h"
#include "barelief/surface_settings.h"
#include "barelief/spectrograms/spectrogram_selection.h"
#include "barelief/numeric_image_view.h"
#include "barelief/numeric_pie_view.h"
#include "barelief/numeric_metrics_view.h"
#include <QComboBox>
#include <QAbstractItemModel>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <barelief/quick/quick_ui.h>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QThread>
#include <QWheelEvent>
#include <QtTest/QTest>
#include <QMouseEvent>
#include <barelief/plot_viewport.h>
#include <algorithm>
#include <array>
#include <functional>
#include <cstdio>

class TimedSpectrogram final : public SpectrogramGraph {
public:
    std::int64_t now=0;
protected:
    std::int64_t amplitude_clock() const override {return now;}
};

static bool image_navigation(const auto& check,const auto& wait) {
    NumericImageView image;image.resize(600,400);image.show();image.set_matrix({{0,1,2},{3,4,5},{6,7,8}});
    SpectrogramAxisMetadata metadata;metadata.columns={100,200,300};metadata.ages={0,1,2};metadata.column_coordinates=true;metadata.time_coordinates=true;
    image.set_axis_metadata(metadata);
    const auto identity=image.projection_storage_identity();const auto raster=image.numeric_raster();
    image.presentation_model()->navigate("zoom",.25,.75,.5);
    const auto crop=image.viewport();
    if(!check(crop==QRectF(.125,.375,.5,.5),"image crop differs from cursor anchored zoom"))return false;
    if(!check(image.presentation_model()->state().value("viewTop").toDouble()==.375 && image.projection_storage_identity()==identity && image.numeric_raster()==raster,"navigation rebuilt/mutated matrix or failed to publish crop"))return false;
    if(!check(wait([&]{auto* raster=image.rootObject()->findChild<QQuickItem*>("numericRaster");return raster && raster->width()>image.width();}),"QML raster did not apply viewport scale"))return false;
    const auto ticks=image.presentation_model()->axes().value("xTicks").toList();
    if(!check(!ticks.empty() && ticks.front().toMap().value("text").toString()=="125" && ticks.back().toMap().value("text").toString()=="225","image frequency labels do not follow crop"))return false;
    image.set_reverse_axes(true,true);const auto beforePan=image.viewport();
    image.presentation_model()->navigate("pan",.1,.1);
    if(!check(image.viewport().left()>beforePan.left() && image.viewport().top()>beforePan.top(),"reversed image pan"))return false;
    image.set_reverse_axes(false,false);image.presentation_model()->navigate("reset",0,0);image.presentation_model()->navigate("zoom",.25,.75,.5);
    image.set_matrix({{1,2,3},{4,5,6},{7,8,9}});
    if(!check(image.viewport()==crop,"image frame reset navigation"))return false;
    for(int i=0;i<100;++i)image.presentation_model()->navigate("zoom",0,0,.1);
    if(!check(image.viewport().width()>=1e-6,"unbounded zoom"))return false;
    image.presentation_model()->navigate("reset",0,0);
    if(!check(image.viewport()==QRectF(0,0,1,1),"image reset"))return false;
return true;
}

static bool vector_drag_navigation(LineChartView& chart,const QPointF& cursor,const QRectF& v,const auto& check,const auto& wait) {
    // Exercise real QML drag dispatch, including the mouse grab beyond the press.
    QMouseEvent press(QEvent::MouseButtonPress,cursor,cursor,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QCoreApplication::sendEvent(chart.quickWindow(),&press);
    const QPointF dragged=cursor+QPointF(10,10);
    QMouseEvent move(QEvent::MouseMove,dragged,dragged,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QCoreApplication::sendEvent(chart.quickWindow(),&move);
    QMouseEvent release(QEvent::MouseButtonRelease,dragged,dragged,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::sendEvent(chart.quickWindow(),&release);
    if(!check(chart.viewport().left()<v.left() && chart.viewport().top()>v.top(),"actual QML drag did not pan vector"))return false;
    QTest::mouseDClick(chart.quickWindow(),Qt::LeftButton,Qt::NoModifier,cursor.toPoint());
    if(!check(chart.viewport()==QRectF(0,0,1,1),"actual QML double click did not reset"))return false;
    chart.presentation_model()->navigate("zoom",.5,.5,.5,1);
    if(!check(chart.viewport().width()==.5 && chart.viewport().height()==1,"X only zoom"))return false;
    chart.set_reverse_axes(true,true);wait([&]{return chart.presentation_model()->state().value("reverseX").toBool();});
    chart.presentation_model()->navigate("zoom",.25,.25,.5,3);
    const auto reversed=chart.viewport();chart.presentation_model()->navigate("pan",.1,.1);
    if(!check(chart.viewport().left()>reversed.left() && chart.viewport().top()<reversed.top(),"reversed vector pan"))return false;
    chart.presentation_model()->navigate("zoom",NAN,0,.5);
    if(!check(std::isfinite(chart.viewport().left()),"nonfinite navigation"))return false;
return true;
}

static bool plot_navigation_oracles() {
    const auto check=[](bool ok,const char* message){if(!ok)std::fprintf(stderr,"Navigation oracle: %s\n",message);return ok;};
    const auto wait=[](auto ready){QElapsedTimer timer;timer.start();while(timer.elapsed()<1500){QCoreApplication::processEvents();if(ready())return true;QThread::msleep(3);}return ready();};
    LineChartView chart;chart.resize(600,400);chart.set_range(-10,10);chart.update({0,1,2,3,4,5,6,7,8,9,10});chart.show();
    if(!check(wait([&]{return chart.presentation_model()->state().value("kind")=="vector";}),"QML plot unavailable"))return false;
    const auto state=chart.presentation_model()->state();
    const double left=state.value("plotLeft").toDouble();
    const QPointF cursor(left+(chart.width()-left-22)*.25,28+(chart.height()-86)*.25);
    QWheelEvent wheel(cursor,chart.quickWindow()->mapToGlobal(cursor.toPoint()),{},QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QCoreApplication::sendEvent(chart.quickWindow(),&wheel);
    const auto v=chart.viewport();
    if(!check(v.width()<1 && std::abs(v.left()+.25*v.width()-.25)<1e-8 && std::abs(v.top()+.75*v.height()-.75)<1e-8,"actual QML wheel lost cursor anchor / vector Y direction"))return false;
    const auto baseX=chart.x_range(),baseY=chart.y_range();
    chart.update({1,2,3,4,5,6,7,8,9,10,0});
    if(!check(chart.viewport()==v && chart.x_range()==baseX && chart.y_range()==baseY,"incoming frame or navigation altered configured ranges"))return false;
    if(!vector_drag_navigation(chart,cursor,v,check,wait))return false;
    if(!image_navigation(check,wait))return false;
    return true;
}

static bool matrix_value_scales(const auto& check) {
    // 2D/3D share source metadata and preserve the immutable source history.
    auto* placeholder=new LineChartView;GraphPanel history("spectrogram",placeholder,{{"kind","SpectrogramView"},{"display_mode","image2d"}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"image2d","surface3d"},{},true}));
    auto imageTarget=history.all_render_targets();SpectrogramAxisMetadata metadata;metadata.value_scale.factor=10;
    imageTarget.image->set_axis_metadata(metadata);imageTarget.image->set_matrix({{1,.1f},{.01f,0}});
    history.changePreference("db_factor",20);history.changePreference("db_factor",0);
    if(!check(history.image_view()->axis_metadata().value_scale.effective_factor()==10,"factor override erased source quantity"))return false;
    const auto identity=history.image_view()->projection_storage_identity();
    history.changePreference("value_scale","db");
    float raw=0;history.image_view()->matrix_sample(0,1,raw);
    if(!check(identity==history.image_view()->projection_storage_identity() && std::abs(raw-.1)<1e-6 && history.image_view()->axis_metadata().value_scale.map(raw)<-9.99,"2D scale copied source or ignored power metadata"))return false;
    SpectrogramGraph surface;metadata.value_scale.mode=ValueDisplayScale::Mode::Decibels;surface.set_axis_metadata(metadata);surface.update_frame_history({{1,.1f},{.01f,0}});
    if(!check(surface.projected_axis_metadata().value_scale.map(.1)==-10 && surface.projected_history_sample(0,1,raw) && std::abs(raw-.1)<1e-6,"3D scale changed raw history"))return false;
    std::vector<std::vector<float>> db{{-120,-20,-80}};
    const auto peak=spectrogramPeakSample(db,0,1,0,3,3,true);
    if(!check(peak.value==-20,"dB surface reduction chooses silence over spectral peak"))return false;
return true;
}

static bool chart_density_and_value_scale(GraphPanel& panel,FreqChartView* chart,auto& target,const auto& check,const auto& wait) {
    panel.changePreference("grid_lines_y",9);panel.changePreference("axis_labels_x",5);panel.changePreference("axis_labels_y",7);
    panel.changePreference("value_scale","db");
    if(!check(wait([&]{const auto axes=chart->presentation_model()->axes();return axes.value("xTicks").toList().size()==5 && axes.value("yTicks").toList().size()==7 && axes.value("xGrid").toList().size()==17 && axes.value("yGrid").toList().size()==9;}),"manual density not rendered"))return false;
    auto axes=chart->presentation_model()->axes();
    if(!check(axes.value("xGrid").toList().size()==17 && axes.value("yGrid").toList().size()==9 && axes.value("yTicks").toList().size()==7,"grid and label counts are coupled"))return false;
    if(!check(wait([&]{auto p=chart->projected_points();return p.size()==3 && std::abs(p[1].y()+20)<1e-4 && p[2].y()==-120;}),"chart dB projection not refreshed without new source frame"))return false;
    const auto prefs=QJsonObject::fromVariantMap(panel.preferences());
    panel.restore_presentation(prefs);if(!check(panel.preferences().value("grid_lines_x").toInt()==17 && panel.preferences().value("value_scale")=="db","settings lost on restore"))return false;
    panel.changePreference("axis_labels_x",-1);panel.changePreference("grid_lines_x",12);
    if(!check(wait([&]{return chart->presentation_model()->axes().value("xTicks").toList().empty() && chart->presentation_model()->axes().value("xGrid").toList().size()==12;}),"hidden labels hide grid"))return false;
    panel.changePreference("axis_labels_x",129);panel.changePreference("grid_lines_x",1.5);panel.changePreference("value_scale","invalid");
    if(!check(panel.preferences().value("axis_labels_x").toInt()==-1 && panel.preferences().value("grid_lines_x").toInt()==12 && panel.preferences().value("value_scale")=="db","invalid preferences accepted"))return false;
    panel.changePreference("value_scale","linear");
    if(!check(wait([&]{auto p=chart->projected_points();return p.size()==3 && std::abs(p[1].y()-.1)<1e-6 && p[2].y()==0;}),"switching scale did not retain original vector"))return false;
    target.chart->set_value_source(true,10);target.chart->set_axis_title_keys("Frequency, Hz (X)","Level, dB (Y)");target.chart->update({0,-10,-20});
    if(!check(wait([&]{auto p=chart->projected_points();return p.size()==3 && std::abs(p[1].y()-.1)<1e-6;}),"source power dB inverse uses wrong factor"))return false;
return true;
}

static bool qml_density_controls(GraphPanel& panel,QQuickWidget* settings,const auto& findItem,const auto& check,const auto& wait) {
    QQuickItem* axisMenu=nullptr;
    wait([&]{axisMenu=findItem(settings->rootObject(),"axisMenuX");return axisMenu!=nullptr;});
    if(!check(axisMenu && !settings->rootObject()->property("axisXOpen").toBool() && !settings->rootObject()->property("axisYOpen").toBool() && !settings->rootObject()->property("scaleOpen").toBool(),"settings menus must start closed"))return false;
    auto* closedContent=findItem(settings->rootObject(),"axisSettingsX");
    if(!check(closedContent && !closedContent->isVisible() && QMetaObject::invokeMethod(axisMenu,"clicked"),"axis menu did not open on click"))return false;
    QQuickItem* gridField=nullptr;
    wait([&]{gridField=findItem(settings->rootObject(),"grid_lines_x");return gridField!=nullptr;});
    if(!check(gridField && gridField->setProperty("text","17") && QMetaObject::invokeMethod(gridField,"editingFinished"),"QML density editor unavailable"))return false;
    if(!check(panel.preferences().value("grid_lines_x").toInt()==17,"QML density edit did not reach preferences"))return false;
    if(!check(settings->rootObject()->property("axisXOpen").toBool() && !settings->rootObject()->property("axisYOpen").toBool(),"editing reset menu expansion or opened the other axis"))return false;
    axisMenu=findItem(settings->rootObject(),"axisMenuX");
    if(!check(axisMenu && QMetaObject::invokeMethod(axisMenu,"clicked") && !settings->rootObject()->property("axisXOpen").toBool(),"axis menu did not close on second click"))return false;

return true;
}

static bool numeric_value_scales(const auto& check) {
    ValueDisplayScale scale;scale.mode=ValueDisplayScale::Mode::Decibels;
    if(!check(std::abs(scale.map(.1)+20)<1e-10 && scale.map(0)==-120 && std::isfinite(scale.map(NAN)),"amplitude dB / silence"))return false;
    scale.factor=10;if(!check(std::abs(scale.map(.1)+10)<1e-10,"power dB"))return false;
    scale.source_decibels=true;if(!check(scale.map(-30)==-30 && scale.map(-200)==-120,"double log / floor of dB source"))return false;
    scale.mode=ValueDisplayScale::Mode::Linear;scale.reference=2;
    if(!check(std::abs(scale.map(-10)-.2)<1e-10,"inverse power dB reference"))return false;
return true;
}

static bool waveform_density(auto& waveform,int density,const auto& samples,const auto& check,const auto& wait) {

        waveform.set_axis_density(density,density,7,9);
        if(!check(wait([&]{return waveform.presentation_model()->vectorPoints().size()>100;}),"full PCM projection unavailable"))return false;
        waveform.publish_presentation();
        const auto projected=waveform.projected_points();const auto normalized=waveform.presentation_model()->vectorPoints();
        if(!check(projected.size()==normalized.size(),"axis change lost projected points"))return false;
        bool active[3]{};
        for(qsizetype j=0;j<projected.size();++j){const auto index=std::size_t(std::llround(projected[j].x()));
            if(!check(index<samples.size() && projected[j].y()==samples[index] &&
                std::abs(normalized[j].x()-double(index)/4095)<1e-12 &&
                std::abs(normalized[j].y()-(1-double(samples[index]))/2)<1e-12,"axis change altered PCM values/coordinates"))return false;
            if(std::abs(samples[index])>.01)active[std::min(std::size_t{2},index*3/samples.size())]=true;
        }
        if(!check(active[0]&&active[1]&&active[2],"waveform flattened in part of its window"))return false;return true;
}

static bool panel_density_scale(const auto& check,const auto& wait) {
    auto* chart=new FreqChartView;GraphPanel panel("fft",chart,{{"kind","FFTView"}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"line","points","bars","pie"},{},true}));panel.resize(900,500);panel.show();
    auto target=panel.all_render_targets();target.chart->set_value_source(false,20);target.chart->set_axis_title_keys("Frequency, Hz (X)","Amplitude (Y)");target.chart->update({1,.1f,0});
    auto* settings=panel.findChild<QQuickWidget*>("numericGraphSettings");
    std::function<QQuickItem*(QQuickItem*,const QString&)> findItem=[&](QQuickItem* root,const QString& name)->QQuickItem*{
        if(root->objectName()==name)return root;const auto children=root->childItems();for(auto* child:children)if(auto* found=findItem(child,name))return found;return nullptr;
    };
    if(!qml_density_controls(panel,settings,findItem,check,wait))return false;
    if(!chart_density_and_value_scale(panel,chart,target,check,wait))return false;
    if(!matrix_value_scales(check))return false;
    return true;
}
static bool density_scale_oracles() {
    const auto check=[](bool ok,const char* message){if(!ok)std::fprintf(stderr,"Density/scale oracle: %s\n",message);return ok;};
    const auto wait=[](auto ready){QElapsedTimer timer;timer.start();while(timer.elapsed()<1500){QCoreApplication::processEvents();if(ready())return true;QThread::msleep(3);}return ready();};
    // A complete rolling PCM window must retain its waveform across all axis
    // density settings; labels/grid never become data or zero-fill a prefix.
    class Waveform final:public LineChartView {public:using AbstractChartView::publish_presentation;};
    Waveform waveform;waveform.resize(1200,400);waveform.set_range(-1,1);waveform.show();
    std::vector<float> samples(4096);
    for(std::size_t i=0;i<samples.size();++i)for(unsigned tone=0;tone<8;++tone)
        samples[i]+=float(.07*std::sin(double(i)*(tone+1)*.031));
    waveform.update(samples);
    for(const auto density:{0,1,17,128,-1}){
        if(!waveform_density(waveform,density,samples,check,wait))return false;
    }
    if(!numeric_value_scales(check))return false;
    if(!panel_density_scale(check,wait))return false;
    return true;
}

static bool renderer_check(bool value,const char* message) {
    if(!value)std::fprintf(stderr,"Renderer oracle: %s\n",message);
    return value;
}
static bool plot_wait(const std::function<bool()>& ready) {
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<1500){QCoreApplication::processEvents();if(ready())return true;QThread::msleep(3);}
    return ready();
}
static bool camera_translation_oracles() {
    // Retained runtime_tests camera speed oracle, with no GL initialization.
    for(float ratio:{.125f,1.f,8.f})for(float yaw:{0.f,.785398f,1.570796f}) {
        const auto step=SpectrogramGraph::camera_translation(yaw,1,0,ratio,2);
        const QVector3D world(step.x()*std::min(1.f,ratio),step.y()/2,step.z()*std::min(1.f,1.f/ratio));
        if(!renderer_check(std::abs(world.length()-1.f)<1e-5,"camera speed varies with rotation/aspect"))return false;
    }
    return true;
}
class LineProjectionScenario {
    LineChartView line;
    bool initialize() {
line.set_range(-1,1);line.set_auto_resizing(true);line.update({});
    line.resize(400,300);line.show();
        return true;
    }
    bool dense_extrema() {
    std::vector<float> dense(10000,0);dense[5051]=42;dense[5052]=-17;line.update(dense);
    if(!renderer_check(plot_wait([&]{const auto points=line.projected_points();return std::any_of(points.begin(),points.end(),[](auto p){return p.y()==42;});}),"dense line was not projected"))return false;
    const auto points=line.projected_points();
    const auto extrema=std::minmax_element(points.begin(),points.end(),[](auto a,auto b){return a.y()<b.y();});
    if(!renderer_check(points.size()<1000 && extrema.first->y()==-17 && extrema.second->y()==42,"line pixel aggregation lost spikes or paints an unbounded series"))return false;
    if(!renderer_check(line.x_range().second==9999 && line.y_range().first==-17 && line.y_range().second==42,"line automatic sample/amplitude bounds"))return false;
        return true;
    }
    bool empty_and_scalar() {
    line.update({});if(!renderer_check(plot_wait([&]{return line.projected_point_count()==0;}),"empty line retains stale points"))return false;
    line.set_auto_gain(true,.001f,-.001f);line.set_range(-.001,.001);
    line.set_scalar_readout("0.49",true);line.set_x_coordinates({0});line.update({.49f});
    if(!renderer_check(line.x_range().first==-1 && line.x_range().second==0,"singleton timed scalar axis extends into future time"))return false;
    if(!renderer_check(plot_wait([&]{return line.projected_point_count()==1;}),"singleton scalar has no visible point"))return false;
        return true;
    }
    bool held_peak() {
    const auto heldMaximum=line.y_range().second;
    for(int i=0;i<100;++i)line.update({i%2?.48f:.49f});
    if(!renderer_check(line.y_range().second==heldMaximum,"automatic amplitude jitters within a quantized band"))return false;
    line.update({1.7f});
    if(!renderer_check(line.y_range().second==heldMaximum,"fresh peak changed amplitude bounds inside the configured hold interval"))return false;
        return true;
    }
    bool quiet_and_manual() {
    const auto peakMaximum=line.y_range().second;
    for(int i=0;i<100;++i)line.update({.03f,.03f,.03f});
    if(!renderer_check(line.y_range().second==peakMaximum && plot_wait([&]{return line.projected_point_count()==3;}),"quiet history shrinks immediately or constant scalar line is empty"))return false;
    const auto projectedPoints=line.projected_points();for(auto point:projectedPoints)if(!renderer_check(std::abs(point.y()-.03)<1e-6,"constant scalar line changed value"))return false;
    line.set_auto_resizing(false);line.set_range(-2,2);line.update({100});
    if(!renderer_check(line.y_range().first==-2 && line.y_range().second==2,"manual amplitude range was overwritten"))return false;
        return true;
    }
    bool theme_inheritance() {
    const auto theme=ui::Theme::instance().code();
    for(const auto* mode:{"light","dark"}) {
        ui::Theme::instance().set(mode);QCoreApplication::processEvents();
        ui::quick::UiBridge bridge;
        const auto colors=bridge.palette();
        if(!renderer_check(colors.value("base").value<QColor>()==line.palette().color(QPalette::Base) &&
            colors.value("highlight").value<QColor>()==line.palette().color(QPalette::Highlight) &&
            colors.value("text").value<QColor>()==line.palette().color(QPalette::Text),"QML chart does not inherit semantic theme styling"))return false;
    }
    ui::Theme::instance().set(theme);line.hide();
        return true;
    }
public:
    bool run() {return initialize()&&dense_extrema()&&empty_and_scalar()&&held_peak()&&quiet_and_manual()&&theme_inheritance();}
};
class FrequencyProjectionScenario {
    FreqChartView frequency;
    bool hasPeak(qreal hz) const {
        const auto samples=frequency.projected_points();
        return std::any_of(samples.begin(),samples.end(),[hz](auto point){return point.x()==hz && std::abs(point.y()-.8)<1e-6;});
    }
    bool initialize_and_peak() {
frequency.update_freq_step(10);frequency.update(std::vector<float>(100,0));
    if(!renderer_check(frequency.x_range().second==990,"frequency automatic Hz bounds do not follow data size"))return false;
    frequency.resize(320,240);frequency.set_range(0,1);frequency.show();
    std::vector<float> narrow(2048,0);narrow[1377]=.8f;frequency.update(narrow);
    frequency.set_plot_style("line");
    if(!renderer_check(plot_wait([&]{return hasPeak(13770);}) && frequency.projected_point_count()<2048,"FFT pixel aggregation lost narrow peak/Hz position"))return false;
        return true;
    }
    bool resize_and_step() {
    const auto initial=frequency.projected_point_count();frequency.resize(1100,600);
    if(!renderer_check(plot_wait([&]{return hasPeak(13770) && frequency.projected_point_count()>initial;}),"resize did not rebuild FFT resolution"))return false;
    frequency.update_freq_step(20);
    if(!renderer_check(plot_wait([&]{return hasPeak(27540);}),"frequency-step change retained old Hz positions"))return false;
        return true;
    }
    bool stable_coordinates() {
    frequency.set_x_coordinates(std::vector<double>{70.1,70.2});frequency.update({.2f,.3f});
    int coordinateRangeChanges=0;
    const auto coordinateRange=frequency.x_range();QObject coordinateWitness;
    const auto connection=QObject::connect(frequency.presentation_model(),&QuickPlotModel::changed,&coordinateWitness,[&]{if(frequency.x_range()!=coordinateRange)++coordinateRangeChanges;});
    for(int i=0;i<20;++i){frequency.update_freq_step(1);frequency.set_x_coordinates({70.1,70.2});frequency.update({.3f,.4f});}
    QObject::disconnect(connection);
    if(!renderer_check(coordinateRangeChanges==0 && frequency.x_range()==coordinateRange && frequency.x_range().first==70.1 && frequency.x_range().second==70.2,"unchanged physical FFT axis emits transient index ranges"))return false;
        return true;
    }
    bool empty_clears() {
    frequency.update({});if(!renderer_check(plot_wait([&]{return frequency.projected_point_count()==0;}),"empty FFT retains stale peaks"))return false;
        return true;
    }
public:
    bool run() {return initialize_and_peak()&&resize_and_step()&&stable_coordinates()&&empty_clears();}
};
static bool retained_renderer_oracles() {
    LineProjectionScenario line;FrequencyProjectionScenario frequency;
    return camera_translation_oracles()&&line.run()&&frequency.run();
}
static bool coordinate_controls_ignore_titles() {
    GraphPanel panel("explicitCoordinates",new LineChartView,{},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"image2d","surface3d"},{},false}));
    SpectrogramAxisMetadata axes;
    axes.time_title_key="Custom acquisition coordinate";axes.column_title_key="Custom measurement coordinate";
    axes.time_coordinates=true;axes.column_coordinates=true;axes.ages={0,1};axes.columns={2,4};
    panel.all_render_targets().image->set_axis_metadata(axes);
    if(panel.physical_frequency() || panel.physical_time())return false;
    panel.set_axis_permissions({true,true});
    return panel.physical_frequency() && panel.physical_time();
}
static int failure(int code,int line) {std::fprintf(stderr,"GraphPanel axes failure: oracle %d at line %d\n",code,line);return code;}

static int nearest_point_oracles() {
    int projectedPoints=0;
    const auto planar=[&](int frame,int band){++projectedPoints;return SpectrogramProjectedSample{{double(frame),double(band)},0,true};};
    if(spectrogramNearestPoint({510,256},{0,510,0,256},planar)!=QPoint(510,256) || projectedPoints>18000)return failure(41,__LINE__);
    if(spectrogramNearestPoint({307.2,143.1},{30,470,20,230},planar)!=QPoint(307,143))return failure(42,__LINE__);
    if(spectrogramNearestPoint({0,0},{30,470,20,230},planar,10)!=QPoint(-1,-1))return failure(43,__LINE__);
    const auto reflected=[](int frame,int band){return SpectrogramProjectedSample{{510.-frame,256.-band},0,true};};
    if(spectrogramNearestPoint({20,7},{0,510,0,256},reflected)!=QPoint(490,249))return failure(44,__LINE__);
    const auto invisible=[](int,int){return SpectrogramProjectedSample{};};
    if(spectrogramNearestPoint({20,7},{0,510,0,256},invisible)!=QPoint(-1,-1))return failure(45,__LINE__);
    return 0;
}

static int selection_translation_oracles() {
    const auto selectionLanguage=ui::Language::instance().code();
    for(const auto* code:{"en","ru"}) {
        ui::Language::instance().set(code);
        const auto melRows=spectrogramSelectionRows("—","3","0.002",true,"Frequency, Mel bands (Z)","Amplitude (Y)");
        const auto hzRows=spectrogramSelectionRows("4","70.5","2",false,"Frequency, Hz (Z)","Value (Y)");
        if(melRows.size()!=3 || melRows[0]!=ui::text("Time delay (s): %1").arg("—") ||
           melRows[1]!=ui::text("Mel band")+": 3" || hzRows[0]!=ui::text("Temporal frame: %1").arg("4") ||
           hzRows[1]!=ui::text("Frequency (Hz)")+": 70.5" || hzRows[2]!=ui::text("Value")+": 2")return failure(46,__LINE__);
    }
    ui::Language::instance().set(selectionLanguage);
    return 0;
}

static int readout_layout_oracles(QApplication& app) {
    // Full localized labels and physical values survive a narrow viewport.
    // Wrapping may add visual lines, but no digit or character is elided.
    const QStringList fullReadout{QString::fromUtf8("\u0412\u043e\u0437\u0440\u0430\u0441\u0442 (\u0441): 123456789.0123456789"),
        "Frequency (Hz): 123456789.0123456789", "Amplitude: -1.23456789e-300"};
    const auto compactReadout=spectrogramReadoutLayout(fullReadout,app.font(),{150,180});
    if(compactReadout.lines.join("")!=fullReadout.join("") ||
       compactReadout.lines.size()<=3 || compactReadout.bounds.right()>150 ||
       compactReadout.bounds.bottom()>180)return failure(83,__LINE__);
    const auto normalReadout=spectrogramReadoutLayout({"Age: 0.25","Band: 49","Value: 0.012"},app.font(),{800,600});
    if(normalReadout.lines.size()!=3 || normalReadout.bounds.width()>200 || normalReadout.bounds.height()>80)return failure(84,__LINE__);
    return 0;
}

static int amplitude_hold_oracles() {
    // Simulated monotonic time verifies seconds, complete interval peaks and
    // strict hold of both growth and shrink without multi-second GUI sleeps.
    AmplitudeRange policy;
    if(policy.observe(0,0,0,5) || policy.range()!=std::pair<double,double>{0,1})return failure(20,__LINE__);
    if(!policy.observe(-1,2,100,5) || policy.range().first> -1.2 || policy.range().second<2.4)return failure(21,__LINE__);
    const auto initialBounds=policy.range();
    if(policy.observe(-8,10,1000,5) || policy.observe(0,.01,5099,5) || policy.range()!=initialBounds)return failure(22,__LINE__);
    if(!policy.observe(0,.02,5100,5) || policy.range().first> -9.6 || policy.range().second<12)return failure(23,__LINE__);
    const auto peaks=policy.range();
    if(policy.observe(0,.1,10099,5) || policy.range()!=peaks || !policy.observe(0,.1,10100,5) || policy.range().second>=peaks.second)return failure(24,__LINE__);
    if(AmplitudeRange::interval(0)!=1 || AmplitudeRange::interval(100)!=60 || AmplitudeRange::interval(std::numeric_limits<double>::quiet_NaN())!=10)return failure(25,__LINE__);
    return 0;
}

static int interval_control_oracles() {
    ui::AutoRangeIntervalControl intervalControl;
    int intervalWrites=0;
    QObject::connect(intervalControl.box,&QDoubleSpinBox::valueChanged,&intervalControl,[&]{++intervalWrites;});
    intervalControl.set_seconds(7.123456789);
    if(intervalWrites || intervalControl.seconds()!=7.123456789)return failure(26,__LINE__);
    intervalControl.box->setValue(8);
    if(intervalWrites!=1 || intervalControl.seconds()!=8)return failure(27,__LINE__);
    const auto savedLanguage=ui::Language::instance().code();
    for(const auto* code:{"en","ru"}) {
        ui::Language::instance().set(code);
        bool translated=false;
        const auto intervalLabels=intervalControl.findChildren<QLabel*>();for(auto* label:intervalLabels)
            translated|=label->text()==ui::text("Auto range interval (s)");
        if(!translated || intervalControl.seconds()!=8 || intervalWrites!=1)return failure(35,__LINE__);
    }
    ui::Language::instance().set(savedLanguage);
    return 0;
}

static int extreme_amplitude_oracles() {
    AmplitudeRange bounded;
    bounded.observe(-std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),0,10);
    if(bounded.range().first!=-double(std::numeric_limits<float>::max()) || bounded.range().second!=std::numeric_limits<float>::max())return failure(36,__LINE__);
    const int fullRanges[]{0,100,0,100};
    SpectrogramSceneGeometry wide(2,2,fullRanges,false,false,float(bounded.range().first),float(bounded.range().second));
    if(wide.valueFraction(0)!=.5f || wide.signalTick(0)!=-std::numeric_limits<float>::max() ||
       wide.signalTick(.5f)!=0 || wide.signalTick(1)!=std::numeric_limits<float>::max())return failure(37,__LINE__);
    const auto heldScene=spectrogramSceneGeometry(2,2,fullRanges,SurfaceSettings::defaults(),{0,.001f});
    const auto heldPoint=heldScene.point(0,0,std::numeric_limits<float>::max());
    if(!std::isfinite(heldScene.valueFraction(std::numeric_limits<float>::max())) || heldPoint.y()!=1)return failure(39,__LINE__);
    return 0;
}

static int piano_setting_oracles() {
    GraphPanel pianoSettings("piano",new FreqChartView(10),{{"kind","PianoView"}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"piano"},{},true}));
    if(!pianoSettings.piano_view() || pianoSettings.piano_view()->activity_floor()!=1e-9)return failure(60,__LINE__);
    pianoSettings.changePreference("piano_activity_floor",1e-6);
    if(pianoSettings.piano_view()->activity_floor()!=1e-6)return failure(61,__LINE__);
    pianoSettings.changePreference("piano_activity_floor",-1.);
    if(pianoSettings.piano_view()->activity_floor()!=1e-6)return failure(62,__LINE__);
    const auto pianoSaved=QJsonObject::fromVariantMap(pianoSettings.preferences());
    GraphPanel pianoRestored("piano",new FreqChartView(10),pianoSaved,nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"piano"},{},true}));
    if(!pianoRestored.piano_view() || pianoRestored.piano_view()->activity_floor()!=1e-6)return failure(63,__LINE__);
    pianoRestored.changePreference("piano_activity_floor",0.);
    if(pianoRestored.piano_view()->activity_floor()!=0)return failure(64,__LINE__);
    return 0;
}

static int decibel_source_oracles() {
    auto* dbChart=new FreqChartView;
    GraphPanel dbPanel("fft",dbChart,{{"auto_range",false},{"maximum",.1}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"line","points","bars","pie"},{},true}));
    int dbWrites=0;QObject::connect(&dbPanel,&GraphPanel::presentation_changed,&dbPanel,[&]{++dbWrites;});
    dbPanel.all_render_targets().chart->set_value_source(true,20);
    dbPanel.all_render_targets().chart->set_axis_title_keys("Frequency, Hz (X)","Level, dB (Y)");
    dbChart->update({-120,-30,-60});
    if(dbChart->y_range().first!=-120 || dbChart->y_range().second!=.1 || !dbChart->bar_baseline_minimum() || dbWrites)return failure(80,__LINE__);
    dbPanel.restore_presentation({{"auto_range",false},{"minimum",-90},{"maximum",-10}});
    if(dbChart->y_range().first!=-90 || dbChart->y_range().second!=-10)return failure(81,__LINE__);
    dbPanel.all_render_targets().chart->set_value_source(false,10);
    dbPanel.all_render_targets().chart->set_axis_title_keys("Frequency, Hz (X)","Power (Y)");
    if(dbChart->y_range().first!=-90 || dbChart->bar_baseline_minimum())return failure(82,__LINE__);
    return 0;
}

static int pie_values(auto* pie) {
    if(!pie->set_values({1,3}) || pie->slice_count()!=2 || pie->slice_value(0)!=1 || pie->slice_value(1)!=3 || std::abs(pie->slice_fraction(0)-.25)>1e-12)return failure(66,__LINE__);
    if(pie->set_values({1,-1}) || pie->projection_available() || pie->slice_count()!=0)return failure(67,__LINE__);
    if(!pie->set_values({0,0}) || !pie->projection_available() || pie->slice_count()!=0)return failure(68,__LINE__);
    if(!pie->set_values(std::vector<float>(100,1)) || pie->slice_count()!=64 || pie->slice_value(63)!=37)return failure(69,__LINE__);
    pie->set_values(std::vector<float>(7,1));
    if(pie->presentation_model()->state().value("slices").toList().size()!=7)return failure(71,__LINE__);
    pie->set_values({0});pie->set_projection_status("Renderer projection unavailable");
    if(!pie->presentation_model()->state().value("title").toString().contains("unavailable"))return failure(72,__LINE__);
    const auto pieLanguage=ui::Language::instance().code();
    for(const auto* language:{"en","ru"}) {
        ui::Language::instance().set(language);pie->set_values({-1});
        if(pie->presentation_model()->state().value("title").toString()!=ui::text("Pie charts require finite nonnegative values"))return failure(73,__LINE__);
    }
    ui::Language::instance().set(pieLanguage);
    pie->set_values({});pie->set_projection_status({});if(!pie->presentation_model()->state().value("title").toString().isEmpty())return failure(74,__LINE__);
return 0;
}

static int vector_modes(GraphPanel& panel,FreqChartView* chart,QJsonObject& emitted,int& restorationWrites) {
    // The graph family fixes rank; only rendering modes within that family
    // can change without touching processing topology.
    panel.set_display_mode("image2d");
    if(!panel.active_chart() || panel.image_view() || panel.active_spectrogram())return failure(47,__LINE__);
    panel.set_display_mode("points");if(chart->plot_style()!="points")return failure(58,__LINE__);
    panel.set_display_mode("pie");auto* pie=panel.pie_view();
    if(!pie || panel.active_chart() || panel.image_view() || panel.active_spectrogram() || emitted["display_mode"]!="pie")return failure(65,__LINE__);
    if(const auto result=pie_values(pie))return result;
    const auto pieWrites=restorationWrites;panel.restore_presentation({{"display_mode","pie"}});
    if(panel.pie_view()!=pie || restorationWrites!=pieWrites)return failure(70,__LINE__);
    panel.set_display_mode("points");panel.restore_presentation({});
    if(panel.active_chart()!=chart || panel.display_mode()!="bars" || chart->plot_style()!="bars")return failure(82,__LINE__);
return 0;
}

static int vector_panel_oracles() {
    auto *chart = new FreqChartView(10);
    GraphPanel panel("fft", chart, {{"auto_range_x", false}, {"minimum_x", 100},
                                  {"maximum_x", 500}, {"reverse_x", true}, {"axis_precision", 2}, {"auto_range_interval_seconds",5}}, nullptr);
    if(panel.preferences().value("auto_range_interval_seconds").toDouble()!=5 || chart->auto_range_interval()!=5)return failure(28,__LINE__);
    QJsonObject emitted;
    QObject::connect(&panel,&GraphPanel::presentation_changed,&panel,[&](const QString&,const QJsonObject& values){for(auto it=values.begin();it!=values.end();++it)emitted[it.key()]=it.value();});
    panel.changePreference("auto_range_interval_seconds",9);
    if(chart->auto_range_interval()!=9 || emitted["auto_range_interval_seconds"].toDouble()!=9)return failure(29,__LINE__);
    chart->set_axis_precision(2);
    chart->update(std::vector<float>(200, 1));
    chart->update_freq_step(20);
    chart->update(std::vector<float>(200,7));
    const auto heldY=chart->y_range().second;
    if(chart->x_range().first!=100 || chart->x_range().second!=500 || !chart->reverse_x() || chart->axis_precision()!=2)return failure(1,__LINE__);
    panel.changePreference("reverse_y",true);
    if(chart->y_range().second!=heldY)return failure(12,__LINE__);
    if(!chart->reverse_y())return failure(3,__LINE__);
    panel.changePreference("auto_range_x",true);
    if(chart->x_range().first!=0 || chart->x_range().second!=3980)return failure(4,__LINE__);
    panel.changePreference("minimum_x",600);
    if(chart->x_range().first!=0 || chart->x_range().second!=3980)return failure(5,__LINE__);
    int restorationWrites=0;
    QObject::connect(&panel,&GraphPanel::presentation_changed,&panel,[&]{++restorationWrites;});
    panel.restore_presentation({{"auto_range_x",false},{"minimum_x",200},{"maximum_x",600},{"reverse_x",false},
                                {"auto_range",false},{"minimum",-2},{"maximum",2},{"reverse_y",false},{"auto_range_interval_seconds",7}});
    if(restorationWrites || chart->x_range().first!=200 || chart->x_range().second!=600 || chart->reverse_x())return failure(6,__LINE__);
    if(chart->auto_range_interval()!=7 || panel.preferences().value("auto_range").toBool())return failure(30,__LINE__);
    if(const auto result=vector_modes(panel,chart,emitted,restorationWrites))return result;
    return 0;
}

class MatrixPanelScenario {
    QJsonObject matrixEmitted;
    int matrixWrites=0;
    NumericImageView* image=nullptr;
    SpectrogramGraph* arbitrarySurface=nullptr;
    float imageValue=0;
    SpectrogramAxisMetadata physical;
    GraphPanel matrixPanel{"matrix",new LineChartView,{{"kind","SpectrogramView"},{"display_mode","image2d"}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"image2d","surface3d"},{},false})};
    int initialize() {
    QObject::connect(&matrixPanel,&GraphPanel::presentation_changed,&matrixPanel,[&](const QString&,const QJsonObject& values){++matrixWrites;for(auto it=values.begin();it!=values.end();++it)matrixEmitted[it.key()]=it.value();});
    matrixPanel.set_axis_precision(5);
    image=matrixPanel.image_view();
    if(!image || matrixPanel.active_chart() || matrixPanel.active_spectrogram() || image->axis_precision()!=5)return failure(75,__LINE__);
        return 0;
    }
    int image_geometry() {
    image->set_matrix({{5}});
    if(image->matrix_dimensions()!=QSize(1,1) || !image->matrix_sample(0,0,imageValue) || imageValue!=5)return failure(48,__LINE__);
    image->set_matrix({{1,2,3,4,5}});
    if(image->matrix_dimensions()!=QSize(5,1) || !image->matrix_sample(0,4,imageValue) || imageValue!=5)return failure(49,__LINE__);
    image->set_matrix({{1,2,3},{4,5,6}});
    if(image->matrix_dimensions()!=QSize(3,2) || !image->matrix_sample(1,2,imageValue) || imageValue!=6)return failure(50,__LINE__);
        return 0;
    }
    int image_raster_and_visibility() {
    matrixPanel.resize(900,600);matrixPanel.show();QCoreApplication::processEvents();
    const QImage raster=image->grabFramebuffer();
    if(raster.isNull())return failure(60,__LINE__);
    const auto rasterPlot=QRectF(image->rect()).adjusted(76,16,-20,-54);
    if(rasterPlot.width()<=0 || rasterPlot.height()<=0 || raster.pixelColor(rasterPlot.center().toPoint())==image->palette().color(QPalette::Base))return failure(60,__LINE__);
    const auto imageBounds=image->y_range();matrixPanel.set_render_visible(false);matrixPanel.set_render_visible(true);
    if(image->y_range()!=imageBounds)return failure(51,__LINE__);
        return 0;
    }
    int surface_mode_and_precision() {
    matrixPanel.set_display_mode("surface3d");arbitrarySurface=matrixPanel.active_spectrogram();
    if(!arbitrarySurface || matrixPanel.image_view() || matrixPanel.active_chart() || matrixEmitted["display_mode"]!="surface3d")return failure(52,__LINE__);
    if(arbitrarySurface->surface_settings().value("axis_precision").toInt()!=5)return failure(76,__LINE__);
    matrixPanel.set_axis_precision(3);matrixPanel.set_display_mode("image2d");
    if(matrixPanel.image_view()->axis_precision()!=3)return failure(77,__LINE__);
    matrixPanel.set_display_mode("surface3d");
    if(arbitrarySurface->surface_settings().value("axis_precision").toInt()!=3)return failure(78,__LINE__);
        return 0;
    }
    int surface_geometry() {
    arbitrarySurface->update_frame_history({{5}});
    if(arbitrarySurface->projected_history_frames()!=1 || arbitrarySurface->projected_history_bands()!=1)return failure(53,__LINE__);
    arbitrarySurface->update_frame_history({{1,2,3,4,5}});
    if(arbitrarySurface->projected_history_frames()!=1 || arbitrarySurface->projected_history_bands()!=5)return failure(54,__LINE__);
    arbitrarySurface->update_frame_history({{1,2,3},{4,5,6}});
    if(arbitrarySurface->projected_history_frames()!=2 || arbitrarySurface->projected_history_bands()!=3 || !arbitrarySurface->projected_history_sample(1,2,imageValue) || imageValue!=6)return failure(55,__LINE__);
    if(matrixPanel.modes().size()!=2 || matrixPanel.display_mode()!="surface3d")return failure(56,__LINE__);
    matrixPanel.selectMode("image2d");
    if(!matrixPanel.image_view() || matrixPanel.active_chart())return failure(57,__LINE__);

    const auto restoreWrites=matrixWrites;
    matrixPanel.restore_presentation({{"display_mode","image2d"},{"auto_range_interval_seconds",8}});
    if(!matrixPanel.image_view() || matrixWrites!=restoreWrites)return failure(59,__LINE__);
        return 0;
    }
    int coordinate_permissions() {
    // Coordinate choices affect labels only, survive renderer switches and
    // restore without emitting edits. Nonuniform Mel centres come from source.

    physical.time_title_key="Time delay, seconds (X)";physical.time_coordinates=true;physical.ages={0,.02,.055};
    physical.column_title_key="Frequency, Hz (Z)";physical.column_coordinates=true;physical.columns={70,95,180};
    physical.column_index_title_key="Frequency, Mel bands (Z)";
    physical.time_index_title_key="Temporal frame (X)";
    if(matrixPanel.physical_frequency() || matrixPanel.physical_time())return failure(93,__LINE__);
    const auto beforeMetadata=matrixWrites;
    image->set_axis_metadata(physical);arbitrarySurface->set_axis_metadata(physical);
    if(matrixPanel.physical_frequency() || matrixPanel.physical_time())return failure(96,__LINE__);
    matrixPanel.set_axis_permissions({true,true});
    if(!matrixPanel.physical_frequency() || !matrixPanel.physical_time())return failure(94,__LINE__);
    if(matrixWrites!=beforeMetadata)return failure(95,__LINE__);
    if(matrixPanel.preferences().value("frequency_axis_mode").toString()!="coordinates")return failure(86,__LINE__);
        return 0;
    }
    int position_axes_preserve_storage() {
    const auto imageOwner=image->projection_storage_identity(),surfaceOwner=arbitrarySurface->projection_storage_identity();
    const auto surfaceRevision=arbitrarySurface->history_projection_revision();
    matrixPanel.changePreference("frequency_axis_mode","positions");
    matrixPanel.changePreference("time_axis_mode","positions");
    if(image->axis_metadata().column(2)!=2 || image->axis_metadata().age(2)!=2 ||
       arbitrarySurface->projected_axis_metadata().column_title_key!="Frequency, Mel bands (Z)" ||
       arbitrarySurface->projected_axis_metadata().time_title_key!="Temporal frame (X)" ||
       arbitrarySurface->projected_axis_metadata().age(2)!=2 || matrixEmitted["frequency_axis_mode"]!="positions" ||
       matrixEmitted["time_axis_mode"]!="positions" || image->projection_storage_identity()!=imageOwner ||
       arbitrarySurface->projection_storage_identity()!=surfaceOwner || arbitrarySurface->history_projection_revision()!=surfaceRevision)return failure(87,__LINE__);
        return 0;
    }
    int metadata_removal_and_return() {
    // Removing the source metadata must update controls even while positions
    // are selected: capability is based on the source, not displayed axes.
    const auto beforeRemoval=matrixWrites;
    image->set_axis_metadata({});arbitrarySurface->set_axis_metadata({});
    if(matrixPanel.physical_frequency() || matrixPanel.physical_time() || matrixPanel.preferences().value("frequency_axis_mode").toString()!="positions")
        fprintf(stderr,"Axes removal diagnostic frequency=%d time=%d mode=%s imagePhysical=%d surfacePhysical=%d paused=%d\n",matrixPanel.physical_frequency(),matrixPanel.physical_time(),qPrintable(matrixPanel.preferences().value("frequency_axis_mode").toString()),image->source_axis_metadata().column_coordinates,arbitrarySurface->source_axis_metadata().column_coordinates,arbitrarySurface->paused());
    if(matrixPanel.physical_frequency() || matrixPanel.physical_time() || matrixPanel.preferences().value("frequency_axis_mode").toString()!="positions")return failure(96,__LINE__);
    if(matrixWrites!=beforeRemoval)return failure(97,__LINE__);
    image->set_axis_metadata(physical);arbitrarySurface->set_axis_metadata(physical);
    if(!matrixPanel.physical_frequency() || !matrixPanel.physical_time() || matrixPanel.preferences().value("frequency_axis_mode").toString()!="positions")return failure(98,__LINE__);
        return 0;
    }
    int coordinate_values() {
    matrixPanel.changePreference("frequency_axis_mode","coordinates");
    matrixPanel.changePreference("time_axis_mode","coordinates");
    if(image->axis_metadata().column(2)!=180 || image->axis_metadata().age(2)!=.055 ||
       arbitrarySurface->projected_axis_metadata().column(2)!=180 || arbitrarySurface->projected_axis_metadata().age(2)!=.055)return failure(99,__LINE__);
        return 0;
    }
    int paused_coordinate_restore() {
    arbitrarySurface->set_paused(true);
    auto newer=physical;newer.ages={0,.04,.1};arbitrarySurface->set_axis_metadata(newer);
    auto pausedScale=arbitrarySurface->source_axis_metadata().value_scale;pausedScale.mode=ValueDisplayScale::Mode::Decibels;
    arbitrarySurface->set_value_scale(pausedScale);
    const auto beforeRestore=matrixWrites;
    matrixPanel.restore_presentation({{"display_mode","surface3d"},{"frequency_axis_mode","coordinates"},{"time_axis_mode","coordinates"}});
    if(matrixWrites!=beforeRestore || arbitrarySurface->projected_axis_metadata().age(2)!=.055 ||
       arbitrarySurface->projected_axis_metadata().column(2)!=180 || image->axis_metadata().column(1)!=95)return failure(88,__LINE__);
    arbitrarySurface->set_paused(false);
    if(arbitrarySurface->projected_axis_metadata().age(2)!=.1)return failure(89,__LINE__);
        return 0;
    }
    int generic_row_positions() {
    // Generic matrix row positions must retain their reversed surface mapping.
    auto generic=physical;generic.age_axis=false;generic.time_index_title_key="Row index (X)";
    generic.ages={7,5,2};generic.row_indices={2,1,0};
    arbitrarySurface->set_axis_metadata(generic);arbitrarySurface->set_axis_display_modes("positions","positions");
    if(arbitrarySurface->projected_axis_metadata().age(0)!=2 || arbitrarySurface->projected_axis_metadata().age(2)!=0)return failure(90,__LINE__);
        return 0;
    }
public:
    int run() {
        for(auto step:{&MatrixPanelScenario::initialize,&MatrixPanelScenario::image_geometry,&MatrixPanelScenario::image_raster_and_visibility,
            &MatrixPanelScenario::surface_mode_and_precision,&MatrixPanelScenario::surface_geometry,&MatrixPanelScenario::coordinate_permissions,
            &MatrixPanelScenario::position_axes_preserve_storage,&MatrixPanelScenario::metadata_removal_and_return,&MatrixPanelScenario::coordinate_values,
            &MatrixPanelScenario::paused_coordinate_restore,&MatrixPanelScenario::generic_row_positions})
            if(const auto result=(this->*step)())return result;
        return 0;
    }
};
static int matrix_panel_oracles() {MatrixPanelScenario scenario;return scenario.run();}
static int adaptive_tick_axis(NumericImageView& adaptiveImage,const QVariantMap& adaptiveState,const QFontMetrics& adaptiveFont,bool horizontal) {

        const auto ticks=adaptiveState.value(horizontal?"xTicks":"yTicks").toList();
        const auto sections=adaptiveImage.property(horizontal?"columnAxisSections":"timeAxisSections").toInt();
        if(ticks.size()!=sections+1 || ticks.size()<2)return failure(91,__LINE__);
        const double extent=horizontal?adaptiveImage.width()-adaptiveState.value("plotLeft").toDouble()-22:adaptiveImage.height()-86;
        double previous=-1;QString previousText;
        for(int i=0;i<ticks.size();++i) {
            const auto tick=ticks[i].toMap();const double position=tick.value("position").toDouble();const auto text=tick.value("text").toString();
            if(!std::isfinite(position) || position<=previous || text.isEmpty() || text==previousText ||
               (i==0 && position!=0) || (i==ticks.size()-1 && position!=1))return failure(91,__LINE__);
            if(i>0) {
                const double needed=horizontal?(adaptiveFont.horizontalAdvance(previousText)+adaptiveFont.horizontalAdvance(text))/2.+12:adaptiveFont.height()+12;
                if((position-previous)*extent<needed)return failure(91,__LINE__);
            }
            previous=position;previousText=text;
        }return 0;
}

static int adaptive_image_oracles() {
    // A bigger 2D viewport exposes more sections while values and owner stay fixed.
    NumericImageView adaptiveImage;
    auto adaptiveData=std::vector<std::vector<float>>(400,std::vector<float>(50,0));
    adaptiveData[0][0]=.25f;adaptiveData[123][17]=-.75f;adaptiveData[399][49]=1.f;
    adaptiveImage.set_matrix(adaptiveData);
    const auto adaptiveOwner=adaptiveImage.projection_storage_identity();
    adaptiveImage.resize(320,200);adaptiveImage.show();QCoreApplication::processEvents();
    const auto smallRaster=adaptiveImage.grabFramebuffer();
    if(smallRaster.isNull())return failure(91,__LINE__);
    const auto smallTime=adaptiveImage.property("timeAxisSections").toInt(),smallColumns=adaptiveImage.property("columnAxisSections").toInt();
    adaptiveImage.resize(1400,900);QCoreApplication::processEvents();const auto largeRaster=adaptiveImage.grabFramebuffer();
    if(largeRaster.isNull())return failure(91,__LINE__);
    if(adaptiveImage.property("timeAxisSections").toInt()<=smallTime ||
       adaptiveImage.property("columnAxisSections").toInt()<=smallColumns || adaptiveImage.value_count()!=20000)return failure(91,__LINE__);
    if(adaptiveImage.projection_storage_identity()!=adaptiveOwner)return failure(91,__LINE__);
    for(const auto sample:{QPoint(0,0),QPoint(17,123),QPoint(49,399)}) {
        float actual=0;
        if(!adaptiveImage.matrix_sample(sample.y(),sample.x(),actual) || actual!=adaptiveData[sample.y()][sample.x()])return failure(91,__LINE__);
    }
    // Check readable, ordered ticks rather than the old dense-grid count.
    const auto adaptiveState=adaptiveImage.presentation_model()->state();
    const QFontMetrics adaptiveFont(adaptiveImage.font());
    for(const bool horizontal:{false,true}) {
        if(const auto result=adaptive_tick_axis(adaptiveImage,adaptiveState,adaptiveFont,horizontal))return result;
    }
    return 0;
}

static int scalar_panel_oracles() {
    GraphPanel scalarPanel("metrics",new LineChartView,{{"kind","ScalarView"}},nullptr,nullptr,presentation::plot_capabilities(presentation::PlotDescriptor{{"metrics","signedbar"},{},false}));
    if(!scalarPanel.metrics_view() || scalarPanel.active_chart() || scalarPanel.display_mode()!="metrics")return failure(83,__LINE__);
    scalarPanel.set_display_mode("surface3d");if(scalarPanel.display_mode()!="metrics")return failure(84,__LINE__);
    scalarPanel.metrics_view()->set_inputs({{3,"Metric"}});scalarPanel.metrics_view()->set_value(3,"18446744073709551615");
    for(const auto* mode:{"light","dark"}){ui::Theme::instance().set(mode);QCoreApplication::processEvents();
        if(scalarPanel.metrics_view()->exact_value(3)!="18446744073709551615")return failure(85,__LINE__);}
    return 0;
}

static int surface_adoption_oracles() {
    // Reuse the fixed composition-owned surface across renderer switches.
    auto* suppliedSurface=new SpectrogramGraph;
    GraphPanel adopted("adopted",new LineChartView,{{"display_mode","surface3d"}},nullptr,suppliedSurface,presentation::plot_capabilities(presentation::PlotDescriptor{{"image2d","surface3d"},{},false}));
    if(adopted.active_spectrogram()!=suppliedSurface)return failure(61,__LINE__);
    adopted.restore_presentation({});
    if(adopted.display_mode()!="surface3d" || adopted.active_spectrogram()!=suppliedSurface)return failure(79,__LINE__);
    adopted.restore_presentation({{"display_mode","line"}});
    if(!adopted.image_view() || adopted.active_chart() || adopted.active_spectrogram())return failure(80,__LINE__);
    adopted.restore_presentation({});
    if(adopted.display_mode()!="surface3d" || adopted.active_spectrogram()!=suppliedSurface)return failure(81,__LINE__);

    adopted.set_display_mode("line");adopted.set_display_mode("surface3d");
    if(adopted.active_spectrogram()!=suppliedSurface)return failure(62,__LINE__);
    SpectrogramAxisMetadata rowAxes;rowAxes.age_axis=false;rowAxes.time_coordinates=true;rowAxes.ages={7,5};
    if(!suppliedSurface->set_axis_metadata(rowAxes))return failure(63,__LINE__);
    rowAxes.age_axis=true;if(suppliedSurface->set_axis_metadata(rowAxes))return failure(64,__LINE__);
    return 0;
}

static int surface_range_oracles() {
    TimedSpectrogram spectrogram;
    spectrogram.update_frame_history(std::vector<std::vector<float>>(400,std::vector<float>(50,0)));
    if(spectrogram.y_range()!=QPair<float,float>{0,1})return failure(31,__LINE__);
    spectrogram.update_frame_history({{1,2},{3,4}});
    const auto firstRange=spectrogram.y_range();
    if(firstRange.first!=0 || firstRange.second<4.8)return failure(32,__LINE__);
    spectrogram.update_frame_history({{.01f,30}});
    auto stableOptions=spectrogram.surface_settings();stableOptions["reverse_time"]=true;
    stableOptions["auto_range_interval_seconds"]=7;stableOptions["extension"]=99;
    spectrogram.set_surface_settings(stableOptions);
    spectrogram.set_render_visible(false);spectrogram.set_render_visible(true);
    if(spectrogram.y_range()!=firstRange || spectrogram.surface_settings().value("auto_range_interval_seconds").toDouble()!=7 || spectrogram.surface_settings().value("extension").toInt()!=99)return failure(33,__LINE__);
    const auto roundTrip=SurfaceSettings::effective({{"defaults",SurfaceSettings::defaults()}},{{"surface_override",QJsonObject{{"enabled",true},{"values",spectrogram.surface_settings()}}}});
    if(roundTrip["auto_range_interval_seconds"].toDouble()!=7 || roundTrip["extension"].toInt()!=99)return failure(34,__LINE__);
    spectrogram.set_paused(true);
    spectrogram.update_frame_history({{100,200}});
    spectrogram.now=11000;
    stableOptions["gradient"]="viridis";stableOptions["auto_range_interval_seconds"]=1;
    spectrogram.set_surface_settings(stableOptions);
    if(spectrogram.y_range()!=firstRange || spectrogram.projected_history_frames()!=1)return failure(38,__LINE__);
    spectrogram.set_paused(false);
    float resumedValue=0;
    if(spectrogram.y_range().second<240 || !spectrogram.projected_history_sample(0,1,resumedValue) || resumedValue!=200)return failure(40,__LINE__);
    // Renderer initialization deliberately requires a native session. Validate
    // settings lifecycle here without creating an OpenGL widget/context.
    int userEdits = 0;
    QObject::connect(&spectrogram, &SpectrogramGraph::surface_settings_changed, &spectrogram,
                     [&userEdits](const QJsonObject &) { ++userEdits; });
    auto options = spectrogram.surface_settings();
    options["y_range_mode"] = "manual"; options["y_min"] = -.5; options["y_max"] = 4;
    spectrogram.set_surface_settings(options);
    if (userEdits || spectrogram.y_range().first != -.5 || spectrogram.y_range().second != 4)
        return failure(7,__LINE__);
    return 0;
}

static int axis_control_oracles(QApplication& app) {
    AxisControlWidget axis("Amplitude (Y)");
    int writes = 0;
    QObject::connect(&axis, &AxisControlWidget::range_changed, &axis, [&writes] { ++writes; });
    axis.set_state(.123456789123, 1, false, true);
    if (writes || std::abs(axis.minimum_box()->value() - .123456789123) > 1e-12 ||
        axis.maximum_box()->text().contains("000000") || axis.minimumSizeHint().width() > 160) {
        std::fprintf(stderr, "Compact axis: writes=%d value=%.15g maximum=%s minimum width=%d\n",
                     writes, axis.minimum_box()->value(), qPrintable(axis.maximum_box()->text()),
                     axis.minimumSizeHint().width());
        return failure(8,__LINE__);
    }
    axis.maximum_box()->setValue(.01);
    if (writes != 1 || axis.maximum_box()->value() <= axis.minimum_box()->value()) return failure(9,__LINE__);
    QScrollArea scroll;
    scroll.setWidgetResizable(true);scroll.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body=new QWidget;
    body->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    auto *bodyLayout=new QVBoxLayout(body);
    auto *compact=new AxisControlWidget("Frequency, Mel bands (Z)",body);
    compact->set_hint("Range (%)\nHistory: 400 / 400 frames");
    bodyLayout->addWidget(compact);bodyLayout->addStretch();scroll.setWidget(body);
    scroll.resize(200,220);scroll.show();app.processEvents();
    const auto compactLabels=compact->findChildren<QLabel *>();for(auto *label:compactLabels) {
        if(label->isVisible() && label->wordWrap() && label->height()<label->heightForWidth(label->width())) {
            std::fprintf(stderr,"Wrapped label clipped: %s %dx%d requires %d\n",qPrintable(label->text()),
                         label->width(),label->height(),label->heightForWidth(label->width()));return failure(10,__LINE__);
        }
    }
    return 0;
}

static int fixed_view_mode_oracles() {
    struct Expected { const char* kind; const char* mode; bool audio; };
    constexpr std::array expected{
        Expected{"ScalarMetricsView","metrics",false},Expected{"ScalarSignedBarView","signedbar",false},
        Expected{"VectorLineView","line",false},Expected{"VectorPointView","points",false},
        Expected{"VectorBarView","bars",false},Expected{"VectorPieView","pie",false},
        Expected{"MatrixImageView","image2d",false},Expected{"MatrixSurfaceView","surface3d",false},
        Expected{"AudioLineView","line",true},Expected{"AudioPointView","points",true},
        Expected{"AudioBarView","bars",true},Expected{"AudioPieView","pie",true},
        Expected{"AudioImageView","image2d",true},Expected{"AudioSurfaceView","surface3d",true}};
    const auto visible=numeric_editor::visible_catalog();
    for(const auto& item:expected) {
        const auto kind=QString::fromLatin1(item.kind);
        const auto descriptor=numeric_editor::plot_descriptor_for_kind(kind);
        const auto edited=presentation::plot_descriptor_from_preferences(
            {{"plot_modes",QJsonArray{"pie","surface3d"}},{"plot_specialization",item.audio?"generic":"audio"}},descriptor);
        if(!visible.contains(kind) || edited.modes!=QStringList{QString::fromLatin1(item.mode)} ||
           edited.audio_scales!=item.audio)return failure(127,__LINE__);
    }
    const auto all=numeric_editor::catalog();
    for(const auto* kind:{"ScalarView","AudioView","FFTView","MelView","WindowView",
                          "WindowedView","SpectrogramView"}) {
        if(visible.contains(kind) || !all.contains(kind))return failure(128,__LINE__);
    }
    if(visible.contains("WindowFunctionView") || all.contains("WindowFunctionView"))
        return failure(129,__LINE__);
    return 0;
}

static int independent_plot_oracles(QApplication& app) {
    const presentation::PlotDescriptor audio{{"line","points"},{"Graphs","Audio","Signals"},true};
    const auto saved=presentation::plot_descriptor_preferences(audio);
    const auto restored=presentation::plot_descriptor_from_preferences(saved,{});
    const auto capabilities=presentation::plot_capabilities(restored);
    if(capabilities.modes!=audio.modes || !capabilities.audio_scales ||
       presentation::plot_category_path(restored)!=audio.category_path)return failure(121,__LINE__);
    const auto generic=presentation::plot_descriptor_from_preferences({{"plot_specialization","generic"}},audio);
    if(presentation::plot_capabilities(generic).audio_scales || generic.modes!=audio.modes)return failure(122,__LINE__);
    const auto malformed=presentation::plot_descriptor_from_preferences({{"plot_modes",QJsonArray{"bogus"}}},audio);
    if(malformed.modes!=audio.modes)return failure(123,__LINE__);
    const auto filtered=presentation::plot_descriptor_from_preferences({{"plot_modes",QJsonArray{"line","bogus","line"}}},audio);
    if(filtered.modes!=QStringList{"line"})return failure(125,__LINE__);
    GraphPanel signedOnly("signedOnly",new LineChartView,{{"display_mode","signedbar"}},nullptr,nullptr,
        presentation::plot_capabilities(presentation::PlotDescriptor{{"signedbar"},{},false}));
    if(signedOnly.display_mode()!="signedbar" || !signedOnly.metrics_view())return failure(126,__LINE__);
    if(presentation::PlotDescriptor{}.category_path!=QStringList{"Graphs","Universal","Vectors"})return failure(124,__LINE__);
    if(!coordinate_controls_ignore_titles())return failure(120,__LINE__);
    for(auto oracle:{nearest_point_oracles,selection_translation_oracles,amplitude_hold_oracles,interval_control_oracles,extreme_amplitude_oracles})
        if(const auto result=oracle())return result;
    if(const auto result=readout_layout_oracles(app))return result;
    if(!density_scale_oracles())return failure(13,__LINE__);
    if(!plot_navigation_oracles())return failure(12,__LINE__);
    if(const auto result=fixed_view_mode_oracles())return result;
    if(!retained_renderer_oracles())return failure(11,__LINE__);
    return 0;
}
int main(int argc, char **argv) {
    QApplication app(argc,argv);
    if(const auto result=independent_plot_oracles(app))return result;
    for(auto oracle:{piano_setting_oracles,decibel_source_oracles,vector_panel_oracles,matrix_panel_oracles,
        scalar_panel_oracles,surface_adoption_oracles,surface_range_oracles,adaptive_image_oracles})
        if(const auto result=oracle())return result;
    if(const auto result=axis_control_oracles(app))return result;
    std::puts("GraphPanel axes: restore, dynamic FFT range, reverse, precision, validation; retained camera speed, automatic Line/Freq bounds and pixel aggregation passed");
    return 0;
}
