#include <core_app/core_application.h>
#include <visualization/quick/quick_ui.h>
#include "native_performance_trace.h"
#include <QElapsedTimer>

int main(int argc, char* argv[]) {
    const bool tracing=qEnvironmentVariableIsSet("CORE_APP_TRACE_FILE")&&!qEnvironmentVariable("CORE_APP_TRACE_FILE").isEmpty();
    QElapsedTimer startup;if(tracing){startup.start();native_performance_trace::milestone("main_entry",0);}
    const auto entryForeground=tracing?native_performance_trace::foregroundSnapshot():QJsonObject{};
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    ui::quick::configure();
    CoreApplication app(argc, argv);
    if(tracing)native_performance_trace::milestone("constructor_complete",startup.elapsed());
    native_performance_trace::install(app,tracing?startup.elapsed():0,entryForeground);
    return app.exec();
}
