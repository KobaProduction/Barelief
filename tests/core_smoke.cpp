#include "synthetic_capture.h"
#include "core_app/core_application.h"
#include "core_app/gui_presenter.h"
#include "core_app/main_window.h"
#include "core_app/scheme_editor/editor_widget.h"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QElapsedTimer>
#include <QThread>
#include <stdexcept>
#include <QTimer>
#include <QScreen>
#include <QDockWidget>
#include <QDir>
#include <QOpenGLWidget>
static int schema_regression() {
    auto require = [](bool ok, const char *message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    QTemporaryDir directory;
    const auto path = directory.filePath("schema.json");
    const auto original_pipeline = QJsonObject{{"version", 99}, {"future", QJsonObject{{"keep", 42}}}};
    const auto original_draft = QJsonObject{{"version", 99}, {"future-draft", "keep separately"}};
    const auto original =
        QJsonDocument(QJsonObject{{"core", QJsonObject{{"run_loopback", false}, {"run_analyser", false}}},
                                  {"ui", QJsonObject{{"show_spectrogram", false}, {"autosave_enabled", true}}},
                                  {"pipeline_graph", original_pipeline},
                                  {"scene_graph", original_draft}})
            .toJson();
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot write schema fixture");
    require(file.write(original) == original.size(), "Cannot finish schema fixture");
    file.close();
    auto path_bytes = path.toLocal8Bit();
    char name[] = "schema_smoke", flag[] = "--config";
    char *args[] = {name, flag, path_bytes.data(), nullptr};
    int count = 3;
    CoreApplication app(count, args, test_fixture::synthetic_capture_factory());
    auto wait = [&](auto predicate) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 3000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(predicate(), "Schema regression timed out");
    };
    wait([&] { return !app.presenter().graph_request_pending(); });
    auto read = [&] {
        require(file.open(QIODevice::ReadOnly), "Cannot read schema fixture");
        const auto bytes = file.readAll();
        file.close();
        return bytes;
    };
    require(read() == original, "Bootstrap rewrote incompatible config bytes");
    require(!app.presenter().saved_graph_warning().isEmpty(), "Missing incompatible schema diagnostic");
    MainWindow *window = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *candidate = qobject_cast<MainWindow *>(widget))
            window = candidate;
    require(window && !window->findChild<QLabel *>("savedPipelineWarning")->text().isEmpty(),
            "Bootstrap error is not visible in the UI");
    app._ctx.file_configs.ui.show_window_signal = true;
    emit app.presenter().on_config_updated();
    auto saved = QJsonDocument::fromJson(read()).object();
    require(saved["pipeline_graph"].toObject() == original_pipeline &&
                saved["scene_graph"].toObject() == original_draft,
            "Prefs replaced incompatible saved schemas");
    app._ctx.file_configs.core.spectrum_merge = SpectrumMerge::Maximum;
    app.presenter().apply_processing_settings();
    wait([&] { return !app.presenter().graph_request_pending(); });
    saved = QJsonDocument::fromJson(read()).object();
    require(saved["pipeline_graph"].toObject() == original_pipeline &&
                saved["scene_graph"].toObject() == original_draft,
            "Processing prefs replaced saved schemas");
    auto *recover = window->findChild<QAction *>("defaultPipeline");
    require(recover, "Explicit recovery button missing");
    recover->trigger();
    wait([&] { return app.presenter().saved_graph_warning().isEmpty() && !app.presenter().graph_request_pending(); });
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
        try {
            return schema_regression();
        } catch (const std::exception &error) {
            qCritical("Schema regression: %s", error.what());
            return 1;
        }
    }
    QTemporaryDir fresh_config;
    QByteArray fresh_path = fresh_config.filePath("first-run.json").toLocal8Bit();
    char fresh_name[] = "first_run", fresh_flag[] = "--config";
    char *fresh_args[] = {fresh_name, fresh_flag, fresh_path.data(), nullptr};
    int fresh_count = 3;
    const bool expect_first_run = qEnvironmentVariableIsSet("QT_VISUALIZATION_EXPECT_FIRST_RUN");
    CoreApplication app(expect_first_run ? fresh_count : argc, expect_first_run ? fresh_args : argv, test_fixture::synthetic_capture_factory());
    const bool expect_remote = qEnvironmentVariableIsSet("QT_VISUALIZATION_EXPECT_REMOTE");
    int spectra = 0;
    QObject::connect(&app, &CoreApplication::amplitudes_ready, &app, [&] { ++spectra; });
    const auto review_dir = qEnvironmentVariable("QT_VIS_UI_REVIEW_DIR");
    if (!review_dir.isEmpty()) {
        QDir().mkpath(review_dir);
        auto window = app.findChild<MainWindow *>();
        if (!window)
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *candidate = qobject_cast<MainWindow *>(widget))
                    window = candidate;
        if (!window)
            return 1;
        window->resize(1200, 900);
        auto capture = [window, review_dir](const QString &name) {
            window->grab().save(review_dir + "/" + name + ".png");
        };
        QTimer::singleShot(2200, &app, [capture, window, review_dir] {
            capture("spectrogram");
            for (auto *spectrogram : window->findChildren<QOpenGLWidget *>())
                spectrogram->grabFramebuffer().save(review_dir + "/spectrogram-native.png");
        });
        QTimer::singleShot(2700, &app, [window] {
            auto *dock = window->findChild<QDockWidget *>("fftDock");
            dock->show();
            dock->raise();
        });
        QTimer::singleShot(4000, &app, [capture] { capture("fft"); });
        QTimer::singleShot(4200, &app, [window] { window->findChild<QAction *>("openSettingsSection4")->trigger(); });
        QTimer::singleShot(4700, &app, [window, review_dir] {
            auto *dialog = window->findChild<QWidget *>("settingsDialog");
            dialog->screen()->grabWindow(dialog->winId()).save(review_dir + "/settings.png");
        });
    }
    QTimer::singleShot(!review_dir.isEmpty()                 ? 5200
                       : (expect_remote || expect_first_run) ? 1500
                                                             : 500,
                       &app, &QCoreApplication::quit);
    const auto result = qEnvironmentVariableIsSet("QT_VISUALIZATION_HIDDEN_SMOKE") ? QApplication::exec() : app.exec();
    if (expect_first_run) {
        MainWindow *window = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<MainWindow *>(widget))
                window = candidate;
        auto *surface = window ? window->findChild<QDockWidget *>("spectrogramDock") : nullptr;
        auto *flow = window ? window->findChild<QDockWidget *>("dataFlowDock") : nullptr;
        QString error;
        auto program = pipeline::GraphProgram::compile(app.pipeline().graph(), error);
        if (!surface || !flow || surface->isHidden() || flow->isHidden() ||
            flow->geometry().top() < surface->geometry().bottom() || surface->height() < 180 || flow->height() < 150 ||
            !program || program->capture_source() != 0 ||
            app.pipeline().current_status("Analyzer").state != pipeline::State::Running ||
            app.pipeline().current_status("Capture").state == pipeline::State::Stopped) {
            qCritical("First run did not open the default-source workspace and request synthetic processing");
            return 1;
        }
        qInfo("Fresh configuration: default source with synthetic adapter, Analyzer running, Spectrogram above Pipeline");
    }
    if (expect_remote && (!app.presenter().is_remote() || spectra < 10)) {
        qCritical("Remote GUI did not receive spectra: %d", spectra);
        return 1;
    }
    if (qEnvironmentVariableIsSet("QT_VISUALIZATION_EXPECT_TEST_SIGNAL")) {
        QString error;
        const auto engine_graph = pipeline::GraphProgram::compile(app.pipeline().graph(), error);
        auto *editor = app.findChild<SchemeEditorWidget *>();
        if (!editor)
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *candidate = widget->findChild<SchemeEditorWidget *>())
                    editor = candidate;
        const auto editor_document = editor ? editor->document() : app._ctx.file_configs.scene_graph;
        const auto editor_graph = pipeline::GraphProgram::compile(editor_document, error);
        if (spectra < 10 || !engine_graph || !editor_graph || engine_graph->capture_source() != 2 ||
            !editor_graph->same_processing_as(*engine_graph) ||
            app.pipeline().capture_diagnostics().backend != "In-memory test generator") {
            qCritical(
                "Silent GUI pipeline failed: spectra=%d engine=%d editor=%d source=%d same=%d backend=%s error=%s",
                spectra, bool(engine_graph), bool(editor_graph), engine_graph ? engine_graph->capture_source() : -1,
                engine_graph && editor_graph && editor_graph->same_processing_as(*engine_graph),
                qPrintable(app.pipeline().capture_diagnostics().backend), qPrintable(error));
            qCritical().noquote() << "Engine graph:" << QJsonDocument(app.pipeline().graph()).toJson(QJsonDocument::Compact);
            qCritical().noquote() << "Editor graph:" << QJsonDocument(editor_document).toJson(QJsonDocument::Compact);
            return 1;
        }
    }
    if (expect_remote)
        qInfo("Remote GUI received %d spectra from the console host", spectra);
    return result;
}
