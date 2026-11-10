#include "menu_row_font_fixture.h"
#include "isolated_gui_platform.h"
#include <barelief/compact_spectrogram_view.h>
#include <barelief/classic_spectrogram_colors.h>
#include <barelief/ui_text.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/example_presentation.h>
#include <core_app/config_manager.h>
#include <core_app/core_application.h>
#include <core_app/numeric_view_adapter.h>
#include <core_app/main_window.h>
#include <core_app/workspace/workspace_window.h>
#include <pipeline/numeric_engine.h>
#include <barelief/quick/quick_ui.h>
#include <barelief/render_preparation_policy.h>
#include <QFile>
#include <QJsonDocument>
#include "synthetic_capture.h"
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/node_parameter_model.h>
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QJsonArray>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

namespace {
class LiveSpectrogramProbe final:public QObject {
    CoreApplication& app_;
    QElapsedTimer clock_,heartbeat_;
    QElapsedTimer event_clock_;
    const char* last_event_class_=nullptr;
    int last_event_type_=0;
    QMap<QString,double> slow_events_;
    QTimer pulse_;
    bool measuring_=false;
    double longest_gap_=0;
    QMap<int,quint64> snapshots_,first_,last_;
    QMap<QObject*,quint64> paints_;
    std::array<quint64,16> paint_buckets_{};
    bool eventFilter(QObject* object,QEvent* event) override {
        if(measuring_) {
            const double elapsed=event_clock_.nsecsElapsed()/1e6;
            if(last_event_class_ && elapsed>10) {
                const auto key=QString::fromLatin1(last_event_class_)+":"+QString::number(last_event_type_);
                slow_events_[key]=std::max(slow_events_.value(key),elapsed);
            }
            last_event_class_=object->metaObject()->className();last_event_type_=int(event->type());event_clock_.restart();
        }
        if(measuring_ && event->type()==QEvent::Paint && qobject_cast<ClassicSpectrogramWidget*>(object)) {
            ++paints_[object];const auto second=clock_.elapsed()/1000;
            if(second<qint64(paint_buckets_.size()))++paint_buckets_[std::size_t(second)];
        }
        return false;
    }
    void receive(const pipeline::DeliveredNumericFrame& frame,quint64 ordinal) {
        if(!measuring_ || !frame.context)return;
        const auto node=frame.context->endpoint.node;
        if(!snapshots_.value(node))first_[node]=ordinal;
        ++snapshots_[node];last_[node]=ordinal;
    }
    void focus() {
        const int node=qEnvironmentVariableIntValue("SPECTROGRAM_PROBE_NODE");
        if(!node)return;
        for(const auto windows=QApplication::topLevelWidgets();auto* window:windows)if(auto* main=qobject_cast<MainWindow*>(window))
            for(const auto docks=main->workspaceDocks();auto* dock:docks)if(dock->property("graphNode").isValid()) {
                const bool selected=dock->property("graphNode").toInt()==node;
                if(selected){dock->show();dock->raise();}else dock->hide();
            }
    }
    void begin() {
        snapshots_.clear();first_.clear();last_.clear();paints_.clear();longest_gap_=0;
        measuring_=true;clock_.start();heartbeat_.start();event_clock_.start();
        QTimer::singleShot(qEnvironmentVariableIntValue("SPECTROGRAM_PROBE_NATIVE")==1?12000:8000,this,[this]{finish();});
    }
    void finish() {
        measuring_=false;const double seconds=clock_.nsecsElapsed()/1e9;
        QJsonArray streams,views;
        for(auto it=snapshots_.cbegin();it!=snapshots_.cend();++it)
            streams.append(QJsonObject{{"node",it.key()},{"snapshots",double(*it)},{"snapshot_hz",*it/seconds},{"rows_hz",(last_[it.key()]-first_[it.key()])/seconds}});
        for(auto it=paints_.cbegin();it!=paints_.cend();++it)if(auto* view=qobject_cast<ClassicSpectrogramWidget*>(it.key()))
            views.append(QJsonObject{{"name",view->objectName()},{"paint_hz",*it/seconds},{"values",double(view->value_count())},{"dpr",view->devicePixelRatioF()},{"width",view->width()},{"height",view->height()}});
        QJsonObject slow;for(auto it=slow_events_.cbegin();it!=slow_events_.cend();++it)slow[it.key()]=*it;
        QJsonArray buckets;for(auto count:paint_buckets_)buckets.append(double(count));
        const QJsonObject result{{"seconds",seconds},{"max_gui_gap_ms",longest_gap_},{"slow_event_gaps_ms",slow},{"paint_buckets",buckets},{"preview_interval_ms",app_.pipeline().numeric_preview_interval_ms()},{"streams",streams},{"views",views},{"runtime",app_.pipeline().snapshot().diagnostics}};
        std::printf("LIVE_SPECTROGRAM %s\n",QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        app_.quit();
    }
public:
    explicit LiveSpectrogramProbe(CoreApplication& app):app_(app) {
        app_.installEventFilter(this);
        connect(&app_.presenter(),&GuiPresenter::numeric_history_snapshot_ready,this,[this](const auto& frame,quint64 ordinal,quint64){receive(frame,ordinal);});
        pulse_.setTimerType(Qt::PreciseTimer);pulse_.setInterval(2);
        connect(&pulse_,&QTimer::timeout,this,[this]{if(measuring_){longest_gap_=std::max(longest_gap_,heartbeat_.nsecsElapsed()/1e6);heartbeat_.restart();}});
        pulse_.start();QTimer::singleShot(1500,this,[this]{focus();});
        QTimer::singleShot(qEnvironmentVariableIntValue("SPECTROGRAM_PROBE_NATIVE")==1?10000:3500,this,[this]{begin();});
    }
};
int live_spectrogram_probe(int argc,char** argv) {
    const bool native=qEnvironmentVariableIntValue("SPECTROGRAM_PROBE_NATIVE")==1;
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen" && !native)return 2;
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);ui::quick::configure();
    presentation::RenderPreparationPolicy::plot_host=presentation::PlotHost::NativeWindow;
    presentation::RenderPreparationPolicy::piano_host=presentation::PlotHost::NativeWindow;
    argv[1]=const_cast<char*>("--config");
    CoreApplication app(argc,argv,[](auto kind){return std::make_unique<test_fixture::SyntheticCapture>(kind,true);});
    if(native)for(const auto windows=QApplication::topLevelWidgets();auto* window:windows) {
        window->setAttribute(Qt::WA_ShowWithoutActivating);window->setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    }
    LiveSpectrogramProbe probe(app);return app.exec();
}
int startup_project(int argc,char** argv) {
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen")return 2;
    CoreApplication app(argc,argv,[](auto kind){return std::make_unique<test_fixture::SyntheticCapture>(kind,false);});
    const auto windows=QApplication::topLevelWidgets();
    for(auto* window:windows)if(auto* status=window->findChild<QLabel*>("configurationSaveStatus")){
        const auto* menu=window->findChild<QMenu*>("pipelinePresetsMenu");
        const auto* references=menu?menu->findChild<QMenu*>("rhythmReferencePresetsMenu"):nullptr;
        if(!references || !references->findChild<QAction*>("example-reference-beat") ||
           !menu->findChild<QAction*>("example-compact-spectrogram",Qt::FindDirectChildrenOnly))return 1;
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
    quint64 revision() const {return raster_revision_;}
    quint64 axis_revision() const {return axis_revision_;}
};
void palette_rounding_oracle() {
    std::vector<float> values;
    for(unsigned code=0;code<255;++code) {
        const float midpoint=float(code)+.5f;
        for(float value:{float(code),std::nextafter(midpoint,0.f),midpoint,std::nextafter(midpoint,256.f)})values.push_back(value);
    }
    values.push_back(255);
    RasterProbe view;view.resize(int(values.size()+200),400);
    view.set_snapshot(NumericMatrixSnapshot::take({values}),true);
    QImage image(view.size(),QImage::Format_RGB32);view.render(&image);
    const auto colors=classicSpectrogramColors();const auto& raster=view.raster();
    check(raster.width()==int(values.size()) && raster.height()==1,"palette oracle keeps every value");
    for(size_t column=0;column<values.size();++column)
        check(raster.pixel(int(column),0)==colors[size_t(std::lround(double(values[column])))],"palette rounding matches the existing linear scale");
}
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
int history_edit_probe() {
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    const auto kinds=numeric_editor::catalog();
    for(const auto& kind:kinds)registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    NumericEditorGraphModel graph(registry);QUndoStack undo;graph.set_undo_stack(&undo);check(graph.set_numeric_graph(numeric_compact_spectrogram_graph()),"load spectrogram editor");
    auto* model=graph.delegateModel<AudioNodeModel>(11);
    for(const unsigned capacity:{128,512,1024,8193}) {
        const auto accepted=model->parameter_model()->editLiteral("numericCapacity",QString::number(capacity),1);
        const auto& node=*model->numeric_node();
        const auto actual=std::get<domain::HistoryProcessingParameters>(node.parameters).capacity;
        std::printf("HISTORY_EDIT requested=%u accepted=%d actual=%zu error=%s\n",capacity,accepted,actual,qPrintable(model->parameter_model()->editState("numericCapacity").error));
        if(!accepted)return 1;
        const auto* view=graph.delegateModel<AudioNodeModel>(12)->numeric_node();
        check(std::get<signal_contracts::NumericContract>(view->inputs[0].contract.view()).shape.extents[0].maximum==capacity,"classical view follows history size");
    }
    check(!model->parameter_model()->field("numericCapacity")->numeric_maximum,"history has no artificial length bound");
    check(!model->parameter_model()->editLiteral("numericCapacity","18446744073709551615",1),"unrepresentable storage size refuses atomically");
    undo.undo();check(std::get<domain::HistoryProcessingParameters>(model->numeric_node()->parameters).capacity==1024,"history capacity undo");
    undo.redo();check(std::get<domain::HistoryProcessingParameters>(model->numeric_node()->parameters).capacity==8193,"history capacity redo");
    auto* compact=graph.delegateModel<AudioNodeModel>(7);
    check(compact->parameter_model()->editLiteral("numericCapacity","1024",1),"compact history is not limited to its preset's 512 frames");
    return 0;
}
void raster_invalidation_oracles() {
    RasterProbe view;view.resize(700,400);auto data=dense_spectrum();view.set_snapshot(data,true);
    QImage image(view.size(),QImage::Format_RGB32);view.render(&image);const auto revision=view.revision();
    const auto* allocation=view.raster().constBits();
    SpectrogramAxisMetadata axes;int axis_updates=0;QObject::connect(&view,&ClassicSpectrogramWidget::axis_metadata_changed,&view,[&]{++axis_updates;});
    check(view.set_axis_metadata(axes) && view.set_axis_metadata(axes) && axis_updates==0,"unchanged axes emit no invalidation");
    view.set_snapshot(data,true);view.render(&image);check(view.revision()==revision,"same immutable frame skips raster reconstruction");
    view.set_render_visible(false);
    for(int i=0;i<5;++i)view.set_snapshot(dense_spectrum(),true);
    check(view.revision()==revision,"hidden frames defer raster work");
    view.set_render_visible(true);view.render(&image);
    check(view.revision()==revision+1 && view.raster().constBits()==allocation,"latest pending frame reuses the raster allocation");
    NumericMatrixSnapshot::Matrix rows(1024,std::vector<float>(96));rows.back().back()=255;
    view.set_snapshot(NumericMatrixSnapshot::take(std::move(rows)),true);axes.ages.resize(1024);
    check(view.set_axis_metadata(axes) && view.value_count()==98304,"classical history beyond 512 rows follows shared presentation limits");
    view.render(&image);check(!view.raster().isNull(),"larger history remains drawable");
}
void classical_large_history_oracle() {
    NumericMatrixSnapshot::Matrix rows(8193,std::vector<float>(2048));rows.back().back()=255;
    RasterProbe view;view.resize(700,400);view.set_snapshot(NumericMatrixSnapshot::take(std::move(rows)),true);
    SpectrogramAxisMetadata axes;axes.ages.resize(8193);axes.time_coordinates=true;
    for(size_t row=0;row<axes.ages.size();++row)axes.ages[row]=double(row)/60;
    check(view.set_axis_metadata(axes),"history timestamps have no 4096-row quota");
    QImage image(view.size(),QImage::Format_RGB32);view.render(&image);
    check(view.value_count()==8193u*2048,"full user-sized classical history is preserved");
    const auto& raster=view.raster();check(raster.pixel(raster.width()-1,raster.height()-1)==classicSpectrogramColors().back(),"last bin of long history survives reduction");
}
void axis_cache_oracles() {
    RasterProbe view;view.resize(700,400);view.set_snapshot(dense_spectrum(),true);
    SpectrogramAxisMetadata axes;axes.time_coordinates=true;
    for(int row=0;row<64;++row)axes.ages.push_back(row/60.);
    check(view.set_axis_metadata(axes),"physical time axes accepted");
    QImage image(view.size(),QImage::Format_RGB32);view.render(&image);const auto revision=view.axis_revision();
    for(auto& age:axes.ages)age+=1e-12;
    view.set_axis_metadata(axes);view.set_snapshot(dense_spectrum(),true);view.render(&image);
    check(view.axis_revision()==revision,"unchanged visible tick labels reuse axes despite timestamp rounding noise");
    axes.ages.back()+=1;view.set_axis_metadata(axes);view.render(&image);
    check(view.axis_revision()==revision+1,"changed visible timestamps redraw the axes");
}
template<class View> void raster_benchmark(const char* name,NumericMatrixSnapshot data) {
    View view;view.set_layout(layout());view.resize(1100,750);view.set_snapshot(data,true);
    QImage image(view.size(),QImage::Format_RGB32);view.render(&image);
    QElapsedTimer timer;timer.start();
    for(unsigned i=0;i<80;++i){view.set_snapshot(data,true);view.render(&image);}
    std::printf("SPECTROGRAM_BENCH %s repeated_us=%.1f values=%zu\n",name,double(timer.nsecsElapsed())/80000,view.value_count());
    std::array<NumericMatrixSnapshot,3> frames;
    for(size_t i=0;i<frames.size();++i) {
        NumericMatrixSnapshot::Matrix matrix(data.size(),std::vector<float>(data.front().size()));
        for(size_t row=0;row<data.size();++row)for(size_t column=0;column<data.front().size();++column)matrix[row][column]=float((row+column+i)%256);
        frames[i]=NumericMatrixSnapshot::take(std::move(matrix));
    }
    timer.restart();
    for(unsigned i=0;i<80;++i){view.set_snapshot(frames[i%3],true);view.render(&image);}
    std::printf("SPECTROGRAM_BENCH %s changing_us=%.1f\n",name,double(timer.nsecsElapsed())/80000);
}
int spectrogram_benchmarks() {
    raster_benchmark<ClassicSpectrogramWidget>("classic",dense_spectrum());
    raster_benchmark<CompactSpectrogramView>("compact",samples());return 0;
}
struct HistoryBenchStorage {
    std::vector<std::byte> payload,metadata;
    std::array<signal_contracts::BufferSlot,2> payload_slots{},metadata_slots{};
    signal_contracts::BufferPool values,descriptions;
    HistoryBenchStorage(size_t bytes):payload(2*bytes),metadata(2*65536),values(payload,payload_slots,bytes),descriptions(metadata,metadata_slots,65536){}
};
struct HistoryBench {
    static constexpr size_t rows=1024,columns=2048;
    std::array<std::uint32_t,1> channels{0};
    signal_contracts::NumericContract contract;
    std::vector<std::uint8_t> values=std::vector<std::uint8_t>(rows*columns);
    std::vector<std::byte> times=std::vector<std::byte>(rows*8);
    signal_contracts::PlaneLayout plane{0,{columns,1}};
    HistoryBenchStorage storage{rows*columns};
    std::shared_ptr<pipeline::NumericDeliveryContext> context=std::make_shared<pipeline::NumericDeliveryContext>();
    ClassicSpectrogramWidget view;
    presentation::NumericViewAdapter adapter{{1,{SIZE_MAX,SIZE_MAX,SIZE_MAX,{true,SIZE_MAX}},{true,SIZE_MAX}}};
    presentation::NumericImagePort image_port;
    presentation::NumericRenderTarget target;
    QImage image{QSize(1100,750),QImage::Format_RGB32};
    std::array<double,4> elapsed{};
    HistoryBench() {
        using namespace signal_contracts;
        contract.dtype=DType::UInt8;contract.semantic=Semantic::History;contract.channel_order=channels;
        contract.shape={Rank::Matrix,{Axis::Time,Axis::Bin},{ExtentBounds{0,rows},ExtentBounds{columns,columns}}};
        contract.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1}};
        for(size_t row=0;row<rows;++row) {const auto bits=std::bit_cast<std::uint64_t>(row/60.);for(unsigned b=0;b<8;++b)times[row*8+b]=std::byte(bits>>(8*b));}
        for(size_t index=0;index<values.size();++index)values[index]=std::uint8_t(index%256);
        context->endpoint={"history-benchmark",12,domain::PortDirection::Input,0};
        check(domain::own_processing_contract(contract,numeric_editor::limits().frame,65536,context->contract)==Status::ok,"own history benchmark contract");
        check(adapter.bind(context->endpoint)==Status::ok,"bind history benchmark");
        image_port.set_axis_metadata=[this](const auto& axes){return view.set_axis_metadata(axes);};
        image_port.set_snapshot=[this](auto data,bool age){view.set_snapshot(std::move(data),age);};
        target.image=&image_port;target.visible=true;target.axes={true,true};view.resize(image.size());
    }
    void step(size_t index) {
        using namespace signal_contracts;
        FrameStamp stamp;stamp.producer_epoch=1;stamp.generation=1;stamp.sequence=rows+index;
        NumericView source{&contract,std::as_bytes(std::span(values)),{&plane,1},{rows,columns},{rows,index%rows,{}},{{times,{}}}};
        pipeline::DeliveredNumericFrame frame{context,{1,1},{},Status::ok};QElapsedTimer timer;timer.start();
        check(retain_frame({stamp,source},numeric_editor::limits().frame,storage.values,storage.descriptions,frame.frame)==Status::ok,"retain benchmark snapshot");
        elapsed[0]+=timer.nsecsElapsed();timer.restart();
        check(adapter.accept_history_snapshot(frame,stamp.sequence,rows)==Status::ok,"project benchmark snapshot");
        elapsed[1]+=timer.nsecsElapsed();timer.restart();
        check(adapter.render(context->endpoint,0,target)==Status::ok,"publish benchmark snapshot");
        elapsed[2]+=timer.nsecsElapsed();timer.restart();view.render(&image);elapsed[3]+=timer.nsecsElapsed();
    }
};
int history_path_benchmark() {
    HistoryBench bench;bench.step(0);bench.elapsed={};
    for(size_t index=1;index<=24;++index)bench.step(index);
    std::printf("HISTORY_PATH_MS rows=1024 bands=2048 copy=%.3f project=%.3f publish=%.3f paint=%.3f total=%.3f\n",bench.elapsed[0]/24e6,bench.elapsed[1]/24e6,bench.elapsed[2]/24e6,bench.elapsed[3]/24e6,(bench.elapsed[0]+bench.elapsed[1]+bench.elapsed[2]+bench.elapsed[3])/24e6);
    return 0;
}
void process_gui_for(int milliseconds) {
    QEventLoop events;QTimer::singleShot(milliseconds,&events,&QEventLoop::quit);events.exec();
}
void frame_rate_oracles() {
    ClassicSpectrogramWidget view;view.setAttribute(Qt::WA_ShowWithoutActivating);view.resize(700,400);
    const std::array data{dense_spectrum(),dense_spectrum()};size_t frame=0;view.set_snapshot(data.front(),true);view.show();
    QTimer frames;frames.setInterval(20);QObject::connect(&frames,&QTimer::timeout,&view,[&]{view.set_snapshot(data[++frame%data.size()],true);});
    frames.start();process_gui_for(1100);
    check(view.display_fps()>0,"painted widget frames produce nonzero FPS");
    std::printf("CLASSIC_UPDATE_FPS=%.1f values=%zu\n",double(view.display_fps()),view.value_count());
    frames.stop();process_gui_for(1100);check(view.display_fps()==0,"idle FPS expires without fabricated frames");
    view.set_render_visible(false);check(view.display_fps()==0,"hidden renderer reports zero FPS");view.hide();
}

}
int main(int argc,char** argv) {
    if(argc==3 && QString::fromLocal8Bit(argv[1])=="--live-probe")return live_spectrogram_probe(argc,argv);
    if(argc==3 && QString::fromLocal8Bit(argv[1])=="--config")return startup_project(argc,argv);
    QApplication app(argc,argv);check(test_fixture::isolated_gui_platform(),"isolated widget renderer only");
    auto fixture_font=menu_row_fixture_font();fixture_font.setPointSize(10);
    app.setFont(fixture_font);ui::Language::instance().set("ru");
    if(argc==3 && QString::fromLocal8Bit(argv[1])=="--load-project")return load_project(QString::fromLocal8Bit(argv[2]));
    if(argc==2 && QString::fromLocal8Bit(argv[1])=="--history-edit")return history_edit_probe();
    if(argc==2 && QString::fromLocal8Bit(argv[1])=="--benchmark")return spectrogram_benchmarks();
    if(argc==2 && QString::fromLocal8Bit(argv[1])=="--history-benchmark")return history_path_benchmark();
    const QString directory=argc==2?QString::fromLocal8Bit(argv[1]):QString{};
    if(!directory.isEmpty())check(QDir().mkpath(directory),"image directory");
    CompactSpectrogramView view;axis_oracles(view);history_oracles(view);paint_oracles(view,directory);classical_oracles(directory);classical_resolution_oracles(directory);palette_rounding_oracle();raster_invalidation_oracles();axis_cache_oracles();classical_large_history_oracle();check(history_edit_probe()==0,"history capacity editor regression");frame_rate_oracles();
    std::puts("PASS compact2d: 48+48 axes, shared history, time direction, UInt8 scale, isolated Light/Dark rendering");return 0;
}
