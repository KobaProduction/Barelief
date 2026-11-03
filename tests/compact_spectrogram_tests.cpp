#include "isolated_gui_platform.h"
#include <barelief/compact_spectrogram_view.h>
#include <barelief/classic_spectrogram_colors.h>
#include <barelief/ui_text.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/example_presentation.h>
#include <core_app/config_manager.h>
#include <core_app/core_application.h>
#include "synthetic_capture.h"
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <QApplication>
#include <QAction>
#include <QJsonArray>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

namespace {
int startup_project(int argc,char** argv) {
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen")return 2;
    CoreApplication app(argc,argv,[](auto kind){return std::make_unique<test_fixture::SyntheticCapture>(kind,false);});
    const auto windows=QApplication::topLevelWidgets();
    for(auto* window:windows)if(auto* status=window->findChild<QLabel*>("configurationSaveStatus")){
        const auto actions=window->findChildren<QAction*>();
        const auto presets=std::count_if(actions.begin(),actions.end(),[](const auto* action){return action->objectName().startsWith("example-");});
        if(presets!=2 || !window->findChild<QAction*>("example-reference-beat") || !window->findChild<QAction*>("example-compact-spectrogram"))return 1;
        std::fprintf(stderr,"GUI_PRESETS=reference-beat,compact-spectrogram\n");
        std::fprintf(stderr,"GUI_STARTUP_STATE=%d %s\n",status->property("saveState").toInt(),qPrintable(status->toolTip()));
        return status->property("saveState").toInt()==2?0:1; // ConfigurationSaveState::Saved.
    }
    return 2;
}
int load_project(const QString& path) {
    ConfigManager manager(path);desktop::NumericProjectDocument document;domain::NumericProcessingGraph graph;
    FileSaveApplicationConfig settings;QString error;
    if(!manager.loadNumeric(document,graph,&error) || !manager.readNumericSettings(document,settings,&error)){
        std::fprintf(stderr,"PROJECT_READ_FAILURE: %s\n",qPrintable(error));return 1;
    }
    desktop::NumericDraftPresentation presentation;const auto result=desktop::read_numeric_presentation(document,presentation);
    if(!result){std::fprintf(stderr,"PRESENTATION_FAILURE: %s\n",qPrintable(result.error));return 1;}
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    const auto kinds=numeric_editor::catalog();
    for(const auto& kind:kinds)registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    NumericEditorGraphModel model(registry);
    if(!model.set_numeric_graph(graph,&error,presentation.node_adapters)){
        std::fprintf(stderr,"EDITOR_IMPORT_FAILURE: %s\n",qPrintable(error));return 1;
    }
    std::puts("GUI_PROJECT_LOAD_PASS");return 0;
}
void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}
}
QJsonObject layout() {
    const auto example=numeric_compact_spectrogram_example();
    const auto views=example_view_preferences(example,"local",QString::fromStdString(example.graph.graph_id));
    for(const auto& value:views)if(value.toObject().value("node").toInt()==8)return value.toObject().value("compact_layout").toObject();
    return {};
}
NumericMatrixSnapshot samples() {
    NumericMatrixSnapshot::Matrix values(512,std::vector<float>(96));
    for(size_t row=0;row<512;++row)for(size_t column=0;column<96;++column)
        values[row][column]=float((column*7+row)%256);
    return NumericMatrixSnapshot::take(std::move(values));
}
NumericMatrixSnapshot dense_spectrum() {
    NumericMatrixSnapshot::Matrix values(64,std::vector<float>(2048));
    for(size_t column=0;column<2048;++column)values[column%7][column]=255;
    return NumericMatrixSnapshot::take(std::move(values));
}
class RasterProbe final:public ClassicSpectrogramWidget {
public:
    const QImage& raster() const {return raster_;}
};
void verify_resolved_peaks(const RasterProbe& view,bool horizontal) {
    const auto& raster=view.raster();const auto colors=classicSpectrogramColors();
    const auto bands=size_t(horizontal?raster.height():raster.width());
    const auto frames=size_t(horizontal?raster.width():raster.height());
    for(size_t column=0;column<2048;++column) {
        const auto band=((column+1)*bands-1)/2048,frame=((column%7+1)*frames-1)/64;
        check(raster.pixel(horizontal?int(frame):int(band),horizontal?int(bands-1-band):int(frame))==colors.back(),"every FFT bin survives pixel reduction");
    }
    check(raster.pixel(horizontal?int(frames-1):0,horizontal?int(bands-1):int(frames-1))==colors.front(),"silent history remains silent after reduction");
    float value=0;check(view.value_count()==131072 && view.sample(63,2047,value) && value==0,"renderer preserves all original 64x2048 values");
}
void classical_resolution_oracles(const QString& directory) {
    RasterProbe view;view.set_snapshot(dense_spectrum(),true);
    for(const auto* direction:{"vertical","horizontal"})for(const auto size:{QSize{700,400},QSize{480,240}}) {
        view.set_orientation(direction);view.resize(size);
        QImage image(view.size(),QImage::Format_RGB32);view.render(&image);
        verify_resolved_peaks(view,QString::fromLatin1(direction)=="horizontal");
        if(!directory.isEmpty())check(image.save(QDir(directory).filePath(QString("classic-peaks-%1-%2.png").arg(direction).arg(size.width()))),"save full-spectrum peak-preserving renderer");
    }
}
void axis_oracles(CompactSpectrogramView& view) {
    auto configured=layout();check(view.set_layout(configured),"explicit spectrogram layout");
    check(!view.set_layout({}),"incomplete axes rejected");
    auto invalid=configured;auto sections=configured["broadband_sections"].toArray();
    auto last=sections.last().toObject();last["count"]=7;sections[sections.size()-1]=last;invalid["broadband_sections"]=sections;
    check(!view.set_layout(invalid),"section counts must cover all 48 broadband channels");
    check(view.section_count()==7 && view.section_label(0)==QString::fromUtf8("Бас") && view.section_label(6)==QString::fromUtf8("Воздух"),"seven translated section labels");
    const std::array starts{0,12,16,22,30,36,40,48};
    for(size_t i=0;i<7;++i)check(view.section_range(i)==std::pair{starts[i],starts[i+1]},"section boundaries match 12+4+6+8+6+4+8");
    check(view.note_label(0)=="C3" && view.note_label(47)=="B6","48 note channels cover C3 through B6");
    const auto bounds=configured["broadband_centers_hz"].toArray();
    check(view.frequency(0)==bounds[0].toDouble(),"first group center from actual boundaries");
    check(view.frequency(47)==bounds[47].toDouble(),"last group center from actual boundaries");
    check(std::isnan(view.frequency(48)) && view.note_label(48).isEmpty(),"no overlap beyond 48 channels");
}
void history_oracles(CompactSpectrogramView& view) {
    const auto data=samples();view.set_snapshot(data,true);
    check(view.projection_storage_identity()==data.storage_identity(),"both groups share one immutable snapshot");
    float value=0;check(view.sample(511,48,value) && value==data[511][48],"first note uses column48, no transpose");
    check(view.sample(0,47,value) && value==data[0][47],"last broadband uses column47");
    SpectrogramAxisMetadata axes;axes.time_coordinates=true;
    for(unsigned i=0;i<512;++i)axes.ages.push_back(i/60.);
    check(view.set_axis_metadata(axes),"explicit history timestamps");
    check(view.age(511)==0 && std::abs(view.age(0)-511./60)<1e-12,"latest frame at right is age zero");
    check(view.value_range()==QPair<double,double>{0,255},"linear UInt8 values, no normalization or dB");
}
void paint_oracles(CompactSpectrogramView& view,const QString& directory) {
    view.resize(1600,750);
    const auto colors=classicSpectrogramColors();
    check(colors.front()==QColor(0x00,0x00,0x04).rgb() && colors.back()==QColor(0xfc,0xfd,0xbf).rgb(),"common black to light-yellow linear data scale");
    QRgb first=0;
    for(const auto* theme:{"light","dark"}) {
        ui::Theme::instance().set(theme);QCoreApplication::processEvents();
        QImage image(view.size(),QImage::Format_RGB32);image.fill(Qt::transparent);view.render(&image);
        check(!image.isNull(),"own widget renderer image");
        if(first)check(image.pixel(100,100)==first,"data color does not depend on interface theme");first=image.pixel(100,100);
        if(!directory.isEmpty())check(image.save(QDir(directory).filePath(QString("compact2d-%1.png").arg(theme))),"save grouped spectrum");
    }
    view.set_snapshot(NumericMatrixSnapshot::take({{1,2,3}}),true);
    float value=0;check(!view.sample(0,0,value),"wrong channel count clears stale display");
}
void classical_oracles(const QString& directory) {
    static_assert(std::is_base_of_v<ClassicSpectrogramWidget,CompactSpectrogramView>);
    ClassicSpectrogramWidget view;view.resize(1100,650);const auto data=samples();
    view.set_snapshot(data,true);SpectrogramAxisMetadata axes;axes.time_coordinates=axes.column_coordinates=true;
    for(unsigned i=0;i<512;++i)axes.ages.push_back(i/60.);
    for(unsigned i=0;i<96;++i)axes.columns.push_back((i+1)*10.);
    check(view.set_axis_metadata(axes),"classical explicit axes");
    for(const auto* direction:{"vertical","horizontal"}) {
        view.set_orientation(direction);check(view.projection_storage_identity()==data.storage_identity(),"direction changes no samples or history");
        check(view.age(511)==0 && view.frequency(95)==960,"classical time and frequency coordinates");
        QImage image(view.size(),QImage::Format_RGB32);view.render(&image);
        if(!directory.isEmpty())check(image.save(QDir(directory).filePath(QString("classic-%1.png").arg(direction))),"classical shared palette and raster");
    }
}
void process_gui_for(int milliseconds) {
    QEventLoop events;QTimer::singleShot(milliseconds,&events,&QEventLoop::quit);events.exec();
}
void frame_rate_oracles() {
    ClassicSpectrogramWidget view;view.setAttribute(Qt::WA_ShowWithoutActivating);view.resize(700,400);
    const auto data=dense_spectrum();view.set_snapshot(data,true);view.show();
    QTimer frames;frames.setInterval(20);QObject::connect(&frames,&QTimer::timeout,&view,[&]{view.set_snapshot(data,true);});
    frames.start();process_gui_for(1100);
    check(view.display_fps()>0,"painted widget frames produce nonzero FPS");
    std::printf("CLASSIC_UPDATE_FPS=%.1f values=%zu\n",double(view.display_fps()),view.value_count());
    frames.stop();process_gui_for(1100);check(view.display_fps()==0,"idle FPS expires without fabricated frames");
    view.set_render_visible(false);check(view.display_fps()==0,"hidden renderer reports zero FPS");view.hide();
}

}
int main(int argc,char** argv) {
    if(argc==3 && QString::fromLocal8Bit(argv[1])=="--config")return startup_project(argc,argv);
    QApplication app(argc,argv);check(test_fixture::isolated_gui_platform(),"isolated widget renderer only");
    check(QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"))>=0,"system font with Cyrillic glyphs");
    app.setFont(QFont("Segoe UI",10));ui::Language::instance().set("ru");
    if(argc==3 && QString::fromLocal8Bit(argv[1])=="--load-project")return load_project(QString::fromLocal8Bit(argv[2]));
    const QString directory=argc==2?QString::fromLocal8Bit(argv[1]):QString{};
    if(!directory.isEmpty())check(QDir().mkpath(directory),"image directory");
    CompactSpectrogramView view;axis_oracles(view);history_oracles(view);paint_oracles(view,directory);classical_oracles(directory);classical_resolution_oracles(directory);frame_rate_oracles();
    std::puts("PASS compact2d: 48+48 axes, shared history, time direction, UInt8 scale, isolated Light/Dark rendering");return 0;
}
