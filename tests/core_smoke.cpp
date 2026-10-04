#include "core_app/core_application.h"
#include "core_app/gui_presenter.h"
#include "core_app/main_window.h"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QElapsedTimer>
#include <QThread>
#include <stdexcept>
#include <QTimer>
static int schema_regression() {
    auto require = [](bool ok, const char *message) { if (!ok) throw std::runtime_error(message); };
    QTemporaryDir directory;
    const auto path = directory.filePath("schema.json");
    const auto original_pipeline = QJsonObject{{"version", 99}, {"future", QJsonObject{{"keep", 42}}}};
    const auto original_draft = QJsonObject{{"version", 99}, {"future-draft", "keep separately"}};
    const auto original = QJsonDocument(QJsonObject{
        {"core", QJsonObject{{"run_loopback", false}, {"run_analyser", false}}},
        {"ui", QJsonObject{{"show_surface", false}, {"autosave_enabled", true}}},
        {"pipeline_graph", original_pipeline}, {"scene_graph", original_draft}}).toJson();
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot write schema fixture");
    require(file.write(original) == original.size(), "Cannot finish schema fixture");
    file.close();
    auto path_bytes = path.toLocal8Bit();
    char name[] = "schema_smoke", flag[] = "--config";
    char *args[] = {name, flag, path_bytes.data(), nullptr};
    int count = 3;
    CoreApplication app(count, args);
    auto wait = [&](auto predicate) {
        QElapsedTimer timer; timer.start();
        while (!predicate() && timer.elapsed() < 3000) {
            QCoreApplication::processEvents(); QThread::msleep(1);
        }
        require(predicate(), "Schema regression timed out");
    };
    wait([&] { return !app.presenter().graph_request_pending(); });
    auto read = [&] {
        require(file.open(QIODevice::ReadOnly), "Cannot read schema fixture");
        const auto bytes = file.readAll(); file.close(); return bytes;
    };
    require(read() == original, "Bootstrap rewrote incompatible config bytes");
    require(!app.presenter().saved_graph_warning().isEmpty(), "Missing incompatible schema diagnostic");
    MainWindow *window = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *candidate = qobject_cast<MainWindow *>(widget)) window = candidate;
    require(window && !window->findChild<QLabel *>("savedPipelineWarning")->text().isEmpty(),
            "Bootstrap error is not visible in the UI");
    app._ctx.file_configs.ui.show_samples = true;
    emit app.presenter().on_config_updated();
    auto saved = QJsonDocument::fromJson(read()).object();
    require(saved["pipeline_graph"].toObject() == original_pipeline &&
            saved["scene_graph"].toObject() == original_draft, "Prefs replaced incompatible saved schemas");
    app._ctx.file_configs.core.spectrum_merge = SpectrumMerge::Maximum;
    app.presenter().apply_processing_settings();
    wait([&] { return !app.presenter().graph_request_pending(); });
    saved = QJsonDocument::fromJson(read()).object();
    require(saved["pipeline_graph"].toObject() == original_pipeline &&
            saved["scene_graph"].toObject() == original_draft, "Processing prefs replaced saved schemas");
    auto *recover = window->findChild<QPushButton *>("defaultPipeline");
    require(recover, "Explicit recovery button missing");
    recover->click();
    wait([&] { return !app.presenter().graph_request_pending(); });
    saved = QJsonDocument::fromJson(read()).object();
    QString error;
    require(pipeline::GraphProgram::compile(saved["pipeline_graph"].toObject(), error) != nullptr &&
            pipeline::GraphProgram::compile(saved["scene_graph"].toObject(), error) != nullptr,
            "Explicit recovery did not save valid schemas");
    require(app.presenter().saved_graph_warning().isEmpty(), "Recovery did not clear diagnostic");
    return 0;
}
int main(int argc, char **argv) {
    if (qEnvironmentVariableIsSet("QT_VISUALIZATION_SCHEMA_REGRESSION")) {
        try { return schema_regression(); }
        catch (const std::exception &error) { qCritical("Schema regression: %s", error.what()); return 1; }
    }
    CoreApplication app(argc, argv);
    const bool expect_remote = qEnvironmentVariableIsSet("QT_VISUALIZATION_EXPECT_REMOTE");
    int spectra = 0;
    QObject::connect(&app, &CoreApplication::amplitudes_ready, &app, [&] { ++spectra; });
    QTimer::singleShot(expect_remote ? 1500 : 500, &app, &QCoreApplication::quit);
    const auto result = qEnvironmentVariableIsSet("QT_VISUALIZATION_HIDDEN_SMOKE") ? QApplication::exec() : app.exec();
    if (expect_remote && (!app.presenter().is_remote() || spectra < 10)) {
        qCritical("Remote GUI did not receive spectra: %d", spectra);
        return 1;
    }
    if (expect_remote)
        qInfo("Remote GUI received %d spectra from the console host", spectra);
    return result;
}
