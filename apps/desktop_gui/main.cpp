#include <core_app/core_application.h>
#include <barelief/quick/quick_ui.h>
#include <barelief/render_preparation_policy.h>
#include <core/telemetry.h>
#if BARELIEF_DEV_TELEMETRY
#include "native_performance_trace.h"
#include "dev_telemetry_ui.h"
#endif
#include <QElapsedTimer>

int main(int argc, char* argv[]) {
    core::telemetry::Session telemetry;
    core::telemetry::thread_name("GUI");
#if BARELIEF_DEV_TELEMETRY
    const bool tracing=qEnvironmentVariableIsSet("CORE_APP_TRACE_FILE")&&!qEnvironmentVariable("CORE_APP_TRACE_FILE").isEmpty();
    QElapsedTimer startup;if(tracing){startup.start();native_performance_trace::milestone("main_entry",0);}
    const auto entryForeground=tracing?native_performance_trace::foregroundSnapshot():QJsonObject{};
#endif
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    ui::quick::configure();
    // Docked scenes and their settings share native hosts. A texture settings
    // sidebar would keep the entire window on GUI-thread RHI composition.
    // Proxy-embedded diagnostic plots explicitly keep their texture host.
    presentation::RenderPreparationPolicy::plot_host=presentation::PlotHost::NativeWindow;
    presentation::RenderPreparationPolicy::piano_host=presentation::PlotHost::NativeWindow;
    CoreApplication app(argc, argv);
#if BARELIEF_DEV_TELEMETRY
    install_dev_telemetry_ui();
    if(tracing)native_performance_trace::milestone("constructor_complete",startup.elapsed());
    native_performance_trace::install(app,tracing?startup.elapsed():0,entryForeground);
#endif
    return app.exec();
}
