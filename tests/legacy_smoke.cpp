#include "app.h"
#include "pipeline/test_signal.h"
#include <QEventLoop>
#include <QTimer>
int main(int argc, char **argv) {
    Application app(argc, argv, 0, [](pipeline::AudioSourceKind) {
        return pipeline::make_test_signal_source();
    });
    QTimer::singleShot(500, &app, &QCoreApplication::quit);
    return app.exec();
}
