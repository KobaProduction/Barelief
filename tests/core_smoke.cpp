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
#include <DockAreaWidget.h>
#include <QDir>
#include <QOpenGLWidget>
#include <QProcess>
#include <QProcessEnvironment>
#include <cstring>
#include <QStatusBar>
static int configuration_startup_cases(int &argc,char **argv) {
    QCoreApplication parent(argc,argv);
    QTemporaryDir directory;
    const QList<QByteArray> rejected{
        R"({"schema_version":2,"opaque":[1,2]})",R"({"schema_version":null})",
        R"({"schema_version":"1"})",R"({broken-json)",
        R"({"schema_version":1,"devices":{"remote":{"presets":[{"id":"stable","graph":{"version":99}}]}}})"};
    for(int i=0;i<=rejected.size();++i) {
        const bool missing=i==rejected.size();const auto path=directory.filePath(QString::number(i)+".json");
        QFile file(path);
        if(!missing){if(!file.open(QIODevice::WriteOnly))return 1;file.write(rejected[i]);file.close();}
        QProcess child;auto env=QProcessEnvironment::systemEnvironment();
        for(const auto &key:QStringList{"QT_VISUALIZATION_SCHEMA_REGRESSION","QT_VISUALIZATION_EXPECT_FIRST_RUN",
                                       "QT_VISUALIZATION_EXPECT_TEST_SIGNAL","QT_VISUALIZATION_EXPECT_REMOTE","QT_VIS_UI_REVIEW_DIR"})env.remove(key);
        env.insert("QT_QPA_PLATFORM","offscreen");env.insert("QT_FORCE_STDERR_LOGGING","1");
        env.insert("QT_VISUALIZATION_CONFIGURATION_STARTUP_CASE",missing?"missing":"rejected");child.setProcessEnvironment(env);
        child.start(parent.applicationFilePath(),{"--config",path});
        if(!child.waitForFinished(6000) || child.exitStatus()!=QProcess::NormalExit || child.exitCode()!=0) {
            qCritical().noquote()<<"Configuration startup child failed:"<<child.readAllStandardError()<<child.readAllStandardOutput();return 1;
        }
        if(!missing){if(!file.open(QIODevice::ReadOnly) || file.readAll()!=rejected[i])return 1;file.close();}
    }
    return 0;
}
static int shutdown_save_cases(int &argc,char **argv) {
    QCoreApplication parent(argc,argv);QTemporaryDir directory;
    for(const auto mode:QStringList{"enabled","disabled","protected"}) {
        const bool autosave=mode!="disabled",protected_file=mode=="protected";
        const auto path=directory.filePath(mode+".json");
        QJsonObject config{{"schema_version",1},{"pipeline_graph",pipeline::GraphProgram::test_signal_defaults()},
            {"scene_graph",pipeline::GraphProgram::test_signal_defaults()},
            {"ui",QJsonObject{{"language","ru"},{"autosave_enabled",autosave},{"show_spectrogram",false}}}};
        if(protected_file)config["schema_version"]=2;
        const auto original=QJsonDocument(config).toJson();QFile file(path);
        if(!file.open(QIODevice::WriteOnly))return 1;file.write(original);file.close();
        QProcess child;auto env=QProcessEnvironment::systemEnvironment();
        for(const auto &key:QStringList{"QT_VISUALIZATION_SCHEMA_REGRESSION","QT_VISUALIZATION_EXPECT_FIRST_RUN",
            "QT_VISUALIZATION_EXPECT_TEST_SIGNAL","QT_VISUALIZATION_EXPECT_REMOTE","QT_VISUALIZATION_CONFIGURATION_STARTUP_CASE","QT_VIS_UI_REVIEW_DIR"})env.remove(key);
        env.insert("QT_QPA_PLATFORM","offscreen");env.insert("QT_FORCE_STDERR_LOGGING","1");
        env.insert("QT_VISUALIZATION_SHUTDOWN_SAVE_REGRESSION",mode);child.setProcessEnvironment(env);
        child.start(parent.applicationFilePath(),{"--config",path});
        if(!child.waitForFinished(6000) || child.exitStatus()!=QProcess::NormalExit || child.exitCode()!=0) {
            qCritical().noquote()<<"Exception shutdown child failed:"<<child.readAllStandardError()<<child.readAllStandardOutput();return 1;
        }
        if(!file.open(QIODevice::ReadOnly))return 1;const auto saved=file.readAll();file.close();
        if((!autosave || protected_file) && saved!=original){qCritical("Disabled/protected autosave wrote on unwind");return 1;}
        const auto ui=QJsonDocument::fromJson(saved).object().value("ui").toObject();
        if(autosave && !protected_file && (ui.value("language")!="en" || ui.value("editor_view").toObject().value("shutdown-marker")!=73 ||
            ui.value("editor_view").toObject().value("version")!=1 ||
            ui.value("workspace").toObject().value("version")!=1 ||
            ui.value("workspace").toObject().value("ads_state").toString().isEmpty())) {
            qCritical("Final enabled autosave lost pending preferences/layout on unwind");return 1;
        }
    }
    return 0;
}
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
    require(app.pipeline().graph_revision()==0 &&
                app.pipeline().current_status("Analyzer").state!=pipeline::State::Running &&
                app.pipeline().current_status("Capture").state!=pipeline::State::Running,
            "Incompatible schema silently applied/activated a default pipeline");
    require(!app.presenter().saved_graph_warning().isEmpty(), "Missing incompatible schema diagnostic");
    MainWindow *window = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *candidate = qobject_cast<MainWindow *>(widget))
            window = candidate;
    require(window && !window->findChild<QLabel *>("pipelineValidation")->toolTip().isEmpty(),
            "Bootstrap error is not visible in the UI");
    auto* diagnostic=window->findChild<QLabel *>("pipelineValidation");
    require(diagnostic && diagnostic->parentWidget()!=window->statusBar() &&
                !window->findChild<QLabel *>("savedPipelineWarning"),
            "Schema diagnostic still occupies the global status strip");
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
    // Explicit recovery may change the working graph, but an unsupported source
    // remains protected even after an intentional default Apply.
    require(pipeline::GraphProgram::compile(app.pipeline().graph(), error) != nullptr,
            "Explicit recovery did not apply a valid working graph");
    require(read()==original,"Explicit recovery overwrote the protected future source");
    window->statusBar()->clearMessage(); // Protection must outlive any transient save-failure message.
    const auto warning=window->findChild<QLabel *>("pipelineValidation");
    require(!app.presenter().file_protection_warning().isEmpty() && warning &&
                !warning->isHidden() && warning->toolTip().contains(path),"Protected file warning disappeared after working recovery");
    require(app.presenter().saved_graph_warning().isEmpty(), "Recovery did not clear diagnostic");
    return 0;
}
int main(int argc, char **argv) {
    for(int i=1;i<argc;++i)if(std::strcmp(argv[i],"--configuration-startup-only")==0)return configuration_startup_cases(argc,argv);
    for(int i=1;i<argc;++i)if(std::strcmp(argv[i],"--configuration-shutdown-only")==0)return shutdown_save_cases(argc,argv);
    if(qEnvironmentVariableIsSet("QT_VISUALIZATION_SHUTDOWN_SAVE_REGRESSION")) {
        try {
            CoreApplication app(argc,argv,test_fixture::synthetic_capture_factory());
            app._ctx.file_configs.ui.language="en";
            app._ctx.file_configs.ui.editor_view["shutdown-marker"]=73;
            app._ctx.editor_draft_modified=true;
            throw std::runtime_error("intentional shutdown unwind");
        } catch(const std::runtime_error &error) {
            if(std::strcmp(error.what(),"intentional shutdown unwind")!=0){qCritical("Unexpected shutdown exception: %s",error.what());return 1;}
            return 0;
        }
    }
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
    const auto startup_case=qEnvironmentVariable("QT_VISUALIZATION_CONFIGURATION_STARTUP_CASE");
    if(!startup_case.isEmpty()) {
        const auto initial_revision=app.pipeline().graph_revision();
        QTimer::singleShot(startup_case=="missing"?1500:400,&app,&QCoreApplication::quit);const auto result=app.exec();if(result)return result;
        if(startup_case=="rejected") {
            MainWindow *window=nullptr;for(auto *widget:QApplication::topLevelWidgets())
                if(auto *candidate=qobject_cast<MainWindow *>(widget))window=candidate;
            const auto warning=window?window->findChild<QLabel *>("pipelineValidation"):nullptr;
            if(initial_revision!=0 || app.pipeline().graph_revision()!=0 || app.presenter().graph_request_pending() ||
               app.pipeline().current_status("Analyzer").state==pipeline::State::Running ||
               app.pipeline().current_status("Capture").state==pipeline::State::Running ||
               app.presenter().saved_graph_warning().isEmpty() ||
               !app.presenter().saved_graph_warning().contains(app.arguments().value(app.arguments().indexOf("--config")+1)) ||
               !warning || warning->toolTip().isEmpty() || warning->isHidden()) {
                qCritical("Rejected configuration bootstrapped processing or lost the visible diagnostic");return 1;
            }
        } else {
            QString error;const auto graph=pipeline::GraphProgram::compile(app.pipeline().graph(),error);
            // The default is prepared by the adapter; accepting identical
            // first-run processing can legitimately leave graph revision zero.
            if(!graph || graph->capture_source()!=0 || app.presenter().graph_request_pending() ||
               app.pipeline().current_status("Analyzer").state!=pipeline::State::Running ||
               app.pipeline().capture_diagnostics().backend!="In-memory test generator") {
                qCritical("Missing configuration did not retain first-run synthetic processing");return 1;
            }
        }
        return 0;
    }
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
            auto *dock = window->workspaceDock("fftDock");
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
        auto *surface = window ? window->workspaceDock("spectrogramDock") : nullptr;
        auto *flow = window ? window->workspaceDock("dataFlowDock") : nullptr;
        QString error;
        auto areaRect=[](WorkspaceDock *dock) {
            auto *area=dock->dockAreaWidget();
            return QRect(area->mapTo(area->window(),QPoint()),area->size());
        };
        auto program = pipeline::GraphProgram::compile(app.pipeline().graph(), error);
        if (!surface || !flow || surface->isHidden() || flow->isHidden() ||
            areaRect(flow).top() < areaRect(surface).bottom() || surface->height() < 180 || flow->height() < 150 ||
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
