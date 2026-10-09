#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <pipeline/audio_source.h>
#include <QTemporaryDir>
#include <QAction>
#include <QPushButton>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <cmath>
#include <mutex>
#include <desktop/numeric_project_document.h>
#include <core_app/numeric_default_graph.h>
#include <QJsonDocument>
#include <QComboBox>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <QLabel>

// Explicit physical probe only: delegates to the actual platform source. No
// playback, synthetic substitution, saved PCM, or user configuration is used.
static void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
static bool wait(const auto& predicate,int timeout=6000){QElapsedTimer t;t.start();while(t.elapsed()<timeout){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(2);}return predicate();}
struct Observation {std::mutex mutex;pipeline::AudioSourceDiagnostics diagnostics;unsigned starts=0,stops=0;};
class ObservedSource final:public pipeline::IAudioSource {
    std::unique_ptr<IAudioSource> delegate;std::shared_ptr<Observation> observation;
    void observe(){const auto d=delegate->diagnostics();std::lock_guard lock(observation->mutex);observation->diagnostics=d;}
public:
    ObservedSource(std::unique_ptr<IAudioSource> source,std::shared_ptr<Observation> value):delegate(std::move(source)),observation(std::move(value)){}
    ~ObservedSource() override{delegate->stop();}
    void configure(const QJsonObject& value) override{delegate->configure(value);}
    void start() override{{std::lock_guard lock(observation->mutex);++observation->starts;}delegate->start();}
    void stop() override{delegate->stop();observe();std::lock_guard lock(observation->mutex);++observation->stops;}
    bool is_running() const override{return delegate->is_running();}
    pipeline::AudioSourceKind source() const override{return delegate->source();}
    void set_audio_handler(AudioHandler handler) override{delegate->set_audio_handler([this,handler=std::move(handler)](auto frame){observe();handler(std::move(frame));});}
    void set_status_handler(StatusHandler handler) override{delegate->set_status_handler([this,handler=std::move(handler)](bool ready,QString error){observe();handler(ready,std::move(error));});}
    void set_lifecycle_handler(LifecycleHandler handler) override{delegate->set_lifecycle_handler(std::move(handler));}
};
int main(int argc,char** argv){
    if(argc!=2 || (QByteArray(argv[1])!="--physical"&&QByteArray(argv[1])!="--physical-nonzero"&&QByteArray(argv[1])!="--physical-selector"))return 2;
    const bool selector_mode=QByteArray(argv[1])=="--physical-selector";
    const bool require_nonzero=QByteArray(argv[1])!="--physical";
    try {
        require(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen","isolated offscreen required");
        QTemporaryDir directory;require(directory.isValid(),"temporary directory");
        auto path=directory.filePath("project.json").toLocal8Bit();
        char executable[]="numeric_loopback_probe",option[]="--config";char* args[]={executable,option,path.data(),nullptr};int count=3;
        std::mutex observations_mutex;std::vector<std::shared_ptr<Observation>> observations;
        auto real=pipeline::default_audio_source_factory();
        CoreApplication app(count,args,[&](auto kind)->std::unique_ptr<pipeline::IAudioSource>{auto source=real(kind);if(kind!=pipeline::AudioSourceKind::SystemOutput)return source;auto o=std::make_shared<Observation>();{std::lock_guard lock(observations_mutex);observations.push_back(o);}return std::make_unique<ObservedSource>(std::move(source),o);});
        MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* w=qobject_cast<MainWindow*>(widget))window=w;
        require(window,"main window");window->show();
        auto* defaults=window->findChild<QAction*>("defaultPipeline");auto* run=window->findChild<QPushButton*>("pipelineRun");auto* stop=window->findChild<QPushButton*>("pipelineStop");require(defaults&&run&&stop,"actual controls");
        require(wait([&]{return !app.presenter().graph_request_pending();}),"initial pending command");
        stop->click();require(wait([&]{auto s=app.presenter().client().snapshot();return !app.presenter().graph_request_pending()&&s.statuses.value("analyzer").state==pipeline::State::Stopped&&s.statuses.value("capture").state==pipeline::State::Stopped;}),"initial Stop");
        {std::lock_guard lock(observations_mutex);observations.clear();}
        if(!selector_mode)defaults->trigger();QCoreApplication::processEvents();
        const auto prior_revision=app.presenter().client().snapshot().graph_revision;
        std::uint64_t revision=0,epoch=0;auto expected=selector_mode?numeric_startup_graph():numeric_default_graph();
        std::atomic<unsigned> pcm=0,fft=0,mel=0,history=0,nonzero_pcm=0,nonzero_fft=0,nonzero_mel=0,nonzero_history=0;std::atomic<bool> invalid=false;std::uint64_t ordinal=0,history_sequence=0;
        QObject witness;
        unsigned generator_frames=0;
        QObject::connect(&app.presenter(),&GuiPresenter::numeric_frame_ready,&witness,[&](const auto& delivered){
            if(!delivered.context||!delivered.frame||!delivered.frame.view().stamp.available)return;
            if(revision&&delivered.delivery.graph_revision==revision&&delivered.delivery.epoch==epoch&&delivered.context->endpoint.node==1&&delivered.context->endpoint.direction==domain::PortDirection::Output)++generator_frames;
            if(!revision||delivered.delivery.graph_revision!=revision||delivered.delivery.epoch!=epoch||delivered.context->endpoint.direction!=domain::PortDirection::Input)return;
            const auto node=delivered.context->endpoint.node;
            if(node!=9&&node!=10&&node!=11)return;
            const auto frame=delivered.frame.view();const auto* view=std::get_if<signal_contracts::NumericView>(&frame.payload);if(!view){invalid=true;return;}
            const auto contract=std::get<signal_contracts::NumericContract>(expected.nodes[node-1].inputs[0].contract.view());if(!signal_contracts::same_contract(contract,*view->contract)){invalid=true;return;}
            bool nonzero=false;for(std::size_t p=0;p<view->planes.size();++p)for(std::size_t i=0;i<view->actual[0];++i){float x=0;if(signal_contracts::load_element(*view,{p,i,0},x)!=signal_contracts::Status::ok||!std::isfinite(x))invalid=true;nonzero|=std::abs(x)>1e-8f;}
            if(nonzero){if(node==9)++nonzero_pcm;if(node==10)++nonzero_fft;if(node==11)++nonzero_mel;}
            if(node==9)++pcm;if(node==10)++fft;if(node==11)++mel;
        });
        QObject::connect(&app.presenter(),&GuiPresenter::numeric_history_ready,&witness,[&](const auto& row){
            if(!revision||row.compact.delivery.graph_revision!=revision||row.compact.delivery.epoch!=epoch)return;
            if(row.event!=pipeline::NumericHistoryEvent::Row||!row.compact.context||row.compact.context->endpoint.node!=8||row.compact.context->endpoint.direction!=domain::PortDirection::Input)return;
            if(!row.compact.frame){invalid=true;return;}
            const auto frame=row.compact.frame.view();
            const auto* values=std::get_if<signal_contracts::NumericView>(&frame.payload);
            const auto contract=std::get<signal_contracts::NumericContract>(expected.nodes[5].outputs[0].contract.view());
            if(!frame.stamp.available||!values||!values->contract||!signal_contracts::same_contract(contract,*values->contract)||values->planes.size()!=1||values->actual[0]!=50){invalid=true;return;}
            bool nonzero=false;
            for(std::size_t i=0;i<50;++i){float x=0;if(signal_contracts::load_element(*values,{0,i,0},x)!=signal_contracts::Status::ok||!std::isfinite(x))invalid=true;nonzero|=std::abs(x)>1e-8f;}
            if(row.capacity!=400||row.filled_count!=std::min(row.accepted_row_ordinal,std::uint64_t{400})||row.accepted_row_ordinal<=ordinal||frame.stamp.sequence<=history_sequence)invalid=true;
            ++history;if(nonzero)++nonzero_history;ordinal=row.accepted_row_ordinal;history_sequence=frame.stamp.sequence;
        });
        run->click();
        require(wait([&]{auto s=app.presenter().client().snapshot();return !app.presenter().graph_request_pending()&&s.graph_revision>prior_revision&&(s.statuses.value("capture").state==pipeline::State::Running||s.statuses.value("capture").state==pipeline::State::Failed);}),"capture status timeout");
        auto state=app.presenter().client().snapshot();
        const auto status=state.statuses.value("capture");
        std::printf("capture state=%d error=%s revision=%llu\n",int(status.state),qPrintable(status.error),(unsigned long long)state.graph_revision);
        require(status.state==pipeline::State::Running,"actual capture did not start");
        domain::NumericProcessingGraph applied;require(bool(desktop::decode_numeric_graph(QJsonDocument(state.graph).toJson(),applied))&&applied.nodes.front().kind==(selector_mode?"TestSignal":"AudioSource"),"applied source identity");
        revision=state.graph_revision;epoch=state.epoch;
        require(wait([&]{return history>=5&&pcm>=5&&fft>=5&&mel>=5&&(!require_nonzero||(nonzero_pcm&&nonzero_fft&&nonzero_mel&&nonzero_history));},6000),"physical Default pipeline delivery/nonzero signal stalled");
        if(selector_mode) {
            auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());require(model,"selector model");
            auto* selector=model->delegateModel<AudioNodeModel>(14);auto* generator=model->delegateModel<AudioNodeModel>(1);require(selector&&generator,"startup selector sources");
            std::unique_ptr<QWidget> form(selector->parameter_widget());auto* lever=form->findChild<QComboBox*>("numericSelected");require(lever&&lever->count()==2,"actual selector lever");
            auto* values=generator->embeddedWidget()->findChild<QLabel*>("previewValues");require(values,"generator preview values");
            for(int choice:{1,0}) {
                const auto previous_revision=revision,previous_ordinal=ordinal;const auto previous_generator=generator_frames;
                revision=0;pcm=0;fft=0;mel=0;history=0;nonzero_pcm=0;nonzero_fft=0;nonzero_mel=0;nonzero_history=0;
                lever->setCurrentIndex(choice);
                require(wait([&]{auto s=app.presenter().client().snapshot();return s.graph_revision>previous_revision&&!app.presenter().graph_request_pending();}),"actual physical selector Apply");
                state=app.presenter().client().snapshot();revision=state.graph_revision;epoch=state.epoch;
                require(wait([&]{return history>=5&&ordinal>previous_ordinal&&pcm>=5&&fft>=5&&mel>=5&&nonzero_pcm&&nonzero_fft&&nonzero_mel&&nonzero_history&&generator_frames>previous_generator+5&&values->text().toULongLong()>0;},6000),"actual selected physical/generator chain or natural preview unavailable");
                require(!invalid,"actual compact Mel History payload/ordinal/sequence invalid");
                std::printf("actual History input=%d finite_Mel50_rows=%u nonzero_rows=%u ordinal=%llu sequence=%llu\n",choice,history.load(),nonzero_history.load(),(unsigned long long)ordinal,(unsigned long long)history_sequence);
                std::printf("actual selector input=%d revision=%llu history_ordinal=%llu generator_frames=%u nonzero_PCM=%u FFT=%u Mel=%u\n",choice,(unsigned long long)revision,(unsigned long long)ordinal,generator_frames,nonzero_pcm.load(),nonzero_fft.load(),nonzero_mel.load());
            }
        }
        std::shared_ptr<Observation> observation;{std::lock_guard lock(observations_mutex);require(observations.size()==1,"exact physical provider");observation=observations.back();}
        pipeline::AudioSourceDiagnostics d;{std::lock_guard lock(observation->mutex);d=observation->diagnostics;}
        std::printf("backend=%s device=%s endpoint=%s rate=%u native_rate=%u native_channels=%u callbacks=%llu callback_frames=%llu silence_frames=%llu pcm=%u fft=%u mel=%u history=%u ordinal=%llu gaps=%llu\n",qPrintable(d.backend),qPrintable(d.device),qPrintable(d.endpoint_id),d.sample_rate,d.native_sample_rate,d.native_channels,(unsigned long long)d.callback_count,(unsigned long long)d.callback_frames,(unsigned long long)d.silence_frames,pcm.load(),fft.load(),mel.load(),history.load(),(unsigned long long)ordinal,(unsigned long long)d.discontinuities);
        require(d.backend=="WASAPI"&&d.sample_rate==48000&&!invalid,"actual backend/contract admission");
        if(require_nonzero)require(d.callback_frames&&nonzero_pcm&&nonzero_fft&&nonzero_mel,"actual music-active native delivery not observed");
        std::printf("nonzero delivered PCM=%u FFT=%u Mel=%u (observed only; no playback generated)\n",nonzero_pcm.load(),nonzero_fft.load(),nonzero_mel.load());
        stop->click();require(wait([&]{return app.presenter().client().snapshot().statuses.value("capture").state==pipeline::State::Stopped;}),"Stop capture");
        const auto after=pcm.load(),after_history=history.load();QElapsedTimer t;t.start();while(t.elapsed()<80){QCoreApplication::processEvents();QThread::msleep(2);}require(pcm==after&&history==after_history,"delivery after Stop");
        run->click();require(wait([&]{std::lock_guard lock(observations_mutex);return observations.size()==2&&!app.presenter().graph_request_pending()&&app.presenter().client().snapshot().statuses.value("capture").state==pipeline::State::Running;}),"physical restart");
        state=app.presenter().client().snapshot();revision=state.graph_revision;epoch=state.epoch;ordinal=0;history_sequence=0;
        require(wait([&]{return pcm>after&&history>after_history;}),"physical restart delivery");
        require(!invalid,"physical restart payload/History ordering invalid");
        stop->click();require(wait([&]{return app.presenter().client().snapshot().statuses.value("capture").state==pipeline::State::Stopped;}),"restart Stop");
        std::puts(require_nonzero?"physical own-CoreApplication native nonzero PCM/FFT/Mel/History Run/Stop/Restart PASS":"physical own-CoreApplication Default Run/Stop and PCM/FFT/Mel/History delivery PASS; silence allowed, nonzero capture not asserted");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
