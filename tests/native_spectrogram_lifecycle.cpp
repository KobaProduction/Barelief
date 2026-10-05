#include "visualization/spectrograms/spectrogram_graph.h"
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QPointer>
#include <QScreen>
#include <QtTest/QTest>
#include <memory>

// Manual check: a native OpenGL window is required, not the offscreen placeholder.
int main(int argc, char **argv) {
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QApplication app(argc, argv);
    if (app.platformName() == "offscreen" || app.platformName() == "minimal") {
        qCritical("Spectrogram lifecycle check requires a native graphical session");
        return 2;
    }
    for (int iteration = 0; iteration < 24; ++iteration) {
        auto window = std::make_unique<QMainWindow>();
        auto *dock = new QDockWidget("Spectrogram lifecycle check", window.get());
        auto *graph = new SpectrogramGraph(dock);
        if (!graph->initialize(QSize(240, 180), QSize(800, 600))) return 1;
        dock->setWidget(graph);
        window->addDockWidget(Qt::LeftDockWidgetArea, dock);
        window->resize(820, 620);
        const auto screens = app.screens();
        window->move(screens[iteration % screens.size()]->availableGeometry().topLeft() + QPoint(20, 20));
        window->show();
        if (iteration % 2) dock->setFloating(true);
        QTest::qWait(150);
        QPointer<Q3DSurface> native;
        for (auto *candidate : app.allWindows()) {
            if (candidate->objectName() == "spectrogramWindow") native = qobject_cast<Q3DSurface *>(candidate);
        }
        if (!native || !native->hasContext()) return 1;
        auto *container = graph->findChild<QWidget *>("spectrogramContainer");
        if (!container || native->size() != container->size()) return 1;
        native->scene()->activeCamera()->setXRotation(float(iteration * 11));
        native->requestUpdate();
        window->resize(828, 628);
        // Destroy before the requested update is delivered, including floating docks.
        window.reset();
        if (native) { qCritical("Native Spectrogram survived its owner"); return 1; }
        QTest::qWait(20);
    }
    qInfo("Native Spectrogram creation/pending-update/floating-dock teardown: 24 cycles passed");
    return 0;
}
