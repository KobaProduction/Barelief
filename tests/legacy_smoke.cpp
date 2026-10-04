#include "app.h"
#include <QEventLoop>
#include <QTimer>
int main(int argc, char **argv) {
    Application app(argc, argv);
    QTimer::singleShot(500, &app, &QCoreApplication::quit);
    return app.exec();
}
