#include "app.h"
#include "visualization/spectrograms/spectrogram_graph.h"

#include <QtWidgets/qapplication.h>
#include <QtGui/qscreen.h>

using namespace Qt::StringLiterals;

int main(int argc, char **argv) {
    qputenv("QSG_RHI_BACKEND", "opengl");
    Application a(argc, argv);

    return Application::exec();
}
