#include "core_app/graph_panel.h"
#include "visualization/charts/freq.h"
#include "visualization/spectrograms/spectrogram_graph.h"
#include "visualization/axis_control_widget.h"
#include "visualization/ui_text.h"
#include "visualization/amplitude_range.h"
#include "visualization/auto_range_interval_control.h"
#include "visualization/surface_settings.h"
#include "visualization/spectrograms/spectrogram_selection.h"
#include "visualization/numeric_image_view.h"
#include "visualization/numeric_pie_view.h"
#include "visualization/numeric_metrics_view.h"
#include <QComboBox>
#include <QAbstractItemModel>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <visualization/quick/quick_ui.h>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <functional>
#include <cstdio>

class TimedSpectrogram final : public SpectrogramGraph {
public:
    std::int64_t now=0;
protected:
    std::int64_t amplitude_clock() const override {return now;}
};

static bool retained_renderer_oracles() {
    const auto check=[](bool value,const char* message){if(!value)std::fprintf(stderr,"Renderer oracle: %s\n",message);return value;};
    const auto wait=[](const std::function<bool()>& ready){QElapsedTimer timer;timer.start();while(timer.elapsed()<1500){QCoreApplication::processEvents();if(ready())return true;QThread::msleep(3);}return ready();};
    // Retained runtime_tests camera speed oracle, with no GL initialization.
    for(float ratio:{.125f,1.f,8.f})for(float yaw:{0.f,.785398f,1.570796f}) {
        const auto step=SpectrogramGraph::camera_translation(yaw,1,0,ratio,2);
        const QVector3D world(step.x()*std::min(1.f,ratio),step.y()/2,step.z()*std::min(1.f,1.f/ratio));
        if(!check(std::abs(world.length()-1.f)<1e-5,"camera speed varies with rotation/aspect"))return false;
    }
    LineChartView line;line.set_range(-1,1);line.set_auto_resizing(true);line.update({});
    line.resize(400,300);line.show();
    std::vector<float> dense(10000,0);dense[5051]=42;dense[5052]=-17;line.update(dense);
    if(!check(wait([&]{const auto points=line.projected_points();return std::any_of(points.begin(),points.end(),[](auto p){return p.y()==42;});}),"dense line was not projected"))return false;
    const auto points=line.projected_points();
    const auto extrema=std::minmax_element(points.begin(),points.end(),[](auto a,auto b){return a.y()<b.y();});
    if(!check(points.size()<1000 && extrema.first->y()==-17 && extrema.second->y()==42,"line pixel aggregation lost spikes or paints an unbounded series"))return false;
    if(!check(line.x_range().second==9999 && line.y_range().first==-17 && line.y_range().second==42,"line automatic sample/amplitude bounds"))return false;
    line.update({});if(!check(wait([&]{return line.projected_point_count()==0;}),"empty line retains stale points"))return false;
    line.set_auto_gain(true,.001f,-.001f);line.set_range(-.001,.001);
    line.set_scalar_readout("0.49",true);line.set_x_coordinates({0});line.update({.49f});
    if(!check(line.x_range().first==-1 && line.x_range().second==0,"singleton timed scalar axis extends into future time"))return false;
    if(!check(wait([&]{return line.projected_point_count()==1;}),"singleton scalar has no visible point"))return false;
    const auto heldMaximum=line.y_range().second;
    for(int i=0;i<100;++i)line.update({i%2?.48f:.49f});
    if(!check(line.y_range().second==heldMaximum,"automatic amplitude jitters within a quantized band"))return false;
    line.update({1.7f});
    if(!check(line.y_range().second==heldMaximum,"fresh peak changed amplitude bounds inside the configured hold interval"))return false;
    const auto peakMaximum=line.y_range().second;
    for(int i=0;i<100;++i)line.update({.03f,.03f,.03f});
    if(!check(line.y_range().second==peakMaximum && wait([&]{return line.projected_point_count()==3;}),"quiet history shrinks immediately or constant scalar line is empty"))return false;
    for(auto point:line.projected_points())if(!check(std::abs(point.y()-.03)<1e-6,"constant scalar line changed value"))return false;
    line.set_auto_resizing(false);line.set_range(-2,2);line.update({100});
    if(!check(line.y_range().first==-2 && line.y_range().second==2,"manual amplitude range was overwritten"))return false;
    const auto theme=ui::Theme::instance().code();
    for(const auto* mode:{"light","dark"}) {
        ui::Theme::instance().set(mode);QCoreApplication::processEvents();
        ui::quick::UiBridge bridge;
        const auto colors=bridge.palette();
        if(!check(colors.value("base").value<QColor>()==line.palette().color(QPalette::Base) &&
            colors.value("highlight").value<QColor>()==line.palette().color(QPalette::Highlight) &&
            colors.value("text").value<QColor>()==line.palette().color(QPalette::Text),"QML chart does not inherit semantic theme styling"))return false;
    }
    ui::Theme::instance().set(theme);line.hide();
    FreqChartView frequency;frequency.update_freq_step(10);frequency.update(std::vector<float>(100,0));
    if(!check(frequency.x_range().second==990,"frequency automatic Hz bounds do not follow data size"))return false;
    frequency.resize(320,240);frequency.set_range(0,1);frequency.show();
    std::vector<float> narrow(2048,0);narrow[1377]=.8f;frequency.update(narrow);
    frequency.set_plot_style("line");
    const auto hasPeak=[&](qreal hz){const auto samples=frequency.projected_points();return std::any_of(samples.begin(),samples.end(),[hz](auto p){return p.x()==hz && std::abs(p.y()-.8)<1e-6;});};
    if(!check(wait([&]{return hasPeak(13770);}) && frequency.projected_point_count()<2048,"FFT pixel aggregation lost narrow peak/Hz position"))return false;
    const auto initial=frequency.projected_point_count();frequency.resize(1100,600);
    if(!check(wait([&]{return hasPeak(13770) && frequency.projected_point_count()>initial;}),"resize did not rebuild FFT resolution"))return false;
    frequency.update_freq_step(20);
    if(!check(wait([&]{return hasPeak(27540);}),"frequency-step change retained old Hz positions"))return false;
    frequency.set_x_coordinates(std::vector<double>{70.1,70.2});frequency.update({.2f,.3f});
    int coordinateRangeChanges=0;
    const auto coordinateRange=frequency.x_range();
    QObject::connect(frequency.presentation_model(),&QuickPlotModel::changed,&frequency,[&]{if(frequency.x_range()!=coordinateRange)++coordinateRangeChanges;});
    for(int i=0;i<20;++i){frequency.update_freq_step(1);frequency.set_x_coordinates({70.1,70.2});frequency.update({.3f,.4f});}
    if(!check(coordinateRangeChanges==0 && frequency.x_range()==coordinateRange && frequency.x_range().first==70.1 && frequency.x_range().second==70.2,"unchanged physical FFT axis emits transient index ranges"))return false;
    frequency.update({});if(!check(wait([&]{return frequency.projected_point_count()==0;}),"empty FFT retains stale peaks"))return false;
    return true;
}

static int failure(int code,int line) {std::fprintf(stderr,"GraphPanel axes failure: oracle %d at line %d\n",code,line);return code;}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    int projectedPoints=0;
    const auto planar=[&](int frame,int band){++projectedPoints;return SpectrogramProjectedSample{{double(frame),double(band)},0,true};};
    if(spectrogramNearestPoint({510,256},{0,510,0,256},planar)!=QPoint(510,256) || projectedPoints>18000)return failure(41,__LINE__);
    if(spectrogramNearestPoint({307.2,143.1},{30,470,20,230},planar)!=QPoint(307,143))return failure(42,__LINE__);
    if(spectrogramNearestPoint({0,0},{30,470,20,230},planar,10)!=QPoint(-1,-1))return failure(43,__LINE__);
    const auto reflected=[](int frame,int band){return SpectrogramProjectedSample{{510.-frame,256.-band},0,true};};
    if(spectrogramNearestPoint({20,7},{0,510,0,256},reflected)!=QPoint(490,249))return failure(44,__LINE__);
    const auto invisible=[](int,int){return SpectrogramProjectedSample{};};
    if(spectrogramNearestPoint({20,7},{0,510,0,256},invisible)!=QPoint(-1,-1))return failure(45,__LINE__);
    const auto selectionLanguage=ui::Language::instance().code();
    for(const auto* code:{"en","ru"}) {
        ui::Language::instance().set(code);
        const auto melRows=spectrogramSelectionRows("—","3","0.002",true,"Frequency, Mel bands (Z)","Amplitude (Y)");
        const auto hzRows=spectrogramSelectionRows("4","70.5","2",false,"Frequency, Hz (Z)","Value (Y)");
        if(melRows.size()!=3 || melRows[0]!=ui::text("Time delay (s): %1").arg("—") ||
           melRows[1]!=ui::text("Mel band")+": 3" || hzRows[0]!=ui::text("Temporal frame: %1").arg("4") ||
           hzRows[1]!=ui::text("Frequency (Hz)")+": 70.5" || hzRows[2]!=ui::text("Value")+": 2")return failure(46,__LINE__);
    }
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
    ui::Language::instance().set(selectionLanguage);
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
        for(auto* label:intervalControl.findChildren<QLabel*>())
            translated|=label->text()==ui::text("Auto range interval (s)");
        if(!translated || intervalControl.seconds()!=8 || intervalWrites!=1)return failure(35,__LINE__);
    }
    ui::Language::instance().set(savedLanguage);
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
    if(!retained_renderer_oracles())return failure(11,__LINE__);
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
    // The graph family fixes rank; only rendering modes within that family
    // can change without touching processing topology.
    panel.set_display_mode("image2d");
    if(!panel.active_chart() || panel.image_view() || panel.active_spectrogram())return failure(47,__LINE__);
    GraphPanel matrixPanel("matrix",new LineChartView,{{"kind","SpectrogramView"},{"display_mode","image2d"}},nullptr);
    QJsonObject matrixEmitted;int matrixWrites=0;
    QObject::connect(&matrixPanel,&GraphPanel::presentation_changed,&matrixPanel,[&](const QString&,const QJsonObject& values){++matrixWrites;for(auto it=values.begin();it!=values.end();++it)matrixEmitted[it.key()]=it.value();});
    matrixPanel.set_axis_precision(5);
    auto* image=matrixPanel.image_view();
    if(!image || matrixPanel.active_chart() || matrixPanel.active_spectrogram() || image->axis_precision()!=5)return failure(75,__LINE__);
    image->set_matrix({{5}});float imageValue=0;
    if(image->matrix_dimensions()!=QSize(1,1) || !image->matrix_sample(0,0,imageValue) || imageValue!=5)return failure(48,__LINE__);
    image->set_matrix({{1,2,3,4,5}});
    if(image->matrix_dimensions()!=QSize(5,1) || !image->matrix_sample(0,4,imageValue) || imageValue!=5)return failure(49,__LINE__);
    image->set_matrix({{1,2,3},{4,5,6}});
    if(image->matrix_dimensions()!=QSize(3,2) || !image->matrix_sample(1,2,imageValue) || imageValue!=6)return failure(50,__LINE__);
    matrixPanel.resize(900,600);matrixPanel.show();QCoreApplication::processEvents();
    const QImage raster=image->grabFramebuffer();
    if(raster.isNull())return failure(60,__LINE__);
    const auto rasterPlot=QRectF(image->rect()).adjusted(76,16,-20,-54);
    if(rasterPlot.width()<=0 || rasterPlot.height()<=0 || raster.pixelColor(rasterPlot.center().toPoint())==image->palette().color(QPalette::Base))return failure(60,__LINE__);
    const auto imageBounds=image->y_range();matrixPanel.set_render_visible(false);matrixPanel.set_render_visible(true);
    if(image->y_range()!=imageBounds)return failure(51,__LINE__);
    matrixPanel.set_display_mode("surface3d");auto* arbitrarySurface=matrixPanel.active_spectrogram();
    if(!arbitrarySurface || matrixPanel.image_view() || matrixPanel.active_chart() || matrixEmitted["display_mode"]!="surface3d")return failure(52,__LINE__);
    if(arbitrarySurface->surface_settings()["axis_precision"].toInt()!=5)return failure(76,__LINE__);
    matrixPanel.set_axis_precision(3);matrixPanel.set_display_mode("image2d");
    if(matrixPanel.image_view()->axis_precision()!=3)return failure(77,__LINE__);
    matrixPanel.set_display_mode("surface3d");
    if(arbitrarySurface->surface_settings()["axis_precision"].toInt()!=3)return failure(78,__LINE__);
    arbitrarySurface->update_frame_history({{5}});
    if(arbitrarySurface->projected_history_frames()!=1 || arbitrarySurface->projected_history_bands()!=1)return failure(53,__LINE__);
    arbitrarySurface->update_frame_history({{1,2,3,4,5}});
    if(arbitrarySurface->projected_history_frames()!=1 || arbitrarySurface->projected_history_bands()!=5)return failure(54,__LINE__);
    arbitrarySurface->update_frame_history({{1,2,3},{4,5,6}});
    if(arbitrarySurface->projected_history_frames()!=2 || arbitrarySurface->projected_history_bands()!=3 || !arbitrarySurface->projected_history_sample(1,2,imageValue) || imageValue!=6)return failure(55,__LINE__);
    if(matrixPanel.modes().size()!=2 || matrixPanel.display_mode()!="surface3d")return failure(56,__LINE__);
    matrixPanel.selectMode("image2d");
    if(!matrixPanel.image_view() || matrixPanel.active_chart())return failure(57,__LINE__);
    panel.set_display_mode("points");if(chart->plot_style()!="points")return failure(58,__LINE__);
    const auto restoreWrites=matrixWrites;
    matrixPanel.restore_presentation({{"display_mode","image2d"},{"auto_range_interval_seconds",8}});
    if(!matrixPanel.image_view() || matrixWrites!=restoreWrites)return failure(59,__LINE__);
    // Coordinate choices affect labels only, survive renderer switches and
    // restore without emitting edits. Nonuniform Mel centres come from source.
    SpectrogramAxisMetadata physical;
    physical.time_title_key="Time delay, seconds (X)";physical.time_coordinates=true;physical.ages={0,.02,.055};
    physical.column_title_key="Frequency, Hz (Z)";physical.column_coordinates=true;physical.columns={70,95,180};
    physical.column_index_title_key="Frequency, Mel bands (Z)";
    if(matrixPanel.physical_frequency() || matrixPanel.physical_time())return failure(93,__LINE__);
    const auto beforeMetadata=matrixWrites;
    image->set_axis_metadata(physical);arbitrarySurface->set_axis_metadata(physical);
    if(!matrixPanel.physical_frequency() || !matrixPanel.physical_time())return failure(94,__LINE__);
    if(matrixWrites!=beforeMetadata)return failure(95,__LINE__);
    if(matrixPanel.preferences().value("frequency_axis_mode").toString()!="coordinates")return failure(86,__LINE__);
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
    // Removing the source metadata must update controls even while positions
    // are selected: capability is based on the source, not displayed axes.
    const auto beforeRemoval=matrixWrites;
    image->set_axis_metadata({});arbitrarySurface->set_axis_metadata({});
    if(matrixPanel.physical_frequency() || matrixPanel.physical_time() || matrixPanel.preferences().value("frequency_axis_mode").toString()!="positions")return failure(96,__LINE__);
    if(matrixWrites!=beforeRemoval)return failure(97,__LINE__);
    image->set_axis_metadata(physical);arbitrarySurface->set_axis_metadata(physical);
    if(!matrixPanel.physical_frequency() || !matrixPanel.physical_time() || matrixPanel.preferences().value("frequency_axis_mode").toString()!="positions")return failure(98,__LINE__);
    matrixPanel.changePreference("frequency_axis_mode","coordinates");
    matrixPanel.changePreference("time_axis_mode","coordinates");
    if(image->axis_metadata().column(2)!=180 || image->axis_metadata().age(2)!=.055 ||
       arbitrarySurface->projected_axis_metadata().column(2)!=180 || arbitrarySurface->projected_axis_metadata().age(2)!=.055)return failure(99,__LINE__);
    arbitrarySurface->set_paused(true);
    auto newer=physical;newer.ages={0,.04,.1};arbitrarySurface->set_axis_metadata(newer);
    const auto beforeRestore=matrixWrites;
    matrixPanel.restore_presentation({{"display_mode","surface3d"},{"frequency_axis_mode","coordinates"},{"time_axis_mode","coordinates"}});
    if(matrixWrites!=beforeRestore || arbitrarySurface->projected_axis_metadata().age(2)!=.055 ||
       arbitrarySurface->projected_axis_metadata().column(2)!=180 || image->axis_metadata().column(1)!=95)return failure(88,__LINE__);
    arbitrarySurface->set_paused(false);
    if(arbitrarySurface->projected_axis_metadata().age(2)!=.1)return failure(89,__LINE__);
    // Generic matrix row positions must retain their reversed surface mapping.
    auto generic=physical;generic.age_axis=false;generic.time_index_title_key="Row index (X)";
    generic.ages={7,5,2};generic.row_indices={2,1,0};
    arbitrarySurface->set_axis_metadata(generic);arbitrarySurface->set_axis_display_modes("positions","positions");
    if(arbitrarySurface->projected_axis_metadata().age(0)!=2 || arbitrarySurface->projected_axis_metadata().age(2)!=0)return failure(90,__LINE__);
    // A bigger 2D viewport exposes more sections while values and owner stay fixed.
    NumericImageView adaptiveImage;
    adaptiveImage.set_matrix(std::vector<std::vector<float>>(400,std::vector<float>(50,0)));
    adaptiveImage.resize(320,200);adaptiveImage.show();QCoreApplication::processEvents();
    const auto smallRaster=adaptiveImage.grabFramebuffer();
    if(smallRaster.isNull())return failure(91,__LINE__);
    const auto smallTime=adaptiveImage.property("timeAxisSections").toInt(),smallColumns=adaptiveImage.property("columnAxisSections").toInt();
    adaptiveImage.resize(1400,900);QCoreApplication::processEvents();const auto largeRaster=adaptiveImage.grabFramebuffer();
    if(largeRaster.isNull())return failure(91,__LINE__);
    if(adaptiveImage.property("timeAxisSections").toInt()<=smallTime || adaptiveImage.property("timeAxisSections").toInt()<=5 ||
       adaptiveImage.property("columnAxisSections").toInt()<=smallColumns || adaptiveImage.value_count()!=20000)return failure(91,__LINE__);
    GraphPanel scalarPanel("metrics",new LineChartView,{{"kind","ScalarView"}},nullptr);
    if(!scalarPanel.metrics_view() || scalarPanel.active_chart() || scalarPanel.display_mode()!="metrics")return failure(83,__LINE__);
    scalarPanel.set_display_mode("surface3d");if(scalarPanel.display_mode()!="metrics")return failure(84,__LINE__);
    scalarPanel.metrics_view()->set_inputs({{3,"Metric"}});scalarPanel.metrics_view()->set_value(3,"18446744073709551615");
    for(const auto* mode:{"light","dark"}){ui::Theme::instance().set(mode);QCoreApplication::processEvents();
        if(scalarPanel.metrics_view()->exact_value(3)!="18446744073709551615")return failure(85,__LINE__);}
    panel.set_display_mode("pie");auto* pie=panel.pie_view();
    if(!pie || panel.active_chart() || panel.image_view() || panel.active_spectrogram() || emitted["display_mode"]!="pie")return failure(65,__LINE__);
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
    const auto pieWrites=restorationWrites;panel.restore_presentation({{"display_mode","pie"}});
    if(panel.pie_view()!=pie || restorationWrites!=pieWrites)return failure(70,__LINE__);
    // Reuse the fixed composition-owned surface across renderer switches.
    auto* suppliedSurface=new SpectrogramGraph;
    GraphPanel adopted("adopted",new LineChartView,{{"display_mode","surface3d"}},nullptr,suppliedSurface);
    if(adopted.active_spectrogram()!=suppliedSurface)return failure(61,__LINE__);
    adopted.restore_presentation({});
    if(adopted.display_mode()!="surface3d" || adopted.active_spectrogram()!=suppliedSurface)return failure(79,__LINE__);
    adopted.restore_presentation({{"display_mode","line"}});
    if(!adopted.image_view() || adopted.active_chart() || adopted.active_spectrogram())return failure(80,__LINE__);
    adopted.restore_presentation({});
    if(adopted.display_mode()!="surface3d" || adopted.active_spectrogram()!=suppliedSurface)return failure(81,__LINE__);
    panel.set_display_mode("points");panel.restore_presentation({});
    if(panel.active_chart()!=chart || panel.display_mode()!="bars" || chart->plot_style()!="bars")return failure(82,__LINE__);
    adopted.set_display_mode("line");adopted.set_display_mode("surface3d");
    if(adopted.active_spectrogram()!=suppliedSurface)return failure(62,__LINE__);
    SpectrogramAxisMetadata rowAxes;rowAxes.age_axis=false;rowAxes.time_coordinates=true;rowAxes.ages={7,5};
    if(!suppliedSurface->set_axis_metadata(rowAxes))return failure(63,__LINE__);
    rowAxes.age_axis=true;if(suppliedSurface->set_axis_metadata(rowAxes))return failure(64,__LINE__);
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
    if(spectrogram.y_range()!=firstRange || spectrogram.surface_settings()["auto_range_interval_seconds"].toDouble()!=7 || spectrogram.surface_settings()["extension"].toInt()!=99)return failure(33,__LINE__);
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
    for(auto *label:compact->findChildren<QLabel *>()) {
        if(label->isVisible() && label->wordWrap() && label->height()<label->heightForWidth(label->width())) {
            std::fprintf(stderr,"Wrapped label clipped: %s %dx%d requires %d\n",qPrintable(label->text()),
                         label->width(),label->height(),label->heightForWidth(label->width()));return failure(10,__LINE__);
        }
    }
    std::puts("GraphPanel axes: restore, dynamic FFT range, reverse, precision, validation; retained camera speed, automatic Line/Freq bounds and pixel aggregation passed");
    return 0;
}
