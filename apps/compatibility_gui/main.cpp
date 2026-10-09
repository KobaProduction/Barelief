#include "app.h"
#include "visualization/spectrograms/spectrogram_graph.h"
#include <visualization/quick/quick_ui.h>

#include <QtWidgets/qapplication.h>
#include <QtGui/qscreen.h>

using namespace Qt::StringLiterals;

int main(int argc, char **argv) {
    ui::quick::configure();
    Application a(argc, argv);

    return Application::exec();
}
