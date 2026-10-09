#include <core_app/core_application.h>
#include <visualization/quick/quick_ui.h>

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    ui::quick::configure();
    return CoreApplication(argc, argv).exec();
}
