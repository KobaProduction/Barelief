#include "synthetic_capture.h"
#include "shipped_examples_probe.h"
#include "shipped_examples_gui_probe.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStatusBar>
#include <QMenu>
#include <QMenuBar>
#include <QToolButton>
#include <QProcess>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
#include <limits>
#include <bit>
#include <cmath>
#include <functional>
#include <algorithm>

static bool exact_policy(const domain::NumericProcessingGraph& graph) {
    if(graph.nodes.size()!=1)return false;
    const auto* parameters=std::get_if<domain::ConverterProcessingParameters>(&graph.nodes[0].parameters);
    if(!parameters)return false;
    const auto& p=parameters->policy;
    const auto* gain=std::get_if<std::uint64_t>(&p.scaling.gain.numerator);
    const auto* offset=std::get_if<std::int64_t>(&p.scaling.offset.numerator);
    const auto* nan=std::get_if<float>(&p.nan_replacement);
    const auto* minus_zero=std::get_if<double>(&p.negative_inf_replacement);
    return gain && *gain==18446744073709551615ULL && offset && *offset==std::numeric_limits<std::int64_t>::min()
        && nan && std::bit_cast<std::uint32_t>(*nan)==0x7fc00001u
        && minus_zero && std::bit_cast<std::uint64_t>(*minus_zero)==0x8000000000000000ULL;
}

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); return 1; } } while(false)
static QByteArray read(const QString& path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
static bool wait_until(const std::function<bool()>& predicate,int timeout=3000) {
    QElapsedTimer timer;timer.start();while(timer.elapsed()<timeout){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(5);}return predicate();
}
int main(int argc, char** argv) {
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto path=directory.filePath("project.json"), next=directory.filePath("saved.json");
    if(argc==3 && QByteArray(argv[1])=="--verify-project") {
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        const QString source=QString::fromLocal8Bit(argv[2]);const auto original=read(source);
        CHECK(!original.isEmpty() && QFile::copy(source,path));
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        app._ctx.file_configs.ui.autosave_enabled=false;
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        domain::NumericProcessingGraph graph;QString error;CHECK(model->export_numeric_graph(graph,&error));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending() && app.presenter().saved_graph_warning().isEmpty() && app.presenter().client().snapshot().statuses.value("analyzer").state==pipeline::State::Running;}));
        pipeline::DeliveredNumericFrame frame;std::uint64_t ordinal=0,filled=0;
        CHECK(wait_until([&]{return app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},frame,&ordinal,&filled)==signal_contracts::Status::ok && ordinal>10;}));
        const auto view=std::get<signal_contracts::NumericView>(frame.frame.view().payload);
        CHECK(view.actual[0]==400 && filled<=400 && model->delegateModel<AudioNodeModel>(17)->numeric_node()->kind=="SpectrogramView");
        CHECK(read(source)==original);
        std::puts("Repaired project actual editor/engine/400-row History acceptance PASS; source file untouched by verification");return 0;
    }
    if(argc>1 && (QByteArray(argv[1])=="--startup-menu-empty"||QByteArray(argv[1])=="--startup-menu-old")) {
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        const bool old_builtin=QByteArray(argv[1])=="--startup-menu-old";
        desktop::NumericProjectDocument fixture;
        desktop::NumericDraftPresentation display;
        display.application_settings=QJsonObject{{"ui",QJsonObject{{"autosave_enabled",false},{"pipeline_auto_apply",false},
            {"show_raw_samples",false},{"show_amplitudes",false},{"show_test_amplitudes",false},{"show_spectrogram",false}}}};
        if(old_builtin) {
            CHECK(desktop::create_numeric_startup_project(numeric_test_graph(),fixture));
            CHECK(desktop::stage_numeric_presentation(fixture,display,fixture));
        } else CHECK(desktop::create_numeric_project(numeric_test_graph(),display,fixture));
        CHECK(desktop::save_numeric_project(fixture,path));const auto original_file=read(path);
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::startup_capture_factory());
        MainWindow* window=nullptr;for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(window&&model);window->show();
        const auto stored_presets=app._ctx.file_configs.ui.pipeline_presets;
        CHECK(stored_presets.size()==(old_builtin?1:0));CHECK(model->numeric_graph().nodes.size()==13);
        if(old_builtin)CHECK(stored_presets[0].toObject()["graph"].toObject()["nodes"].toArray().size()==13);
        CHECK(!app._ctx.file_configs.ui.autosave_enabled&&!app._ctx.file_configs.ui.pipeline_auto_apply);
        CHECK(app.presenter().client().snapshot().graph_revision==0);
        auto* demo=window->findChild<QAction*>("generatorLoopbackPreset");CHECK(demo&&demo->isEnabled()&&demo->isVisible());
        auto* menu=window->findChild<QMenu*>("pipelinePresetsMenu");auto* display_menu=window->findChild<QMenu*>("graphsMenu");
        auto* presets_button=window->findChild<QToolButton*>("pipelinePresets");
        CHECK(menu&&display_menu&&presets_button&&menu->actions().contains(demo));
        CHECK(display_menu->actions().contains(menu->menuAction())&&window->menuBar()->actions().contains(display_menu->menuAction()));
        CHECK(display_menu->menuAction()->isVisible()&&menu->menuAction()->isVisible());
        CHECK(presets_button->menu()==menu&&presets_button->isVisible());
        auto* language=window->findChild<QComboBox*>("interfaceLanguage");CHECK(language);
        const auto original_language=language->currentIndex();
        for(int locale:{0,1}) {
            language->setCurrentIndex(locale);QCoreApplication::processEvents();
            CHECK(demo->text()==(locale==0?QString::fromUtf8("Генератор и loopback"):QStringLiteral("Generator and loopback")));
            CHECK(menu->title()==(locale==0?QString::fromUtf8("Пресеты"):QStringLiteral("Presets")));
            CHECK(display_menu->title()==(locale==0?QString::fromUtf8("Отображение"):QStringLiteral("Display")));
        }
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();}));const auto startup_revision=app.presenter().client().snapshot().graph_revision;
        language->setCurrentIndex(original_language);demo->trigger();
        CHECK(model->numeric_graph().nodes.size()==15&&model->numeric_graph().edges.size()==14);
        CHECK(app._ctx.file_configs.ui.pipeline_presets==stored_presets&&read(path)==original_file);
        CHECK(app._ctx.file_configs.ui.show_raw_samples&&app._ctx.file_configs.ui.show_amplitudes&&app._ctx.file_configs.ui.show_test_amplitudes&&app._ctx.file_configs.ui.show_spectrogram);
        CHECK(wait_until([&]{QSet<QPair<int,int>> positions;for(auto id:model->allNodeIds()){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}return positions.size()==15;}));
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>startup_revision && !app.presenter().graph_request_pending();}));
        CHECK(verify_startup_selector(app,window,false));
        CHECK(app._ctx.file_configs.ui.pipeline_presets==stored_presets&&read(path)==original_file);
        std::printf("unconditional generatorLoopbackPreset existing config old_builtin=%d: actual14 draft/layout/views/Run/lever/card/History; original file and stored presets unchanged PASS\n",int(old_builtin));return 0;
    }
    if(argc>1 && (QByteArray(argv[1])=="--startup-selector"||QByteArray(argv[1])=="--startup-selector-unavailable")) {
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        const bool unavailable=QByteArray(argv[1])=="--startup-selector-unavailable";
        QByteArray original;
        if(unavailable){
            desktop::NumericProjectDocument fixture;desktop::NumericDraftPresentation display;
            display.application_settings=QJsonObject{{"ui",QJsonObject{{"autosave_enabled",false},{"pipeline_auto_apply",false},{"show_raw_samples",true},{"show_amplitudes",true},{"show_test_amplitudes",true},{"show_spectrogram",true}}}};
            for(const auto& node:numeric_startup_graph().nodes)
                display.positions[QString::number(node.id)]=QJsonObject{{"x",double(node.id%4)*420},{"y",double(node.id/4)*260}};
            CHECK(desktop::create_numeric_startup_project(numeric_startup_graph(),fixture));
            CHECK(desktop::stage_numeric_presentation(fixture,display,fixture));CHECK(desktop::save_numeric_project(fixture,path));original=read(path);
        }
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::startup_capture_factory(unavailable));
        MainWindow* window=nullptr;for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();
        auto* autosave=window->findChild<QCheckBox*>("enableAutosave");CHECK(autosave);autosave->setChecked(false);
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();},6000));
        CHECK(verify_startup_selector(app,window,unavailable));CHECK(unavailable?read(path)==original:!QFile::exists(path));return 0;
    }
    if(argc>1 && (QByteArray(argv[1])=="--shipped-examples"||QByteArray(argv[1])=="--shipped-examples-ui")) {
        const bool gui_workflow=QByteArray(argv[1])=="--shipped-examples-ui";
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        app._ctx.file_configs.ui.autosave_enabled=false;app._ctx.file_configs.ui.pipeline_auto_apply=false;
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        MainWindow* window=nullptr;for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();auto* editor=window->findChild<SchemeEditorWidget*>();CHECK(editor);
        auto* automatic=window->findChild<QCheckBox*>("pipelineAutoApply");auto* autosave=window->findChild<QCheckBox*>("enableAutosave");
        CHECK(!automatic&&autosave);autosave->setChecked(false);
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();},5000));
        CHECK(app.presenter().client().snapshot().statuses.value("analyzer").state==pipeline::State::Running);
        // Exercise the actual editor admission after both JSON persistence
        // boundaries, independently of whichever draft is currently displayed.
        for(const auto& shipped:{numeric_default_graph(),numeric_test_graph(),numeric_startup_graph()}) {
            desktop::NumericProjectDocument project;
            CHECK(desktop::create_numeric_startup_project(shipped,project));
            QJsonObject settings;
            CHECK(desktop::read_numeric_application_settings(project,settings));
            const auto presets=settings["ui"].toObject()["pipeline_presets"].toArray();
            CHECK(presets.size()==1);
            domain::NumericProcessingGraph persisted;
            CHECK(desktop::decode_numeric_graph(QJsonDocument(presets[0].toObject()["graph"].toObject()).toJson(),persisted));
            NumericEditorGraphModel candidate(model->dataModelRegistry());QString diagnostic;
            CHECK(candidate.set_numeric_graph(persisted,&diagnostic));
            CHECK(candidate.numeric_graph().edges.size()==shipped.edges.size());
            QByteArray before;CHECK(desktop::encode_numeric_graph(candidate.numeric_graph(),before));
            // A pre-rank-restriction user draft could contain a Matrix MelView.
            // Reject it atomically and identify the stable offending endpoints.
            auto invalid=persisted;auto invalid_view=invalid.nodes[7];
            invalid_view.id=17;invalid_view.kind="MelView";invalid.nodes.push_back(std::move(invalid_view));
            invalid.edges.push_back({7,17,0,0});
            CHECK(!candidate.set_numeric_graph(invalid,&diagnostic));
            CHECK(diagnostic.contains("[7]")&&diagnostic.contains("[17]"));
            QByteArray after;CHECK(desktop::encode_numeric_graph(candidate.numeric_graph(),after));CHECK(after==before);
        }
        auto inspect=[&](const char* label,pipeline::AudioSourceKind source){
            domain::NumericProcessingGraph graph;QString error;if(!editor->numeric_draft(graph,&error)){std::fprintf(stderr,"%s draft: %s\n",label,qPrintable(error));return false;}
            const bool dual=graph.nodes.size()==15;
            if(graph.nodes.size()!=(dual?15:13)||graph.edges.size()!=(dual?14:12))return false;
            QSet<QPair<int,int>> positions;for(auto id:model->allNodeIds()){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}
            if(positions.size()!=int(graph.nodes.size())){std::fprintf(stderr,"%s overlapping startup positions: %d\n",label,int(positions.size()));return false;}
            return measure_shipped_example(graph,label,source)&&(!gui_workflow||measure_shipped_gui(app,window,label));
        };
        CHECK(wait_until([&]{QSet<QPair<int,int>> positions;for(auto id:model->allNodeIds()){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}return positions.size()==15;}));
        CHECK(inspect("missing-config",pipeline::AudioSourceKind::TestSignal));
        auto* defaults=window->findChild<QAction*>("defaultPipeline");CHECK(defaults);defaults->trigger();QCoreApplication::processEvents();
        CHECK(model->numeric_graph().nodes.front().kind=="AudioSource");
        CHECK(inspect("Default",pipeline::AudioSourceKind::SystemOutput));
        const auto original_edges=model->numeric_graph().edges;
        auto* generator=window->findChild<QAction*>("useTestSource");CHECK(generator);generator->trigger();QCoreApplication::processEvents();
        CHECK(model->numeric_graph().nodes.front().kind=="TestSignal");
        CHECK(model->numeric_graph().edges.size()==original_edges.size());
        const auto replaced_edges=model->numeric_graph().edges;
        CHECK(std::is_permutation(original_edges.begin(),original_edges.end(),replaced_edges.begin(),replaced_edges.end()));
        CHECK(inspect("Generator-example",pipeline::AudioSourceKind::TestSignal));
        CHECK(editor->load_preset("builtin-test-signal"));QCoreApplication::processEvents();
        CHECK(inspect("My-presets/builtin-test-signal",pipeline::AudioSourceKind::TestSignal));
        CHECK(!QFile::exists(path));
        if(gui_workflow)CHECK(verify_example_autosave(app,window,path));
        std::puts("all shipped examples actual UI selection / synthetic full-chain Apply Run PASS");return 0;
    }
    if(argc>1 && QByteArray(argv[1])=="--first-run") {
        CHECK(!QFile::exists(path));auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        const auto graph=model->numeric_graph();CHECK(graph.nodes.size()==15 && graph.edges.size()==14);
        CHECK(graph.nodes[12].id==13 && graph.nodes[12].kind=="AudioSource");
        CHECK(graph.nodes[13].id==14 && graph.nodes[13].kind=="InputSelector");
        CHECK(graph.nodes[13].inputs.size()==2 && graph.nodes[13].outputs.size()==1);
        CHECK(graph.nodes[0].id==1 && graph.nodes[0].kind=="TestSignal");
        const auto& params=std::get<domain::EndpointProcessingParameters>(graph.nodes[0].parameters).values;
        bool eight_tones=false;for(const auto& parameter:params)if(parameter.key=="tones")eight_tones=std::get<std::uint64_t>(std::get<signal_contracts::NumericLiteral>(parameter.value))==8;
        CHECK(eight_tones && numeric_default_graph().nodes[0].kind=="AudioSource");
        CHECK(app._ctx.file_configs.ui.pipeline_presets.size()==1);
        CHECK(app._ctx.file_configs.ui.pipeline_presets[0].toObject()["id"]=="builtin-test-signal");
        CHECK(app._ctx.file_configs.ui.show_raw_samples && app._ctx.file_configs.ui.show_amplitudes && app._ctx.file_configs.ui.show_test_amplitudes && app._ctx.file_configs.ui.show_spectrogram);
        CHECK(app.presenter().file_protection_warning().isEmpty() && app.presenter().client().snapshot().graph_revision==0);
        MainWindow* window=nullptr;for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();
        bool accepted=false,rejected=false;int pcm_frames=0;bool nonzero=false;
        QObject::connect(&app.presenter(),&GuiPresenter::graph_apply_finished,&app,[&](quint64,bool ok,const QString& error){accepted|=ok;rejected|=!ok;if(!ok)std::fprintf(stderr,"first-run Apply: %s\n",qPrintable(error));});
        QObject::connect(&app.presenter(),&GuiPresenter::numeric_frame_ready,&app,[&](const auto& delivered){
            if(!delivered.context||delivered.context->endpoint.node!=1||!delivered.frame)return;
            const auto frame=delivered.frame.view();const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload);if(!numeric||!frame.stamp.available)return;
            float value=0;if(signal_contracts::load_element(*numeric,{0,100,0},value)==signal_contracts::Status::ok){++pcm_frames;nonzero|=std::abs(value)>1e-6f;}
        });
        CHECK(wait_until([&]{QSet<QPair<int,int>> positions;for(auto id:model->allNodeIds()){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}return positions.size()==15;}));
        CHECK(wait_until([&]{return rejected||(accepted && pcm_frames>=3 && nonzero);},4000));CHECK(accepted && !rejected && pcm_frames>=3 && nonzero);
        std::puts("missing-config TestSignal preset/layout/automatic demo/live delivery PASS");return 0;
    }
    if (argc==3 && QByteArray(argv[1])=="--reload") {
        auto encoded=QByteArray(argv[2]); char executable[]="numeric_application_tests", option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr}; int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        app._ctx.file_configs.ui.autosave_enabled=false;
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
        CHECK(model && model->has_numeric_nodes() && model->nodeExists(7));
        const auto graph=model->numeric_graph(); CHECK(graph.nodes.size()==1);
        CHECK(std::get<std::uint64_t>(std::get<domain::ConverterProcessingParameters>(graph.nodes[0].parameters).policy.scaling.gain.numerator)==std::numeric_limits<std::uint64_t>::max());
        CHECK(exact_policy(graph));
        CHECK(model->delegateModel<AudioNodeModel>(7)->presentation()["inline_parameters"].toArray()==QJsonArray{"enabled"});
        CHECK(app._ctx.file_configs.ui.pipeline_presets.size()==2);
        CHECK(read(QString::fromLocal8Bit(encoded)).contains("\"opaque\":1e9999"));
        CHECK(app.presenter().client().snapshot().graph_revision==0);
        std::puts("numeric preset source restart PASS"); return 0;
    }
    if (argc>1 && QByteArray(argv[1])=="--rejected") {
        const QByteArray rejected=R"({"schema_version":2,"scene_graph":{"version":3,"processing_version":2,"nodes":"invalid","connections":[]}})";
        QFile file(path); CHECK(file.open(QIODevice::WriteOnly)); file.write(rejected); file.close();
        auto encoded=path.toLocal8Bit(); char executable[]="numeric_application_tests", option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr}; int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        CHECK(app._ctx.data_flow_graph->allNodeIds().empty());
        CHECK(app.presenter().client().snapshot().graph_revision==0);
        CHECK(!app.presenter().file_protection_warning().isEmpty());
        MainWindow* diagnosticWindow=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))diagnosticWindow=candidate;
        CHECK(diagnosticWindow);
        auto* diagnosticEditor=diagnosticWindow->findChild<SchemeEditorWidget*>();CHECK(diagnosticEditor);
        auto* warning=diagnosticEditor->toolbar()->findChild<QLabel*>("pipelineValidation");
        CHECK(warning && !warning->isHidden() && warning->toolTip().contains(app.presenter().file_protection_warning()));
        CHECK(!diagnosticWindow->findChild<QLabel*>("savedPipelineWarning") && !diagnosticWindow->findChild<QLabel*>("graphApplyStatus"));
        diagnosticWindow->statusBar()->clearMessage();
        CHECK(!warning->isHidden() && !warning->toolTip().isEmpty());
        app.save_configuration_as(next);
        CHECK(!QFile::exists(next) && read(path)==rejected);
        std::puts("numeric rejected startup PASS"); return 0;
    }
    desktop::NumericProjectDocument document;
    CHECK(desktop::read_numeric_project(R"({"schema_version":2,"opaque":{"exact":1e9999,"integer":18446744073709551615},"ui":{"autosave_enabled":true,"show_spectrogram":false,"pipeline_presets":[{"version":1,"id":"opaque-preset","name":"opaque","opaque":1e9999,"graph":{"version":3,"processing_version":2,"graph_id":"main","nodes":[],"connections":[]}}],"editor_view":{"version":1,"scale":1,"center_x":123,"center_y":456,"extension":1e9999}},"pipeline_graph":{"version":3,"processing_version":2,"graph_id":"main","nodes":[],"connections":[],"extension":7}})",document));
    domain::NumericProcessingGraph graph; graph.graph_id="main";
    auto node=numeric_editor::default_node("Converter"); node.id=7;
    std::get<domain::ConverterProcessingParameters>(node.parameters).policy.scaling.gain.numerator=std::numeric_limits<std::uint64_t>::max();
    auto& exact=std::get<domain::ConverterProcessingParameters>(node.parameters).policy;
    exact.scaling.offset.numerator=std::numeric_limits<std::int64_t>::min();
    exact.nan_replacement=std::bit_cast<float>(std::uint32_t{0x7fc00001});
    exact.negative_inf_replacement=std::bit_cast<double>(std::uint64_t{0x8000000000000000});
    graph.nodes.push_back(node);
    ConfigManager manager(path); QString error;
    CHECK(manager.saveNumeric(document,graph,&error));
    desktop::NumericDraftPresentation presentation;
    presentation.positions["7"]=QJsonObject{{"x",321},{"y",654}};
    presentation.node_presentations["7"]=QJsonObject{{"inline_parameters",QJsonArray{"enabled"}}};
    desktop::NumericProjectDocument positioned;
    CHECK(desktop::stage_numeric_presentation(document,presentation,positioned));
    auto positioned_bytes=QByteArray(positioned.source.bytes().data(),static_cast<qsizetype>(positioned.source.bytes().size()));
    positioned_bytes.replace("\"presentation\":{","\"presentation\":{\"opaque_flags\":9007199254740993,");
    CHECK(desktop::read_numeric_project(positioned_bytes,positioned));
    CHECK(desktop::save_numeric_project(positioned,path));
    auto encoded=path.toLocal8Bit(); char executable[]="numeric_application_tests", option[]="--config";
    char* arguments[]={executable,option,encoded.data(),nullptr}; int count=3;
    CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    CHECK(model && model->has_numeric_nodes() && model->nodeExists(7));
    CHECK(!app._ctx.file_configs.ui.show_spectrogram);
    MainWindow* window=nullptr;
    // The composition owns the window; the model deliberately has no QWidget parent.
    for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
    CHECK(window);
    auto* editor=window->findChild<SchemeEditorWidget*>(); CHECK(editor);
    CHECK(model->nodeData(7,QtNodes::NodeRole::Position).toPointF()==QPointF(321,654));
    CHECK(model->delegateModel<AudioNodeModel>(7)->presentation()["inline_parameters"].toArray()==QJsonArray{"enabled"});
    CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();}));const auto startup_revision=app.presenter().client().snapshot().graph_revision;
    const auto saved_layout_bytes=read(path);QCoreApplication::processEvents();
    CHECK(model->nodeData(7,QtNodes::NodeRole::Position).toPointF()==QPointF(321,654));CHECK(read(path)==saved_layout_bytes);
    CHECK(app.presenter().client().snapshot().graph_revision==startup_revision);
    CHECK(editor->numeric_draft(graph,&error));
    CHECK(exact_policy(graph));
    CHECK(std::get<std::uint64_t>(std::get<domain::ConverterProcessingParameters>(graph.nodes[0].parameters).policy.scaling.gain.numerator)==std::numeric_limits<std::uint64_t>::max());
    graph.nodes[0].enabled=false;
    CHECK(editor->load_numeric_draft(graph,&error));
    // Model keeps unknown visual/preset fields; the application must save only
    // registered flags and let storage retain the original raw scene metadata.
    model->delegateModel<AudioNodeModel>(7)->set_presentation(QJsonObject{{"inline_parameters",QJsonArray{"enabled"}},{"future_visual_flag",9}});
    model->setNodeData(7,QtNodes::NodeRole::Position,QPointF(99,88));
    app._ctx.editor_draft_modified=true;
    emit editor->numeric_draft_changed(); // Actual autosave callback, not execution.
    CHECK(manager.loadNumeric(document,graph,&error)); CHECK(!graph.nodes[0].enabled);
    CHECK(read(path).contains("1e9999") && read(path).contains("18446744073709551615"));
    CHECK(desktop::read_numeric_presentation(document,presentation));
    CHECK(presentation.positions["7"].toObject()["x"].toDouble()==99);
    CHECK(presentation.node_presentations["7"].toObject()["inline_parameters"].toArray()==QJsonArray{"enabled"});
    CHECK(read(path).contains("\"opaque_flags\":9007199254740993"));
    app._ctx.file_configs.ui.language="en";
    app._ctx.file_configs.ui.camera=QJsonObject{{"x",42}};
    emit window->on_config_updated(); // Manual Save converges on the same path.
    CHECK(manager.loadNumeric(document,graph,&error));
    FileSaveApplicationConfig settings;
    CHECK(manager.readNumericSettings(document,settings,&error));
    CHECK(settings.ui.language=="en" && settings.ui.camera["x"].toDouble()==42);
    // Actual editor operations reconcile existing preset identities and opaque
    // source while preserving exact typed node snapshots.
    CHECK(editor->save_preset("first"));
    CHECK(manager.loadNumeric(document,graph,&error));
    CHECK(manager.readNumericSettings(document,settings,&error));
    CHECK(settings.ui.pipeline_presets.size()==2);
    const auto first_bytes=read(path);
    CHECK(editor->save_preset("second"));
    CHECK(read(path)!=first_bytes);
    CHECK(manager.loadNumeric(document,graph,&error));
    CHECK(manager.readNumericSettings(document,settings,&error));
    CHECK(settings.ui.pipeline_presets.size()==3);
    app._ctx.file_configs.ui.pipeline_presets=settings.ui.pipeline_presets;
    QString first_id;
    for(const auto& item:settings.ui.pipeline_presets)if(item.toObject()["name"].toString()=="first")first_id=item.toObject()["id"].toString();
    CHECK(!first_id.isEmpty() && editor->load_preset(first_id));
    CHECK(editor->numeric_draft(graph,&error)); CHECK(graph.nodes.size()==1);
    CHECK(exact_policy(graph));
    CHECK(model->delegateModel<AudioNodeModel>(7)->presentation()["inline_parameters"].toArray()==QJsonArray{"enabled"});
    CHECK(std::get<std::uint64_t>(std::get<domain::ConverterProcessingParameters>(graph.nodes[0].parameters).policy.scaling.gain.numerator)==std::numeric_limits<std::uint64_t>::max());
    CHECK(editor->remove_preset(first_id));
    CHECK(editor->save_preset("repeat"));
    CHECK(manager.loadNumeric(document,graph,&error));
    CHECK(manager.readNumericSettings(document,settings,&error));
    CHECK(settings.ui.pipeline_presets.size()==3);
    QString repeat_id;
    for(const auto& item:settings.ui.pipeline_presets)if(item.toObject()["name"].toString()=="repeat")repeat_id=item.toObject()["id"].toString();
    CHECK(!repeat_id.isEmpty() && editor->load_preset(repeat_id));
    CHECK(editor->remove_preset(repeat_id));
    app.save_configuration_as(next);
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error));
    CHECK(exact_policy(graph));
    std::optional<domain::NumericProcessingGraph> applied;
    CHECK(desktop::read_numeric_applied(document,applied));CHECK(applied && exact_policy(*applied));
    CHECK(read(next).contains("1e9999") && read(next).contains("\"extension\":7"));
    CHECK(ConfigManager(next).readNumericSettings(document,settings,&error));
    CHECK(settings.ui.pipeline_presets.size()==2);
    const auto before_restart=read(next);
    QProcess restart;
    restart.start(QCoreApplication::applicationFilePath(),{"--reload",next});
    CHECK(restart.waitForFinished(6000));
    if(restart.exitCode()!=0)std::fprintf(stderr,"restart: %s\n",restart.readAllStandardError().constData());
    CHECK(restart.exitStatus()==QProcess::NormalExit && restart.exitCode()==0);
    CHECK(read(next)==before_restart);
    CHECK(app.presenter().client().snapshot().graph_revision==startup_revision);
    graph.nodes.clear(); CHECK(editor->load_numeric_draft(graph,&error));
    emit window->on_config_updated();
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error)); CHECK(graph.nodes.empty());
    QFile external(next); CHECK(external.open(QIODevice::WriteOnly)); external.write("{\"schema_version\":100}"); external.close();
    app._ctx.editor_draft_modified=true; emit window->on_config_updated();
    CHECK(read(next)=="{\"schema_version\":100}"); CHECK(app._ctx.editor_draft_modified);
    CHECK(app.presenter().client().snapshot().graph_revision==startup_revision);
    std::puts("canonical numeric application/presets/restart PASS"); return 0;
}
