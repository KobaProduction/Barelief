#include <core_app/core_application.h>

int main(int argc, char* argv[]) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    return CoreApplication(argc, argv).exec();
}
