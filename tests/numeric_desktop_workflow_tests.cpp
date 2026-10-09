#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/numeric_view_adapter.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <desktop/numeric_project_document.h>
#include <pipeline/audio_source.h>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace pipeline;
using namespace signal_contracts;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool wait_for(const std::function<bool()>& predicate){QElapsedTimer timer;timer.start();while(!predicate()&&timer.elapsed()<2500){QCoreApplication::processEvents();QThread::msleep(1);}return predicate();}
struct Probe {AudioSourceKind kind;IAudioSource::AudioHandler audio;IAudioSource::StatusHandler status;QJsonObject settings;int starts=0,stops=0;};
class Source final:public IAudioSource {
    std::shared_ptr<Probe> probe;
public:
    explicit Source(std::shared_ptr<Probe> p):probe(std::move(p)){}
    void configure(const QJsonObject& settings)override{probe->settings=settings;}
    void start()override{++probe->starts;probe->status(true,{});}
    void stop()override{++probe->stops;}
    bool is_running()const override{return probe->starts>probe->stops;}
    AudioSourceKind source()const override{return probe->kind;}
    void set_audio_handler(AudioHandler handler)override{probe->audio=std::move(handler);}
    void set_status_handler(StatusHandler handler)override{probe->status=std::move(handler);}
    void set_lifecycle_handler(LifecycleHandler)override{}
};
QJsonObject encode(const domain::NumericProcessingGraph& graph){QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"canonical graph encode");return QJsonDocument::fromJson(bytes).object();}
}
int main(int argc,char** argv){
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen")return 2;
    try {
        QTemporaryDir temporary;require(temporary.isValid(),"isolated config directory");
        auto graph=numeric_default_graph();desktop::NumericDraftPresentation presentation;
        const QJsonArray presets{QJsonObject{{"version",1},{"id","loopback"},{"name","Loopback"},{"graph",encode(graph)}},
            QJsonObject{{"version",1},{"id","synthetic"},{"name","Eight tones"},{"graph",encode(numeric_test_graph())}}};
        presentation.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"pipeline_auto_apply",false},{"pipeline_presets",presets},{"show_data_flow",true}}}};
        desktop::NumericProjectDocument document;const auto created=desktop::create_numeric_project(graph,presentation,document);
        if(!created)throw std::runtime_error((created.path+": "+created.error).toStdString());
        const auto path=temporary.filePath("workflow.json");require(bool(desktop::save_numeric_project(document,path)),"isolated project save");
        auto path_bytes=path.toLocal8Bit();char executable[]="numeric_desktop_workflow_tests",option[]="--config";char* arguments[]{executable,option,path_bytes.data(),nullptr};int count=3;
        std::vector<std::shared_ptr<Probe>> probes;
        CoreApplication app(count,arguments,[&](AudioSourceKind kind){auto p=std::make_shared<Probe>();p->kind=kind;probes.push_back(p);return std::make_unique<Source>(p);});
        MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;
        require(window,"actual application window");auto* editor=window->findChild<SchemeEditorWidget*>();require(editor,"actual editor");
        auto* apply_button=window->findChild<QAbstractButton*>("applyPipeline");require(apply_button,"actual Apply button");
        require(wait_for([&]{return probes.size()==1 && app.presenter().client().snapshot().statuses.value("analyzer").state==State::Running && !app.presenter().graph_request_pending();}),"initial continuous graph");
        const auto initial_revision=app.presenter().client().snapshot().graph_revision;
        require(editor->load_preset("loopback"),"load canonical loopback preset");
        require(probes.size()==1&&app.presenter().client().snapshot().graph_revision==initial_revision,"preset load changed working graph");
        QSignalSpy replies(&app.presenter(),&GuiPresenter::graph_apply_finished);
        auto apply=[&]{const auto previous=replies.size();apply_button->click();require(wait_for([&]{return replies.size()>previous;})&&replies.last()[1].toBool(),"actual Apply rejected");};
        apply();require(wait_for([&]{return probes.size()==1&&app.presenter().client().snapshot().statuses.value("capture").state==State::Running;}),"Apply never reached capture");
        require(probes[0]->kind==AudioSourceKind::SystemOutput&&probes[0]->settings["sample_rate"].toInt()==48000,"wrong loopback source or contract rate");
        const EndpointIdentity history{"main",7,domain::PortDirection::Output,0};
        DeliveredNumericFrame cold;std::uint64_t coldOrdinal=99,coldFilled=99;
        require(app.pipeline().query_numeric(history,cold,&coldOrdinal,&coldFilled)==Status::ok,"cold History is not immediately available");
        const auto coldView=std::get<NumericView>(cold.frame.view().payload);
        require(coldView.actual[0]==400 && coldView.actual[1]==50 && coldOrdinal==0 && coldFilled==0,"cold History must expose 400 prepared rows without advancing");
        for(size_t row=0;row<400;++row)for(size_t band=0;band<50;++band){float v=1;require(load_element(coldView,{0,row,band},v)==Status::ok && v==0,"cold History is not fully zero-filled");}
        presentation::NumericViewAdapter historyDisplay({1,{400,50,1048576,{true,32768}},{true,32768}});
        require(historyDisplay.bind(history)==Status::ok,"bind live History renderer");SpectrogramGraph surface;
        require(historyDisplay.accept_history_snapshot(cold,0,0)==Status::ok && historyDisplay.render(history,0,{nullptr,&surface,nullptr,true})==Status::ok,"cold authoritative History rendering failed");
        require(surface.projected_history_frames()==400 && surface.projected_axis_metadata().ages.size()==400 && surface.projected_axis_metadata().age(399)==399,"cold plot does not expose full fixed extent");cold={};
        std::uint64_t sequence=0,pcm=0;
        QObject::connect(&app.presenter(),&GuiPresenter::numeric_frame_ready,&app,[&](const DeliveredNumericFrame& frame){if(frame.context&&frame.context->endpoint.node==9&&frame.frame&&frame.frame.view().stamp.available)++pcm;});
        auto feed=[&](const std::shared_ptr<Probe>& probe,int blocks){for(int block=0;block<blocks;++block){AudioFrame frame;frame.sample_rate=48000;frame.sequence=++sequence;frame.generation=1;frame.timestamp_ns=std::int64_t((sequence-1)*256*1000000000ULL/48000);frame.left.resize(256);frame.right.resize(256);
            for(std::size_t i=0;i<256;++i)for(int tone=0;tone<8;++tone){const auto phase=6.283185307179586*double(73+113*tone)*double((sequence-1)*256+i)/48000.;frame.left[i]+=float(.02*std::sin(phase));frame.right[i]+=float(.02*std::cos(phase));}probe->audio(frame);}QCoreApplication::processEvents();};
        feed(probes[0],80);require(wait_for([&]{return pcm>0;}),"loopback PCM never delivered to presenter");
        DeliveredNumericFrame before;std::uint64_t ordinal=0,filled=0;
        require(app.pipeline().query_numeric(history,before,&ordinal,&filled)==Status::ok&&ordinal>=25,"loopback full graph History absent");
        std::array<float,50> first{};const auto initial=std::get<NumericView>(before.frame.view().payload);for(std::size_t band=0;band<50;++band)require(load_element(initial,{0,initial.actual[0]-filled,band},first[band])==Status::ok,"old History row");before={};
        graph.nodes[8].enabled=false;require(editor->load_numeric_draft(graph),"compatible sink edit");apply();
        require(probes.size()==1&&probes[0]->stops==0,"compatible edit recreated capture resource");feed(probes[0],32);
        DeliveredNumericFrame after;std::uint64_t next=0,nextFilled=0;require(app.pipeline().query_numeric(history,after,&next,&nextFilled)==Status::ok&&next>ordinal,"compatible Apply reset or stalled History");
        const auto retained=std::get<NumericView>(after.frame.view().payload);for(std::size_t band=0;band<50;++band){float value=0;require(load_element(retained,{0,retained.actual[0]-nextFilled,band},value)==Status::ok&&value==first[band],"compatible Apply overwrote earlier rows");}after={};
        // PCM block count is not analysis row count: at 60 analyses/s and
        // 48 kHz each History row spans 800 samples, not one 256-sample block.
        feed(probes[0],1400);
        DeliveredNumericFrame full;std::uint64_t fullOrdinal=0,fullFilled=0;
        require(app.pipeline().query_numeric(history,full,&fullOrdinal,&fullFilled)==Status::ok && fullFilled==400 && fullOrdinal>400,"History never reaches full fixed capacity");
        require(historyDisplay.accept_history_snapshot(full,fullOrdinal,fullFilled)==Status::ok && historyDisplay.render(history,0,{nullptr,&surface,nullptr,true})==Status::ok,"full History renderer failed");
        const auto fullView=std::get<NumericView>(full.frame.view().payload);
        require(fullView.contract->coordinates[0].unit==NumericUnit::Seconds && fullView.contract->coordinates[1].unit==NumericUnit::Hertz && fullView.coordinates.axes[0].size()==400*8 && fullView.coordinates.axes[1].size()==50*8,"factory pipeline lost explicit physical History coordinates");
        const auto physicalAxes=surface.projected_axis_metadata();
        require(physicalAxes.time_coordinates && physicalAxes.column_coordinates && physicalAxes.age(0)==0,"real full pipeline does not expose seconds and Hertz to the surface");
        for(size_t band=0;band<50;++band){
            const auto frequency=pipeline::decode_coordinate(fullView.coordinates.axes[1].subspan(band*8,8));
            const auto expected=700.*std::expm1(std::log1p(24000./700.)*double(band+1)/51.);
            require(std::abs(frequency-expected)<1e-8 && physicalAxes.column(double(band))==frequency,"History/surface frequency is not the runtime Mel filter-bank centre");
        }
        const auto firstTime=pipeline::decode_coordinate(fullView.coordinates.axes[0].first(8));
        const auto lastTime=pipeline::decode_coordinate(fullView.coordinates.axes[0].subspan(399*8,8));
        require(lastTime>firstTime && std::abs(physicalAxes.age(399)-(lastTime-firstTime))<1e-12,"surface delay does not match real retained source timestamps");
        const auto matrixOwner=surface.projection_storage_identity();float amplitude=0;
        require(surface.projected_history_sample(399,18,amplitude),"rendered amplitude before axis change");
        surface.set_axis_display_modes("positions","positions");
        require(!surface.projected_axis_metadata().column_coordinates && surface.projected_axis_metadata().column(18)==18 && surface.projected_axis_metadata().age(399)==399,"actual pipeline axis selectors do not select positions/frames");
        surface.set_axis_display_modes("coordinates","coordinates");float restoredAmplitude=0;
        require(surface.projected_axis_metadata()==physicalAxes && surface.projection_storage_identity()==matrixOwner && surface.projected_history_sample(399,18,restoredAmplitude) && restoredAmplitude==amplitude,"axis selectors changed producer data, sample storage or physical coordinate values");
        const auto fullRevision=surface.history_projection_revision();full={};
        feed(probes[0],24);
        DeliveredNumericFrame rolling;std::uint64_t rollingOrdinal=0,rollingFilled=0;
        require(app.pipeline().query_numeric(history,rolling,&rollingOrdinal,&rollingFilled)==Status::ok && rollingOrdinal>fullOrdinal && rollingFilled==400,"History stopped replacing rows after capacity");
        require(historyDisplay.accept_history_snapshot(rolling,rollingOrdinal,rollingFilled)==Status::ok && historyDisplay.render(history,0,{nullptr,&surface,nullptr,true})==Status::ok,"rolling History render failed");
        require(surface.projected_history_frames()==400 && surface.history_projection_revision()>fullRevision,"full graph froze after History filled");rolling={};
        require(editor->load_preset("synthetic"),"synthetic preset load");apply();
        require(wait_for([&]{return probes.size()==2;}),"new preset source absent");require(probes[1]->kind==AudioSourceKind::TestSignal,"preset reused prior physical source");
        require(probes[0]->stops==1,"source replacement retained old provider");
        const auto retired=probes[0]->status;retired(false,"retired loopback error");probes[1]->status(false,"current test failure");
        require(wait_for([&]{return app.presenter().client().snapshot().statuses.value("capture").state==State::Failed;}),"source error not exposed");
        require(app.presenter().client().snapshot().statuses.value("capture").error.contains("current test failure")&&!app.presenter().client().snapshot().statuses.value("capture").error.contains("retired loopback error"),"retired source error escaped fence");
        require(app.presenter().client().snapshot().statuses.value("analyzer").state==State::Running,"source failure stopped core");
        std::printf("actual preset/Apply/Run/Stop workflow PCM=%llu History=%llu->%llu capture resources retained; source replacement/error fences PASS\n",(unsigned long long)pcm,(unsigned long long)ordinal,(unsigned long long)next);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"desktop workflow FAIL: %s\n",error.what());return 1;}
}
