#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <desktop/numeric_project_document.h>
#include <QThread>
#include <QScreen>
#include <utility>
#include <QEvent>
#include <QStatusBar>
#include <QTimer>
#include <QFileInfo>
#include <QProcess>
#include <QDir>
#include <core/performance.h>
#include <core_app/gui_event_profile.h>
#include "core_application_state.h"
#include <barelief/ui_text.h>
#include <array>
#include <optional>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#if BARELIEF_DEV_TELEMETRY
namespace {
uint64_t gui_thread_cycles() noexcept {
#ifdef Q_OS_WIN
    ULONG64 cycles = 0;
    return QueryThreadCycleTime(GetCurrentThread(), &cycles) ? cycles : 0;
#else
    return 0;
#endif
}
}

#endif
Q_LOGGING_CATEGORY(core_app_main, "core_app.main")
#if BARELIEF_DEV_TELEMETRY
namespace {
struct GuiTelemetryScope {
    static inline const ___tracy_source_location_data source{"Qt event","CoreApplication::notify",__FILE__,__LINE__,0};
    core::telemetry::Zone zone{source};
    std::optional<core::perf::Scope> event_time,backing_store;
    GuiTelemetryScope(QObject* receiver,QEvent* event) {
        const bool bounded=core::perf::enabled.load(std::memory_order_relaxed);
        if(core::telemetry::enabled() && receiver) {
            const auto* name=receiver->metaObject()->className();zone.text(name,std::strlen(name));
        }
        if(!bounded)event_time.emplace(core::perf::Stage::GuiEvent);
        if(!core::telemetry::enabled() || !receiver)return;
        const auto* name=receiver->metaObject()->className();
        if(!bounded && event->type()==QEvent::UpdateRequest && std::strcmp(name,"MainWindow")==0)
            backing_store.emplace(core::perf::Stage::QtBackingStoreSync);
    }
};
struct GuiEventScope {
    decltype(core::perf::now()) start;
    std::array<char,64> name{};
    int type;
    gui_event_profile::Frame frame{gui_event_profile::current};
    bool detailed;
    std::uint64_t start_cycles;
    GuiEventScope(QObject* receiver,QEvent* event):start(core::perf::now()),type(int(event->type())),detailed(gui_event_profile::enabled),start_cycles(detailed?gui_thread_cycles():0) {
        if(receiver)std::strncpy(name.data(),receiver->metaObject()->className(),name.size()-1);
        if(detailed)gui_event_profile::current=&frame;
    }
    ~GuiEventScope(){if(detailed)gui_event_profile::current=frame.parent;}
    void record_profile(std::uint64_t duration) {
        if(!detailed)return;
        const auto end_cycles=gui_thread_cycles();
        const auto cycles=end_cycles>=start_cycles?end_cycles-start_cycles:0;
        if(frame.parent){frame.parent->children+=duration;frame.parent->children_cycles+=cycles;}
        gui_event_profile::record(name.data(),type,duration,frame.children,cycles,frame.children_cycles);
    }
    void record() {
        const auto duration=core::perf::now()-start;
        record_profile(duration);core::perf::record(core::perf::Stage::GuiEvent,start,duration);
        if(type==QEvent::UpdateRequest && std::strcmp(name.data(),"MainWindow")==0)core::perf::record(core::perf::Stage::QtBackingStoreSync,start,duration);
        if(duration>=50000000 && core::perf::slow_gui_event_count<core::perf::slow_gui_events.size())core::perf::slow_gui_events[core::perf::slow_gui_event_count++]={duration,type,name};
    }
};
}
#endif
bool CoreApplication::notify(QObject* receiver,QEvent* event) {
#if BARELIEF_DEV_TELEMETRY
    GuiTelemetryScope telemetry(receiver,event);
    if(!core::perf::enabled.load(std::memory_order_relaxed) || QThread::currentThread()!=thread())return QApplication::notify(receiver,event);
    GuiEventScope measurement(receiver,event);
    const auto result=QApplication::notify(receiver,event);measurement.record();return result;
#else
    return QApplication::notify(receiver,event);
#endif
}
CoreApplication::CoreApplication(int &argc, char *argv[]) : CoreApplication(argc, argv, {}) {}

CoreApplication::CoreApplication(int &argc, char *argv[], pipeline::AudioSourceFactory source_factory)
    : CoreApplication(argc,argv,std::move(source_factory),{}) {}
void CoreApplication::save_configuration_as(const QString &path) {
    if (path.trimmed().isEmpty())
        return;
    if (_save_configuration(true, path))
        _main_window->statusBar()->showMessage(ui::text("Settings saved: %1").arg(_config_manager.getConfigPath()),
                                               4000);
}

void CoreApplication::open_configuration(const QString& path) {
    if(path.trimmed().isEmpty() || _opening_configuration)return;
    const auto absolutePath=QFileInfo(path).absoluteFilePath();
    if(!_configuration_readable(absolutePath))return;
    QStringList arguments{"--config",absolutePath};arguments<<_serial_arguments;
    if(!_append_remote_arguments(arguments))return;
    if(!QProcess::startDetached(QCoreApplication::applicationFilePath(),arguments,QDir::currentPath())){
        _main_window->statusBar()->showMessage(ui::text("Could not start the application with settings: %1").arg(absolutePath),10000);return;
    }
    _opening_configuration=true;
    QTimer::singleShot(0,this,[this]{_main_window->close();QCoreApplication::quit();});
}
bool CoreApplication::_configuration_readable(const QString& path) {
    QString error;ConfigManager manager(path);desktop::NumericProjectDocument source;domain::NumericProcessingGraph draft;
    if(manager.loadNumeric(source,draft,&error))return true;
    _main_window->statusBar()->showMessage(ui::text("Could not open settings: %1\n%2").arg(path,error),10000);return false;
}
bool CoreApplication::_append_remote_arguments(QStringList& arguments) {
    if(!_presenter->is_remote())return true;
    const auto endpoint=_presenter->remote_endpoint();
    const auto host=endpoint["address"].toString();const int port=endpoint["port"].toInt();
    if(host.isEmpty() || port<1 || port>65535){_main_window->statusBar()->showMessage(ui::text("The selected remote engine endpoint is unavailable."),10000);return false;}
    arguments<<"--remote-host"<<host<<"--remote-port"<<QString::number(port);return true;
}
CoreApplication::~CoreApplication() {
    // The normal close path flushes editor preferences and workspace while
    // every widget is alive. Destructor flush callbacks must not recurse into
    // a partially destroyed window afterward.
    if(_main_window)_main_window->close();
    _flush_autosave();
    if(_save_thread){_save_thread->quit();_save_thread->wait();}
    if(_presenter)disconnect(_presenter.get(),&GuiPresenter::on_config_updated,this,nullptr);
    auto *window=_main_window;
    _main_window = nullptr;
    removeEventFilter(this);
    removeNativeEventFilter(this);
    delete window;
    _presenter.reset();
    _display_session.reset();
}
int CoreApplication::exec() const {
    if (_ctx.file_configs.ui.move_to_primary_screen_on_start &&
        _ctx.file_configs.ui.workspace.value("geometry").toString().isEmpty())
        _main_window->move(primaryScreen()->availableGeometry().center() - _main_window->rect().center());
    _main_window->show();
    return QApplication::exec();
}
void CoreApplication::apply_processing_settings() {
    _presenter->apply_processing_settings();
}
pipeline::NumericEngine &CoreApplication::pipeline() {
    auto *engine = _presenter->is_remote() ? nullptr : _engine_provider->local_engine();
    if (!engine)
        throw std::logic_error("Local diagnostic engine unavailable in remote GUI");
    return *engine;
}
void CoreApplication::start_wled_audio_reactive_server() {
    _presenter->start_wled_audio_reactive_server();
}
void CoreApplication::stop_wled_audio_reactive_server() {
    _presenter->stop_wled_audio_reactive_server();
}
void CoreApplication::restart_wled_audio_reactive_server() {
    _presenter->restart_wled_audio_reactive_server();
}
