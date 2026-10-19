#include <desktop/numeric_source_format.h>
#include "synthetic_capture.h"
#include "performance_report.h"
#include <QTimer>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>
#include <QSettings>
#include <core_app/workspace/workspace_window.h>
#include <core_app/workspace/ui_command_registry.h>
#include "shipped_examples_probe.h"
#include "shipped_examples_gui_probe.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/node_parameter_model.h>
#include <core_app/numeric_default_graph.h>
#include <visualization/ui_text.h>
#include <visualization/numeric_piano_view.h>
#include <core_app/graph_panel.h>
#include <visualization/abstract/abs.h>
#include <desktop/numeric_project_document.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
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
#include <QImage>
#include <QPainter>
#include <QFontDatabase>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <cstdio>
#include <limits>
#include <bit>
#include <cmath>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <algorithm>
#include <atomic>

// Capture factories and discovery describe the same deterministic mono device.
// Host hardware must not silently rewrite synthetic acceptance-test contracts.
static void install_synthetic_capture_discovery(CoreApplication& app,unsigned sample_rate=48000,unsigned channels=1) {
    if(auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get()))
        model->set_capture_device_provider([sample_rate,channels](int,QString*) {
            return QJsonArray{QJsonObject{{"id","synthetic-capture"},{"name","Synthetic capture"},
                {"is_default",true},{"native_sample_rate",int(sample_rate)},{"native_channels",int(channels)}}};
        });
}

static domain::NumericProcessingGraph native_mono_example(domain::NumericProcessingGraph graph) {
    for(auto id:{domain::NodeId{13},domain::NodeId{14}})
        if(std::any_of(graph.nodes.begin(),graph.nodes.end(),[=](const auto& node){return node.id==id&&node.kind=="AudioSource";})) {
            QString error;if(!desktop::resolve_numeric_source_format(graph,id,48000,1,numeric_editor::limits(),&error))throw std::runtime_error(error.toStdString());
        }
    return graph;
}
static void export_startup_scenes(SchemeEditorWidget* editor,NumericEditorGraphModel* model) {
    const auto folder=qEnvironmentVariable("NUMERIC_TEST_SCENE_DIR");if(folder.isEmpty())return;
    auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();if(!scene || !QDir().mkpath(folder))return;
    const auto capture=[&](const QString& name,const std::vector<domain::NodeId>& ids){QRectF bounds;
        for(const auto id:ids)if(auto* item=scene->nodeGraphicsObject(QtNodes::NodeId(id)))bounds=bounds.united(item->sceneBoundingRect());
        if(bounds.isEmpty())return;bounds=bounds.adjusted(-25,-25,25,25);const auto scale=std::min(1.,2400./bounds.width());
        QImage image(QSize(qCeil(bounds.width()*scale),qCeil(bounds.height()*scale)),QImage::Format_ARGB32_Premultiplied);image.fill(QApplication::palette().color(QPalette::Window));
        QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing);scene->render(&painter,QRectF(image.rect()),bounds);painter.end();image.save(QDir(folder).filePath(name));
    };
    capture("startup-sources.png",{1,13,14,101,200,250,332,333});
    const auto graph=model->numeric_graph();std::vector<domain::NodeId> spectrum{4};auto next=domain::NodeId{4};
    for(unsigned depth=0;depth<3;++depth){const auto edge=std::find_if(graph.edges.begin(),graph.edges.end(),[&](const auto& e){return e.from==next && e.output_port==0 && e.input_port==0;});if(edge==graph.edges.end())break;next=edge->to;spectrum.push_back(next);}
    capture("startup-fourier.png",spectrum);
}
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
// Answer only this test process's explicit overwrite warning.
static bool respond_to_overwrite(bool accept,const std::function<void()>& save,const QString& expectedTarget={}) {
    QTimer timer;timer.setInterval(5);bool seen=false,defaults=false;
    QObject::connect(&timer,&QTimer::timeout,&timer,[&]{
        for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget);dialog && dialog->objectName()=="confirmConfigurationOverwrite"){
            seen=true;defaults=dialog->defaultButton()==dialog->button(QMessageBox::Cancel) && dialog->escapeButton()==dialog->button(QMessageBox::Cancel) && dialog->icon()==QMessageBox::Warning && dialog->testOption(QMessageBox::Option::DontUseNativeDialog) && (expectedTarget.isEmpty() || dialog->text().contains(expectedTarget));timer.stop();
            auto* button=dialog->button(QMessageBox::Cancel);
            if(accept)for(auto* candidate:dialog->buttons())if(dialog->buttonRole(candidate)==QMessageBox::AcceptRole)button=candidate;
            button->click();return;
        }
    });
    timer.start();save();return seen && defaults;
}
static bool save_without_overwrite(const std::function<void()>& save) {
    QTimer timer;timer.setInterval(5);bool seen=false;
    QObject::connect(&timer,&QTimer::timeout,&timer,[&]{
        for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QMessageBox*>(widget);dialog && dialog->objectName()=="confirmConfigurationOverwrite"){seen=true;dialog->reject();}
    });timer.start();save();return !seen;
}
// The isolated offscreen window cannot own native focus. Supply that context
// explicitly, then invoke the real QAction handler rather than its output signal.
static void trigger_save_action(MainWindow* window,QAction* action) {
    if(auto* commands=window->findChild<presentation::UiCommandRegistry*>())
        commands->set_workspace_guard([]{return true;});
    action->trigger();
}
static QByteArray read(const QString& path) {
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
static bool wait_until(const std::function<bool()>& predicate,int timeout=3000) {
    QElapsedTimer timer;timer.start();while(timer.elapsed()<timeout){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(5);}return predicate();
}
static bool separated_layout(MainWindow* window,NumericEditorGraphModel* model) {
    auto* editor=window->findChild<SchemeEditorWidget*>();if(!editor)return false;
    auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();if(!scene)return false;
    std::vector<QRectF> rectangles;
    for(auto id:model->allNodeIds()) {
        auto* item=scene->nodeGraphicsObject(id);if(!item)return false;
        const auto bounds=item->sceneBoundingRect();if(bounds.isEmpty())return false;
        for(const auto& previous:rectangles)if(bounds.intersects(previous))return false;
        rectangles.push_back(bounds);
    }
    return !rectangles.empty();
}
int main(int argc, char** argv) {
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto path=directory.filePath("project.json"), next=directory.filePath("saved.json");
    if(argc==3 && (QByteArray(argv[1])=="--save-file-copy" || QByteArray(argv[1])=="--fft-file-copy" || QByteArray(argv[1])=="--resampler-file-copy" || QByteArray(argv[1])=="--music-file-copy")) {
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        const auto original=QString::fromLocal8Bit(argv[2]);const auto originalBytes=read(original);
        CHECK(!originalBytes.isEmpty());QFile copy(path);CHECK(copy.open(QIODevice::WriteOnly));CHECK(copy.write(originalBytes)==originalBytes.size());copy.close();
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;CHECK(window);
        auto* editor=window->findChild<SchemeEditorWidget*>();
        std::fprintf(stderr,"SAVE_PROBE editor_in_main_window=%d\n",int(editor!=nullptr));
        if(!editor)for(auto* widget:QApplication::allWidgets())if(auto* candidate=qobject_cast<SchemeEditorWidget*>(widget))editor=candidate;
        CHECK(editor);
        if(QByteArray(argv[1])=="--music-file-copy"){
            desktop::NumericProjectDocument inputDocument;domain::NumericProcessingGraph inputGraph,evaluated,executable;
            CHECK(desktop::load_numeric_project(path,inputDocument,inputGraph));
            const auto admission=domain::resolve_numeric_parameter_bindings(inputGraph,numeric_editor::limits(),evaluated,executable);
            std::fprintf(stderr,"MUSIC_ADMISSION error=%u node=%d port=%u\n",unsigned(admission.error),admission.node,unsigned(admission.port));
            CHECK(bool(admission));
            install_synthetic_capture_discovery(app);window->show();
            CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();},8000));
            const auto notes=std::find_if(inputGraph.nodes.begin(),inputGraph.nodes.end(),[](const auto& node){return node.kind=="NoteSpectrum";});CHECK(notes!=inputGraph.nodes.end());
            const auto route=std::find_if(inputGraph.edges.begin(),inputGraph.edges.end(),[&](const auto& edge){
                if(edge.from!=notes->id || edge.output_port!=0)return false;
                const auto target=std::find_if(inputGraph.nodes.begin(),inputGraph.nodes.end(),[&](const auto& node){return node.id==edge.to;});
                return target!=inputGraph.nodes.end() && target->kind=="Spectrogram";
            });CHECK(route!=inputGraph.edges.end());
            const auto note_id=notes->id,history_id=route->to;
            std::atomic<unsigned> rows=0,failures=0,origins=0;
            const auto tap=app.pipeline().subscribe_numeric_tap({{"main",note_id,domain::PortDirection::Output,0},{"main",note_id,domain::PortDirection::Output,1},{"main",note_id,domain::PortDirection::Output,2},{"main",note_id,domain::PortDirection::Output,3},{"main",history_id,domain::PortDirection::Output,0}},[&](const pipeline::DeliveredNumericFrame& frame){
                if(!frame.context)return;
                if(frame.status!=signal_contracts::Status::ok && frame.status!=signal_contracts::Status::unavailable){
                    if(failures.fetch_add(1)<8)std::fprintf(stderr,"MUSIC_FAILURE node=%d port=%u status=%u\n",int(frame.context->endpoint.node),unsigned(frame.context->endpoint.port),unsigned(frame.status));
                    return;
                }
                if(!frame.frame || !frame.frame.view().stamp.available)return;
                if(frame.context->endpoint.node==history_id)++rows;
                if(frame.context->endpoint.node==note_id && frame.context->endpoint.port==3)++origins;
            },{});CHECK(tap);
            const bool ready=wait_until([&]{return rows>=8 || failures>=8;},8000);
            app.pipeline().unsubscribe_numeric_tap(tap);
            std::fprintf(stderr,"MUSIC_PROBE ready=%d combined_rows=%u note_origins=%u failures=%u\n",int(ready),rows.load(),origins.load(),failures.load());
            CHECK(read(original)==originalBytes);
            CHECK(ready && rows>=8 && origins>=8 && failures==0);
            std::puts("user music copy: notes, explicit MIDI origin and joined rows PASS");return 0;
        }
        if(QByteArray(argv[1])=="--resampler-file-copy"){
            install_synthetic_capture_discovery(app);window->show();
            CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();},8000));
            auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
            auto* resampler=model->delegateModel<AudioNodeModel>(332);CHECK(resampler);
            if(resampler->parameter_model()->bypassed()){
                const auto revision=app.presenter().client().snapshot().graph_revision;
                CHECK(resampler->parameter_model()->setBypassed(false));
                CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>revision && !app.presenter().graph_request_pending();},8000));
            }
            struct Packet {std::vector<float> values;signal_contracts::FrameStamp stamp;unsigned rate=0;};
            std::mutex mutex;std::vector<Packet> packets;unsigned inputFrames=0,inputGaps=0,windowFrames=0,windowGaps=0;std::size_t windowPrefix=0;
            const auto tap=app.pipeline().subscribe_numeric_tap({{"main",200,domain::PortDirection::Output,0},{"main",332,domain::PortDirection::Output,0},{"main",2,domain::PortDirection::Output,0}},[&](const pipeline::DeliveredNumericFrame& frame){
                if(!frame.context || !frame.frame)return;const auto view=frame.frame.view();if(!view.stamp.available)return;
                const auto* values=std::get_if<signal_contracts::NumericView>(&view.payload);if(!values)return;
                std::lock_guard lock(mutex);
                if(frame.context->endpoint.node==200){++inputFrames;inputGaps+=view.stamp.discontinuity;return;}
                if(frame.context->endpoint.node==2){++windowFrames;windowGaps+=view.stamp.discontinuity;windowPrefix=0;
                    for(size_t i=0;i<values->actual[0];++i){float value=0;signal_contracts::load_element(*values,{0,i,0},value);if(std::abs(value)>1e-6)break;++windowPrefix;}return;}
                Packet packet;packet.stamp=view.stamp;packet.rate=values->contract->clock.sample_rate;packet.values.resize(values->actual[0]);
                for(size_t i=0;i<packet.values.size();++i)signal_contracts::load_element(*values,{0,i,0},packet.values[i]);
                if(packets.size()==64)packets.erase(packets.begin());packets.push_back(std::move(packet));
            },{});CHECK(tap);
            auto* dock=window->workspaceDock("rawSamplesDock");CHECK(dock);dock->toggleView(true);dock->raise();
            auto* panel=qobject_cast<GraphPanel*>(dock->widget());CHECK(panel);auto* chart=panel->active_chart();CHECK(chart);chart->resize(1200,400);
            CHECK(wait_until([&]{std::lock_guard lock(mutex);return packets.size()>=32 && chart->projected_point_count()>100;},8000));
            bool matched=false;unsigned gaps=0;const auto points=chart->projected_points();
            {std::lock_guard lock(mutex);for(const auto& packet:packets){gaps+=packet.stamp.discontinuity;
                bool equal=points.size()==packet.values.size();for(size_t i=0;equal && i<packet.values.size();++i)equal=std::abs(points[int(i)].y()-packet.values[i])<1e-9;
                matched|=equal;
            }
            const auto& p=packets.back();size_t prefix=0;while(prefix<p.values.size() && std::abs(p.values[prefix])<1e-6)++prefix;
            std::fprintf(stderr,"RESAMPLER_PROBE inputFrames=%u inputGaps=%u outputFrames=%zu outputGaps=%u prefix=%zu points=%lld matched=%d firstTick=%lld\n",inputFrames,inputGaps,packets.size(),gaps,prefix,static_cast<long long>(points.size()),int(matched),static_cast<long long>(p.stamp.timestamp_ticks));
            for(size_t i=packets.size()>8?packets.size()-8:0;i<packets.size();++i){const auto& packet=packets[i];size_t zeros=0;while(zeros<packet.values.size()&&std::abs(packet.values[zeros])<1e-6)++zeros;
                std::fprintf(stderr,"RESAMPLER_PACKET size=%zu fs=%u prefix=%zu gap=%d seq=%llu src=%llu gen=%llu revision=%llu\n",packet.values.size(),packet.rate,zeros,int(packet.stamp.discontinuity),(unsigned long long)packet.stamp.sequence,(unsigned long long)packet.stamp.source_sequence,(unsigned long long)packet.stamp.generation,(unsigned long long)packet.stamp.graph_revision);
            }}
            std::fprintf(stderr,"WINDOW_PROBE frames=%u gaps=%u prefix=%zu\n",windowFrames,windowGaps,windowPrefix);
            for(const auto item:app.pipeline().snapshot().diagnostics["node_stats"].toArray()){
                const auto stats=item.toObject();if(stats["errors"].toVariant().toULongLong())std::fprintf(stderr,"NODE_ERRORS %s\n",QJsonDocument(stats).toJson(QJsonDocument::Compact).constData());
            }
            const auto before=app.presenter().client().snapshot().graph_revision;
            for(const auto id:{6,1009}){auto* node=model->delegateModel<AudioNodeModel>(id);CHECK(node && node->parameter_model()->setBypassed(true));}
            CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>before && !app.presenter().graph_request_pending();},8000));
            {std::lock_guard lock(mutex);packets.clear();inputFrames=inputGaps=windowFrames=windowGaps=0;windowPrefix=4096;}
            CHECK(wait_until([&]{std::lock_guard lock(mutex);return packets.size()>=64 && windowFrames>=16;},8000));
            {std::lock_guard lock(mutex);std::fprintf(stderr,"DOWNSTREAM_BYPASS inputFrames=%u inputGaps=%u windowFrames=%u windowGaps=%u prefix=%zu\n",inputFrames,inputGaps,windowFrames,windowGaps,windowPrefix);}
            app.pipeline().unsubscribe_numeric_tap(tap);CHECK(matched);CHECK(read(original)==originalBytes);
            std::puts("resampler/user chart raw frame comparison PASS");return 0;
        }
        if(QByteArray(argv[1])=="--fft-file-copy"){
            install_synthetic_capture_discovery(app);window->show();
            CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();},8000));
            auto* dock=window->workspaceDock("fftDock");CHECK(dock);dock->toggleView(true);dock->raise();
            auto* panel=qobject_cast<GraphPanel*>(dock->widget());CHECK(panel);
            CHECK(wait_until([&]{return panel->active_chart()&&panel->active_chart()->value_count()==2048;},8000));
            auto* chart=panel->active_chart();
            std::fprintf(stderr,"FFT_PROBE values=%zu peak=%g x=%g..%g y=%g..%g diagnostic=%s\n",chart->value_count(),double(chart->peak_value()),chart->projected_x_coordinate(0),chart->projected_x_coordinate(2047),chart->y_range().first,chart->y_range().second,qPrintable(panel->diagnostic()));
            CHECK(chart->peak_value()>0 && chart->y_range().first<0 && chart->bar_baseline_minimum());
            CHECK(wait_until([&]{const auto points=chart->projected_points();return std::any_of(points.begin(),points.end(),[](const auto& p){return p.y()>-110 && p.y()<0;});},8000));
            CHECK(std::abs(chart->projected_x_coordinate(0)-37000./4096)<1e-8 && std::abs(chart->projected_x_coordinate(2047)-18500)<1e-8);
            CHECK(read(original)==originalBytes);std::puts("user FFT copy: live PSD dB values, signed Y range and first/last frequency PASS");return 0;
        }
        domain::NumericProcessingGraph draft;QString error;
        const bool exported=editor->numeric_draft(draft,&error,false);
        std::fprintf(stderr,"SAVE_PROBE export=%d nodes=%zu error=%s\n",int(exported),draft.nodes.size(),error.toUtf8().constData());
        auto* action=window->findChild<QAction*>("saveSettings");CHECK(action);
        CHECK(save_without_overwrite([&]{trigger_save_action(window,action);}));
        auto* status=window->findChild<QLabel*>("configurationSaveStatus");CHECK(status);
        std::fprintf(stderr,"SAVE_PROBE state=%d detail=%s\n",status->property("saveState").toInt(),status->toolTip().toUtf8().constData());
        CHECK(read(original)==originalBytes);
        CHECK(status->property("saveState").toInt()==int(MainWindow::ConfigurationSaveState::Saved));
        desktop::NumericProjectDocument saved;domain::NumericProcessingGraph loaded;CHECK(desktop::load_numeric_project(path,saved,loaded));
        CHECK(loaded.nodes.size()==draft.nodes.size());std::puts("save user project copy PASS");return 0;
    }
    if(argc==2 && QByteArray(argv[1])=="--workspace-persistence"){
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");QCoreApplication controller(argc,argv);
        auto graph=native_mono_example(numeric_startup_graph());graph.execution_interval={1,10};
        for(auto& node:graph.nodes)if(node.id==2)for(auto& field:std::get<domain::TransformProcessingParameters>(node.parameters).values)if(field.key=="size")field.value=signal_contracts::NumericLiteral{std::uint64_t{512}};
        domain::NumericProcessingGraph evaluated,executable;CHECK(domain::resolve_numeric_parameter_bindings(graph,numeric_editor::limits(),evaluated,executable,2));graph=std::move(evaluated);
        auto removed=*std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==8;});removed.id=18;graph.nodes.push_back(removed);graph.edges.push_back({7,18,0,0});
        desktop::NumericProjectDocument document;CHECK(desktop::create_numeric_startup_project(graph,document));CHECK(desktop::save_numeric_project(document,path));
        const auto run=[&](const QString& phase,const QString& source){QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--workspace-phase",phase,source});if(!child.waitForFinished(40000)){child.kill();child.waitForFinished();std::fprintf(stderr,"workspace phase %s timeout: %s\n",qPrintable(phase),child.readAllStandardError().constData());return false;}if(child.exitCode()!=0)std::fprintf(stderr,"workspace phase %s: %s\n",qPrintable(phase),child.readAllStandardError().constData());return child.exitStatus()==QProcess::NormalExit && child.exitCode()==0;};
        CHECK(run("save",path));
        auto saved=QJsonDocument::fromJson(read(path)).object();auto ui=saved["ui"].toObject(),workspace=ui["workspace"].toObject();workspace.remove("graph_navigation_version");ui["workspace"]=workspace;saved["ui"]=ui;
        QFile output(path);CHECK(output.open(QIODevice::WriteOnly));output.write(QJsonDocument(saved).toJson());output.close();
        CHECK(run("load",path));
        const QByteArray rejected=R"({"schema_version":4,"scene_graph":{"version":5,"processing_version":4,"nodes":"invalid","connections":[]}})";
        QFile broken(next);CHECK(broken.open(QIODevice::WriteOnly));broken.write(rejected);broken.close();
        CHECK(run("invalid-save",next));CHECK(read(next)==rejected && QFileInfo::exists(next+".workspace.ini"));
        if(!run("invalid-load",next)){
            const auto evidence=QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../.agent-workspace/workspace-invalid-repro.json");QFile::copy(next,evidence);QFile::copy(next+".workspace.ini",evidence+".workspace.ini");std::fprintf(stderr,"workspace recovery repro: %s\n",qPrintable(evidence));return 1;
        }CHECK(read(next)==rejected);
        ConfigManager manager(next);FileSaveApplicationConfig recovered;CHECK(manager.loadWorkspaceRecovery(recovered));
        {QSettings settings(next+".workspace.ini",QSettings::IniFormat);settings.setValue("extension","preserved");settings.sync();}
        CHECK(manager.saveWorkspaceRecovery(recovered));
        {QSettings settings(next+".workspace.ini",QSettings::IniFormat);CHECK(settings.value("extension").toString()=="preserved");settings.setValue("version",2);settings.sync();}
        const auto future=read(next+".workspace.ini");CHECK(!manager.loadWorkspaceRecovery(recovered) && !manager.saveWorkspaceRecovery(recovered) && read(next+".workspace.ini")==future);
        {QSettings settings(next+".workspace.ini",QSettings::IniFormat);settings.setValue("version",1);settings.sync();}
        QFile changed(next);CHECK(changed.open(QIODevice::Append));changed.write(" ");changed.close();CHECK(!manager.loadWorkspaceRecovery(recovered));
        std::puts("workspace closed/deleted/restart/tabbed placement and protected project recovery PASS");return 0;
    }
    if(argc==4 && QByteArray(argv[1])=="--workspace-phase"){
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");const QByteArray phase=argv[2];const QString source=QString::fromLocal8Bit(argv[3]);
        auto encoded=source.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());install_synthetic_capture_discovery(app);std::fprintf(stderr,"WORKSPACE %s constructed\n",phase.constData());
        MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;CHECK(window);window->show();
        auto* editor=window->findChild<SchemeEditorWidget*>();CHECK(editor);auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        const auto dockFor=[&](int id,bool instance=false)->WorkspaceDock*{for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").isValid() && dock->property("graphNode").toInt()==id && (dock->property("viewInstance").toInt()>0)==instance)return dock;return nullptr;};
        const bool invalid=phase.startsWith("invalid");
        if(phase=="invalid-load"){
            CHECK(!app.presenter().file_protection_warning().isEmpty());CHECK(!app._ctx.file_configs.ui.show_amplitudes && !app._ctx.file_configs.ui.show_spectrogram);
            const auto restoredDocks=window->workspaceDocks();CHECK(std::none_of(restoredDocks.begin(),restoredDocks.end(),[](auto* dock){return dock->property("viewInstance").toInt()>0;}));
            WorkspaceDock *fftDock=nullptr,*spectrogramDock=nullptr;for(auto* dock:restoredDocks){if(dock->objectName()=="fftDock")fftDock=dock;if(dock->objectName()=="spectrogramDock")spectrogramDock=dock;}
            CHECK(fftDock && spectrogramDock && fftDock->isClosed() && spectrogramDock->isClosed());
            for(auto* widget:QApplication::topLevelWidgets())CHECK(!widget->inherits("ads::CFloatingDockContainer") || !widget->isVisible());window->close();return 0;
        }
        if(invalid){CHECK(!app.presenter().file_protection_warning().isEmpty());auto* example=window->findChild<QAction*>("example-startup");CHECK(example);example->trigger();auto* buffer=model->delegateModel<AudioNodeModel>(2);CHECK(buffer && buffer->parameter_model()->editLiteral("size","512",0));}
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending() && dockFor(8);},8000));
        std::fprintf(stderr,"WORKSPACE %s ready\n",phase.constData());
        auto* flow=window->findChild<WorkspaceDock*>("dataFlowDock");CHECK(flow);
        if(phase=="load"){
            CHECK(dockFor(10) && dockFor(10)->isClosed());CHECK(!model->allNodeIds().contains(9));CHECK(!dockFor(999));
            CHECK(dockFor(11,true) && !dockFor(11,true)->isClosed() && !dockFor(11,true)->isFloating() && dockFor(11,true)->sharesHost(flow));
            CHECK(dockFor(12,true) && dockFor(12,true)->isClosed());CHECK(!model->allNodeIds().contains(18) && !dockFor(18) && !dockFor(18,true));
            for(auto* widget:QApplication::topLevelWidgets())CHECK(!widget->inherits("ads::CFloatingDockContainer") || !widget->isVisible());
            CHECK(dockFor(11,true)->dockAreaWidget()==dockFor(8)->dockAreaWidget() && flow->dockAreaWidget()!=dockFor(8)->dockAreaWidget());window->close();return 0;
        }
        for(auto* dock:window->workspaceDocks())CHECK(dock->isClosed() || !dock->isFloating());
        CHECK(dockFor(11) && dockFor(11)->isClosed());CHECK(dockFor(12) && dockFor(12)->isClosed());
        emit editor->graph_open_requested(11,1);CHECK(wait_until([&]{return dockFor(11,true) && !dockFor(11,true)->isClosed();}));
        CHECK(!dockFor(11,true)->isFloating() && dockFor(11,true)->sharesHost(flow) && dockFor(11,true)->dockAreaWidget()==dockFor(8)->dockAreaWidget());
        emit editor->graph_open_requested(12,0);CHECK(wait_until([&]{return dockFor(12,true) && dockFor(12,true)->isFloating();}));
        std::fprintf(stderr,"WORKSPACE %s opened\n",phase.constData());
        const auto closedKey=dockFor(12,true)->property("graphViewKey").toString();dockFor(12,true)->close();dockFor(10)->close();
        if(invalid){dockFor(8)->close();window->close();CHECK(wait_until([&]{return QFileInfo::exists(source+".workspace.ini");}));return 0;}
        emit editor->graph_open_requested(18,0);CHECK(wait_until([&]{return dockFor(18,true) && dockFor(18,true)->isFloating();}));
        std::fprintf(stderr,"WORKSPACE %s before delete\n",phase.constData());CHECK(model->deleteNode(18));CHECK(model->deleteNode(9));std::fprintf(stderr,"WORKSPACE %s deleted\n",phase.constData());
        auto preferences=app._ctx.file_configs.ui.graph_views.value(closedKey).toObject();preferences["node"]=999;preferences["open"]=true;
        auto identity=QJsonDocument::fromJson(QByteArray::fromHex(closedKey.toLatin1())).array();identity[2]=999;identity[3]=999;
        const auto ghost=QString::fromLatin1(QJsonDocument(identity).toJson(QJsonDocument::Compact).toHex());preferences["instance"]=999;app._ctx.file_configs.ui.graph_views[ghost]=preferences;
        CHECK(wait_until([&]{const auto current=QJsonDocument::fromJson(read(source)).object();return current["ui"].toObject()["graph_views"].toObject()[closedKey].toObject()["open"].isBool() && !current["ui"].toObject()["graph_views"].toObject()[closedKey].toObject()["open"].toBool();},6000));
        std::fprintf(stderr,"WORKSPACE %s before close\n",phase.constData());window->close();std::fprintf(stderr,"WORKSPACE %s closed\n",phase.constData());return 0;
    }
    if(argc==2 && QByteArray(argv[1])=="--matrix-center-ring-bypass"){
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        auto graph=numeric_generator_graph();
        const auto retained=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==7;});
        CHECK(retained!=graph.nodes.end());const auto history=retained->outputs[0].contract;
        const auto history_data=history.view();
        for(int id:{23,22}){
            domain::ConfiguredProcessingNode node;node.id=id;node.kind=id==23?"MatrixCenter":"MatrixWindowFunction";
            node.enabled=false;node.parameters=domain::TransformProcessingParameters{};
            domain::OwnedProcessingContract output;
            const auto& input=std::get<signal_contracts::NumericContract>(history_data);
            CHECK((id==23?domain::matrix_center_output_contract(input,output):domain::matrix_window_output_contract(input,output))==signal_contracts::Status::ok);
            node.inputs={{0,domain::PortDirection::Input,history}};node.outputs={{0,domain::PortDirection::Output,output}};
            graph.nodes.push_back(std::move(node));graph.edges.push_back({7,id,0,0});
        }
        desktop::NumericDraftPresentation display;
        display.application_settings={{"ui",QJsonObject{{"autosave_enabled",false}}}};
        desktop::NumericProjectDocument project;CHECK(desktop::create_numeric_project(graph,display,project));
        CHECK(desktop::save_numeric_project(project,path));const auto original=read(path);
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]{executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());install_synthetic_capture_discovery(app);
        const auto tap=app.pipeline().subscribe_numeric_tap({{"main",23,domain::PortDirection::Output,0},{"main",22,domain::PortDirection::Output,0}},[](const pipeline::DeliveredNumericFrame&){},{});
        CHECK(tap);
        struct Guard{pipeline::NumericEngine& engine;std::uint64_t tap;~Guard(){engine.unsubscribe_numeric_tap(tap);}} guard{app.pipeline(),tap};
        QObject::connect(&app.presenter(),&GuiPresenter::graph_apply_finished,&app,[](quint64,bool ok,const QString& error){if(!ok)std::fprintf(stderr,"startup rejected: %s\n",qPrintable(error));});
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();},6000));
        pipeline::DeliveredNumericFrame history_frame;std::uint64_t history_ordinal=0;
        CHECK(wait_until([&]{return app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history_frame,&history_ordinal)==signal_contracts::Status::ok&&history_ordinal>=10;},6000));
        for(int id:{23,22}){
            pipeline::DeliveredNumericFrame frame;
            CHECK(wait_until([&]{return app.pipeline().query_numeric({"main",id,domain::PortDirection::Output,0},frame)==signal_contracts::Status::ok&&frame.frame&&frame.frame.view().stamp.available;},6000));
            const auto matrix=std::get<signal_contracts::NumericView>(frame.frame.view().payload);
            CHECK(!matrix.ring.capacity&&matrix.actual[0]==400&&matrix.actual[1]==50);
            double energy=0;
            for(std::size_t row=0;row<matrix.actual[0];++row)for(std::size_t col=0;col<matrix.actual[1];++col){
                float value=0;CHECK(signal_contracts::load_element(matrix,{0,row,col},value)==signal_contracts::Status::ok&&std::isfinite(value)&&value>=0);
                energy+=double(value)*value;
            }
            CHECK(energy>0);
        }
        CHECK(read(path)==original);
        std::puts("Direct History ring -> bypassed MatrixCenter/MatrixWindow: ordinary matrices delivered and retained, synthetic/offscreen PASS");return 0;
    }
    if(argc==3 && QByteArray(argv[1])=="--export-temporal-project") {
        const QString destination=QString::fromLocal8Bit(argv[2]);CHECK(!QFile::exists(destination));
        desktop::NumericDraftPresentation display;
        display.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"show_spectrogram",true},
            {"show_amplitudes",true},{"show_data_flow",true}}}};
        desktop::NumericProjectDocument project;
        CHECK(desktop::create_numeric_project(numeric_temporal_fft_graph(),display,project));
        CHECK(desktop::save_numeric_project(project,path));
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());install_synthetic_capture_discovery(app);
        app._ctx.file_configs.ui.autosave_enabled=false;
        MainWindow* window=nullptr;for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        CHECK(wait_until([&]{return separated_layout(window,model)&&app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();},6000));
        app.save_configuration_as(destination);
        desktop::NumericProjectDocument exported;domain::NumericProcessingGraph loaded;desktop::NumericDraftPresentation saved_display;
        CHECK(desktop::load_numeric_project(destination,exported,loaded)&&desktop::read_numeric_presentation(exported,saved_display));
        CHECK(saved_display.positions.size()==int(loaded.nodes.size()));
        std::puts("Matrix transpose/window/row FFT example exported with measured non-overlapping layout; existing files untouched");return 0;
    }
    if(argc>1 && (QByteArray(argv[1])=="--temporal-fft"||QByteArray(argv[1])=="--temporal-fft-44100")) {
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        for(unsigned rate:{44100u,48000u}) {
            auto negotiated=numeric_temporal_fft_graph(rate);
            QString error;CHECK(desktop::resolve_numeric_source_format(negotiated,13,rate,1,numeric_editor::limits(),&error));
            CHECK(negotiated.execution_interval==(signal_contracts::Ratio{1,120}));
            domain::NumericProcessingGraph resolved;CHECK(domain::resolve_configuration_bindings(negotiated,numeric_editor::limits(),resolved));
            unsigned generators=0,captures=0,clocks=0;
            for(const auto& node:negotiated.nodes) {
                CHECK(node.kind!="PcmResampler"&&node.kind!="Mixer"&&node.id!=14&&node.id!=332&&node.id!=364);
                domain::ResolvedProcessingNode admitted;CHECK(domain::resolve_processing_node(node,numeric_editor::limits(),admitted));
                if(node.kind=="SignalGenerator"){++generators;CHECK(domain::data_outputs(node).size()==1&&node.clock_node==16);}
                if(node.kind=="AudioSource"){++captures;CHECK(node.id==13&&domain::data_outputs(node).size()==1);
                    domain::ProcessingParameterView source_rate;CHECK(domain::resolve_processing_parameter(admitted,"sample_rate",source_rate)==signal_contracts::Status::ok);
                    CHECK(std::visit([&](auto value){return value==rate;},std::get<signal_contracts::NumericLiteral>(source_rate)));}
                if(node.kind=="SampleClock"){++clocks;CHECK(std::get<domain::SampleClockProcessingParameters>(node.parameters).sample_rate==rate);}
                if(node.kind=="InputSelector")CHECK(node.id==200&&node.inputs.size()==2&&domain::data_outputs(node).size()==1);
                for(const auto* ports:{&node.inputs,&node.outputs})for(const auto& port:*ports) {
                    const auto contract=port.contract.view();const auto* value=std::get_if<signal_contracts::NumericContract>(&contract);CHECK(value);
                    if(value->shape.rank!=signal_contracts::Rank::Scalar)CHECK(value->clock.sample_rate==rate&&value->clock.tick_seconds==(signal_contracts::Ratio{1,rate}));
                    CHECK(value->channel_order.size()==1);
                    if(node.id==4&&port.direction==domain::PortDirection::Output)
                        CHECK(value->coordinates[0].step.numerator*4096==std::int64_t(rate)*std::int64_t(value->coordinates[0].step.denominator));
                }
            }
            CHECK(generators==1&&captures==1&&clocks==1);
        }
        {
            auto mismatched=numeric_temporal_fft_graph();
            auto& generator=mismatched.nodes.front();auto data=generator.outputs.front().contract.view();
            std::get<signal_contracts::NumericContract>(data).dtype=signal_contracts::DType::Float64;
            domain::OwnedProcessingContract changed;
            CHECK(domain::own_processing_contract(data,numeric_editor::limits().frame,numeric_editor::limits().max_owned_metadata_bytes,changed)==signal_contracts::Status::ok);
            generator.outputs.front().contract=std::move(changed);
            QByteArray before,after;CHECK(desktop::encode_numeric_graph(mismatched,before));QString error;
            CHECK(!desktop::resolve_numeric_source_format(mismatched,13,44100,1,numeric_editor::limits(),&error));
            CHECK(desktop::encode_numeric_graph(mismatched,after)&&before==after);
        }
        const unsigned temporal_rate=QByteArray(argv[1])=="--temporal-fft-44100"?44100:48000;
        auto runtime_graph=numeric_temporal_fft_graph(temporal_rate);QString native_error;
        CHECK(desktop::resolve_numeric_source_format(runtime_graph,13,temporal_rate,1,numeric_editor::limits(),&native_error));
        const auto expected_bin_hz=double(runtime_graph.execution_interval.denominator)/double(runtime_graph.execution_interval.numerator)/512.;
        desktop::NumericDraftPresentation display;
        display.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"show_spectrogram",true},
            {"show_amplitudes",true},{"show_data_flow",true}}}};
        desktop::NumericProjectDocument project;
        CHECK(desktop::create_numeric_project(runtime_graph,display,project));
        CHECK(desktop::save_numeric_project(project,path));const auto original=read(path);
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::startup_capture_factory());install_synthetic_capture_discovery(app,temporal_rate);
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        // The test's injected device catalog replaces host discovery before
        // startup Apply. Import through that same catalog so the displayed and
        // applied contracts refer to the deterministic synthetic device.
        const bool imported=model->set_numeric_graph(runtime_graph,&native_error);
        if(!imported)std::fprintf(stderr,"Temporal native import %u Hz: %s\n",temporal_rate,native_error.toUtf8().constData());
        CHECK(imported);
        std::mutex selected_mutex;pipeline::DeliveredNumericFrame selected_latest,pcm_latest,center_latest;
        const auto tap=app.pipeline().subscribe_numeric_tap({{"main",17,domain::PortDirection::Output,0},
            {"main",19,domain::PortDirection::Output,0},{"main",23,domain::PortDirection::Output,0},{"main",200,domain::PortDirection::Output,0}},[&](const pipeline::DeliveredNumericFrame& delivered){
            if(!delivered.context||delivered.context->endpoint.direction!=domain::PortDirection::Output)return;
            std::lock_guard lock(selected_mutex);
            if(delivered.context->endpoint.node==19)selected_latest=delivered;
            if(delivered.context->endpoint.node==200)pcm_latest=delivered;
            if(delivered.context->endpoint.node==23)center_latest=delivered;
        },{});CHECK(tap);
        struct TapGuard {pipeline::NumericEngine& engine;std::uint64_t token;~TapGuard(){engine.unsubscribe_numeric_tap(token);}} guard{app.pipeline(),tap};
        const bool started=wait_until([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();},6000);
        if(!started) {
            QString error;domain::NumericProcessingGraph exported;model->export_numeric_graph(exported,&error);
            std::fprintf(stderr,"Temporal startup: apply=%s saved=%s editor=%s\n",app.presenter().apply_status().toUtf8().constData(),app.presenter().saved_graph_warning().toUtf8().constData(),error.toUtf8().constData());
            const auto snapshot=app.presenter().client().snapshot();
            for(auto it=snapshot.statuses.begin();it!=snapshot.statuses.end();++it)std::fprintf(stderr,"component %s state=%d error=%s\n",qPrintable(it.key()),int(it.value().state),qPrintable(it.value().error));
        }
        CHECK(started);
        auto query=[&](int id,pipeline::DeliveredNumericFrame& frame) {
            if(id==19||id==200||id==23) {
                std::lock_guard lock(selected_mutex);frame=id==19?selected_latest:id==200?pcm_latest:center_latest;
                return frame.frame&&frame.status==signal_contracts::Status::ok&&frame.frame.view().stamp.available
                    &&frame.delivery.graph_revision==app.presenter().client().snapshot().graph_revision;
            }
            return app.pipeline().query_numeric({"main",id,domain::PortDirection::Output,0},frame)==signal_contracts::Status::ok
                &&frame.frame&&frame.frame.view().stamp.available
                &&frame.delivery.graph_revision==app.presenter().client().snapshot().graph_revision;
        };
        pipeline::DeliveredNumericFrame matrix_frame,selected_frame;
        const bool outputs_ready=wait_until([&]{return query(17,matrix_frame)&&query(19,selected_frame);},6000);
        if(!outputs_ready) {
            const auto matrix_status=app.pipeline().query_numeric({"main",17,domain::PortDirection::Output,0},matrix_frame);
            query(19,selected_frame);
            std::fprintf(stderr,"Temporal delivery: matrix_status=%d matrix_available=%d row_status=%d row_available=%d\n",
                int(matrix_status),matrix_frame.frame&&matrix_frame.frame.view().stamp.available,
                int(selected_frame.status),selected_frame.frame&&selected_frame.frame.view().stamp.available);
            std::fprintf(stderr,"Temporal diagnostics: %s\n",QJsonDocument(app.pipeline().snapshot().diagnostics).toJson(QJsonDocument::Compact).constData());
        }
        CHECK(outputs_ready);
        pipeline::DeliveredNumericFrame centered_frame;
        CHECK(wait_until([&]{return query(23,centered_frame);},6000));
        const auto centered=std::get<signal_contracts::NumericView>(centered_frame.frame.view().payload);
        CHECK(centered.actual[0]==50&&centered.actual[1]==400);
        for(std::size_t band=0;band<centered.actual[0];++band){
            double sum=0,scale=1;
            for(std::size_t sample=0;sample<centered.actual[1];++sample){float value=0;
                CHECK(signal_contracts::load_element(centered,{0,band,sample},value)==signal_contracts::Status::ok&&std::isfinite(value));
                sum+=value;scale=std::max(scale,std::abs(double(value)));
            }
            CHECK(std::abs(sum/centered.actual[1])<scale*1e-6);
        }
        const auto matrix=std::get<signal_contracts::NumericView>(matrix_frame.frame.view().payload);
        CHECK(matrix.actual[0]==50&&matrix.actual[1]==256);
        CHECK(matrix.contract->shape.axes[0]==signal_contracts::Axis::Band&&matrix.contract->shape.axes[1]==signal_contracts::Axis::Bin);
        CHECK(matrix.contract->coordinates[1].unit==signal_contracts::NumericUnit::Hertz);
        CHECK(matrix.coordinates.axes[1].size()==256*sizeof(double));
        double step=0;std::memcpy(&step,matrix.coordinates.axes[1].data()+sizeof(double),sizeof(double));
        CHECK(std::abs(step-expected_bin_hz)<1e-6);
        double energy=0;
        for(std::size_t band=0;band<50;++band)for(std::size_t bin=0;bin<256;++bin) {
            float value=0;CHECK(signal_contracts::load_element(matrix,{0,band,bin},value)==signal_contracts::Status::ok);
            CHECK(std::isfinite(value));energy+=double(value)*value;
        }
        CHECK(energy>0);
        auto* row=model->delegateModel<AudioNodeModel>(19);auto* window_function=model->delegateModel<AudioNodeModel>(22);
        auto* center=model->delegateModel<AudioNodeModel>(23);
        CHECK(center&&center->numeric_node()->kind=="MatrixCenter"&&center->numeric_node()->enabled);
        CHECK(row&&window_function&&window_function->parameter_model()->bypassed());
        std::uint64_t ordinal=0,filled=0;pipeline::DeliveredNumericFrame history;
        CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok);
        const auto before_ordinal=ordinal;
        for(std::uint64_t band:{49u,0u,24u}) {
            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(row->parameter_model()->edit("row",int(band)));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(19,selected_frame);},6000));
            CHECK(std::get<signal_contracts::NumericView>(selected_frame.frame.view().payload).actual[0]==256);
        }
        CHECK(!row->parameter_model()->edit("row",50));
        CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=before_ordinal);
        for(bool bypass:{false,true}) {
            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(window_function->parameter_model()->setBypassed(bypass));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(17,matrix_frame);},6000));
        }
        CHECK(window_function->parameter_model()->setBypassed(false));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&query(17,matrix_frame);},6000));
        qint64 slowest_window_switch=0,slowest_window_edit=0;
        for(int function:{1,2,3,0}) {
            const auto revision=app.presenter().client().snapshot().graph_revision;
            QElapsedTimer elapsed;elapsed.start();
            CHECK(window_function->parameter_model()->edit("function",function));
            slowest_window_edit=std::max(slowest_window_edit,elapsed.elapsed());
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(17,matrix_frame);},6000));
            slowest_window_switch=std::max(slowest_window_switch,elapsed.elapsed());
            CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=before_ordinal);
        }
        std::printf("Offscreen window functions: all four revisions available; slowest synchronous edit %lld ms, Apply+delivery %lld ms\n",static_cast<long long>(slowest_window_edit),static_cast<long long>(slowest_window_switch));
        const auto before_burst=app.presenter().client().snapshot().graph_revision;
        CHECK(app.presenter().apply_pipeline_graph({})>0&&app.presenter().graph_request_pending());
        for(int function:{1,2,3})CHECK(window_function->parameter_model()->edit("function",function));
        QElapsedTimer burst;burst.start();
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>=before_burst+2&&query(17,matrix_frame);},6000));
        CHECK(std::get<int>(window_function->parameter_model()->field("function")->read())==3);
        std::printf("Discrete window burst during pending Apply: latest selection delivered in %lld ms\n",static_cast<long long>(burst.elapsed()));
        CHECK(window_function->parameter_model()->setBypassed(true));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&query(17,matrix_frame);},6000));
        for(int method:{1,2,0}){
            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(center->parameter_model()->edit("method",method));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(23,centered_frame)&&query(17,matrix_frame);},6000));
            if(method==2){
                const auto offset_revision=app.presenter().client().snapshot().graph_revision;
                CHECK(center->parameter_model()->edit("offset",signal_contracts::NumericLiteral{1.25}));
                CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>offset_revision&&query(23,centered_frame);},6000));
            }
        }
        for(bool bypass:{true,false}){
            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(center->parameter_model()->setBypassed(bypass));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(23,centered_frame)&&query(17,matrix_frame);},6000));
        }
        auto* generator=model->delegateModel<AudioNodeModel>(1);CHECK(generator);
        const auto pause_revision=app.presenter().client().snapshot().graph_revision;
        CHECK(generator->parameter_model()->setBypassed(true));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>pause_revision&&query(17,matrix_frame);},6000));
        CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok);
        const auto pause_ordinal=ordinal;bool pause_unavailable=false;
        CHECK(wait_until([&]{
            pause_unavailable|=!query(17,matrix_frame)||!query(19,selected_frame);
            return app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=pause_ordinal+420;
        },7000));
        CHECK(!pause_unavailable&&query(17,matrix_frame));
        const auto silence=std::get<signal_contracts::NumericView>(matrix_frame.frame.view().payload);
        for(std::size_t band=0;band<50;++band)for(std::size_t bin=0;bin<256;++bin){float value=1;CHECK(signal_contracts::load_element(silence,{0,band,bin},value)==signal_contracts::Status::ok&&value==0);}
        CHECK(generator->parameter_model()->setBypassed(false));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&query(17,matrix_frame);},6000));
        auto* selector=model->delegateModel<AudioNodeModel>(200);CHECK(selector);
        CHECK(selector->numeric_node()->inputs.size()==2&&domain::data_outputs(*selector->numeric_node()).size()==1);
        for(const auto& node:model->numeric_graph().nodes)CHECK(node.kind!="PcmResampler"&&node.kind!="Mixer"&&node.id!=14&&node.id!=332&&node.id!=364);
        double loopback_energy=0;
        for(int stream:{1,0}) {
            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok);
            const auto previous_ordinal=ordinal;
            CHECK(selector->select_input(domain::PortId(stream)));
            pipeline::DeliveredNumericFrame pcm_frame;
            const bool switched_ready=wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision
                &&query(17,matrix_frame)&&query(19,selected_frame)&&query(200,pcm_frame);},6000);
            if(!switched_ready)std::fprintf(stderr,"Temporal switch %d: %s\n",stream,QJsonDocument(app.pipeline().snapshot().diagnostics).toJson(QJsonDocument::Compact).constData());
            CHECK(switched_ready);
            const auto pcm=std::get<signal_contracts::NumericView>(pcm_frame.frame.view().payload);
            CHECK(pcm.contract->channel_order.size()==1&&pcm.contract->clock.sample_rate==temporal_rate);
            double pcm_energy=0;
            for(std::size_t i=0;i<pcm.actual[0];++i) {
                float value=0;CHECK(signal_contracts::load_element(pcm,{0,i,0},value)==signal_contracts::Status::ok&&std::isfinite(value));
                pcm_energy+=double(value)*value;
            }
            CHECK(pcm.actual[0]>0);pcm_energy/=pcm.actual[0];CHECK(pcm_energy>0);
            if(stream==1)loopback_energy=pcm_energy;
            else CHECK(pcm_energy>loopback_energy*10);
            const auto switched=std::get<signal_contracts::NumericView>(matrix_frame.frame.view().payload);
            CHECK(switched.actual[0]==50&&switched.actual[1]==256);
            double switched_bin_hz=0;std::memcpy(&switched_bin_hz,switched.coordinates.axes[1].data()+sizeof(double),sizeof(double));
            CHECK(std::abs(switched_bin_hz-expected_bin_hz)<1e-6);
            for(std::size_t band=0;band<50;++band)for(std::size_t bin=0;bin<256;++bin) {
                float value=0;CHECK(signal_contracts::load_element(switched,{0,band,bin},value)==signal_contracts::Status::ok&&std::isfinite(value));
            }
            CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=previous_ordinal);
        }
        const auto save_revision=app.presenter().client().snapshot().graph_revision;
        CHECK(selector->select_input(1));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>save_revision;},6000));
        auto selector_presentation=selector->presentation();
        const QJsonArray custom_names{"My generator","Desktop audio"};
        selector_presentation["stream_names"]=custom_names;selector->set_presentation(selector_presentation);
        CHECK(read(path)==original);
        app.save_configuration_as(next);CHECK(QFile::exists(next));
        desktop::NumericProjectDocument saved;domain::NumericProcessingGraph saved_graph;
        CHECK(desktop::load_numeric_project(next,saved,saved_graph));
        CHECK(saved_graph.execution_interval==runtime_graph.execution_interval);
        for(const auto& node:saved_graph.nodes)CHECK(node.kind!="PcmResampler"&&node.id!=14&&node.id!=332&&node.id!=364);
        CHECK(std::any_of(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.kind=="MatrixFFT";}));
        CHECK(std::any_of(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.kind=="MatrixTranspose";}));
        CHECK(std::any_of(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.kind=="MatrixWindowFunction";}));
        CHECK(std::any_of(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.kind=="MatrixCenter"&&node.id==23;}));
        const auto saved_selector=std::find_if(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.id==200;});
        CHECK(saved_selector!=saved_graph.nodes.end());
        const auto& selector_values=std::get<domain::TransformProcessingParameters>(saved_selector->parameters).values;
        const auto saved_choice=std::find_if(selector_values.begin(),selector_values.end(),[](const auto& field){return field.key=="selected_input";});
        CHECK(saved_choice!=selector_values.end()&&std::visit([](auto value){return value==1;},std::get<signal_contracts::NumericLiteral>(saved_choice->value)));
        desktop::NumericDraftPresentation saved_display;CHECK(desktop::read_numeric_presentation(saved,saved_display));
        CHECK(saved_display.node_presentations["200"].toObject()["stream_names"].toArray()==custom_names);
        bool center_stats_seen=false;
        for(const auto item:app.pipeline().snapshot().diagnostics["node_stats"].toArray()){
            const auto stats=item.toObject();if(stats["node"].toInt()!=23)continue;
            center_stats_seen=true;
            CHECK(stats["calls"].toVariant().toULongLong()>0&&stats["errors"].toVariant().toULongLong()==0);
            std::printf("MatrixCenter diagnostics: calls=%s errors=%s max_self_ns=%s\n",qPrintable(stats["calls"].toVariant().toString()),qPrintable(stats["errors"].toVariant().toString()),qPrintable(stats["max_self_nanoseconds"].toVariant().toString()));
        }
        CHECK(center_stats_seen);
        std::printf("Temporal FFT application: mono generator/loopback switching without resampling, 8 silent changing tones, 50x256 matrix, cadence %.9f Hz/bin, selected first/last/middle bands, window on/off, History continuity, canonical SaveAs PASS\n",step);
        return 0;
    }
    if(argc==3 && QByteArray(argv[1])=="--export-current-project") {
        const QString destination=QString::fromLocal8Bit(argv[2]);CHECK(!QFile::exists(destination));
        desktop::NumericProjectDocument project;CHECK(desktop::create_numeric_startup_project(numeric_startup_graph(),project));
        QJsonArray presets;
        for(const auto& example:numeric_example_graphs()) {
            QByteArray bytes;CHECK(desktop::encode_numeric_graph(example.graph,bytes));
            presets.append(QJsonObject{{"version",1},{"id",QStringLiteral("builtin-")+example.id},
                {"name",example.title_key},{"graph",QJsonDocument::fromJson(bytes).object()}});
        }
        desktop::NumericDraftPresentation presentation;presentation.application_settings={{"ui",QJsonObject{{"pipeline_presets",presets}}}};
        desktop::NumericProjectDocument populated;CHECK(desktop::stage_numeric_presentation(project,presentation,populated));
        CHECK(desktop::save_numeric_project(populated,destination));
        std::puts("Current product project and canonical presets exported; existing files untouched");return 0;
    }
    if(argc>1 && QByteArray(argv[1])=="--current-examples") {
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::startup_capture_factory(false,2));install_synthetic_capture_discovery(app,48000,2);
        app._ctx.file_configs.ui.autosave_enabled=false;
        const bool started=wait_until([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();});
        if(!started){const auto snapshot=app.presenter().client().snapshot();std::fprintf(stderr,"Startup Apply: %s\n",qPrintable(app.presenter().apply_status()));for(auto it=snapshot.statuses.begin();it!=snapshot.statuses.end();++it)std::fprintf(stderr,"component %s state=%d error=%s\n",qPrintable(it.key()),int(it.value().state),qPrintable(it.value().error));}
        CHECK(started);
        MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(widget))window=found;
        CHECK(window);auto* editor=window->findChild<SchemeEditorWidget*>();CHECK(editor);
        for(const auto& example:numeric_example_graphs()) {
              if(QByteArray(example.id)=="two-streams") {
                  unsigned generators=0,clocks=0;
                  for(const auto& node:example.graph.nodes) {
                      if(node.kind=="SampleClock")++clocks;
                      CHECK(node.kind!="Mixer");
                      if(node.kind=="SignalGenerator") {
                          ++generators;CHECK(node.clock_node==16&&domain::data_outputs(node).size()==1);
                          CHECK(std::get<domain::SignalGeneratorProcessingParameters>(node.parameters).oscillators.size()==8);
                      }
                  }
                  CHECK(generators==2&&clocks==1);
              }
            auto* action=window->findChild<QAction*>(QStringLiteral("example-")+example.id);CHECK(action);
            const auto revision=app.presenter().client().snapshot().graph_revision;action->trigger();
            std::fprintf(stderr,"apply example=%s status=%s warning=%s\n",qPrintable(action->objectName()),qPrintable(app.presenter().apply_status()),qPrintable(app.presenter().saved_graph_warning()));
            const bool exampleApplied=wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision;});
             if(!exampleApplied){const auto current=app.presenter().client().snapshot();auto* validation=window->findChild<QLabel*>("pipelineValidation");
                 const auto status=app.presenter().apply_status().toUtf8(),message=window->statusBar()->currentMessage().toUtf8();
                 const auto detail=validation?validation->toolTip().toUtf8():QByteArray{};
                  std::fprintf(stderr,"example=%s apply timeout before=%llu after=%llu pending=%d enabled=%d status=%s statusBar=%s validation=%s\n",example.id,(unsigned long long)revision,(unsigned long long)current.graph_revision,int(app.presenter().graph_request_pending()),int(action->isEnabled()),status.constData(),message.constData(),detail.constData());
                  domain::NumericProcessingGraph draft,evaluated,executable;QString draftError;
                  if(editor->numeric_draft(draft,&draftError)){
                      const auto result=domain::resolve_numeric_parameter_bindings(draft,numeric_editor::limits(),evaluated,executable);
                      std::fprintf(stderr,"example=%s draft resolver error=%u node=%d port=%u\n",example.id,unsigned(result.error),int(result.node),unsigned(result.port));
                  }else std::fprintf(stderr,"example=%s draft unavailable: %s\n",example.id,draftError.toUtf8().constData());}
            CHECK(exampleApplied);
            if(QByteArray(example.id)=="startup") {
                window->show();
                auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
                CHECK(wait_until([&]{return separated_layout(window,model);}));
                auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();CHECK(scene);
                const auto generator=scene->nodeGraphicsObject(1)->sceneBoundingRect();
                const auto capture=scene->nodeGraphicsObject(13)->sceneBoundingRect();
                CHECK(generator.left()<=capture.left()+1 && generator.top()<capture.top());
                CHECK(domain::has_configuration_binding(model->numeric_graph(),1,domain::ConfigurationRole::SampleClock));
                const auto& shown=model->numeric_graph();
                const auto source=std::find_if(shown.nodes.begin(),shown.nodes.end(),[](const auto& node){return node.id==1;});
                const auto clock=std::find_if(shown.nodes.begin(),shown.nodes.end(),[](const auto& node){return node.id==16;});
                CHECK(source!=shown.nodes.end() && source->clock_node==16);
                CHECK(clock!=shown.nodes.end() && clock->kind=="SampleClock");
                CHECK(std::any_of(shown.configuration_bindings.begin(),shown.configuration_bindings.end(),[](const auto& binding){return binding.from==16 && binding.to==1 && binding.output==domain::ConfigurationRole::SampleClock && binding.input==domain::ConfigurationRole::SampleClock;}));
                const auto imagePath=qEnvironmentVariable("NUMERIC_TEST_SCENE_IMAGE");
                if(!imagePath.isEmpty()){
                    const auto bounds=scene->itemsBoundingRect().adjusted(-20,-20,20,20);
                    QImage image(bounds.size().toSize().scaled({2400,1800},Qt::KeepAspectRatio),QImage::Format_ARGB32_Premultiplied);
                    image.fill(window->palette().color(QPalette::Window));QPainter painter(&image);
                    scene->render(&painter,QRectF(image.rect()),bounds);painter.end();CHECK(image.save(imagePath));
                }
            }
            if(QByteArray(example.id)=="temporal-fft") {
                auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
                auto* selector=model->delegateModel<AudioNodeModel>(200);CHECK(selector&&selector->selected_input()==0);
                const auto& shown=model->numeric_graph();
                CHECK(selector->numeric_node()->inputs.size()==4 && domain::data_outputs(*selector->numeric_node()).size()==2 && selector->numeric_node()->outputs.size()==2);
                const auto rate=std::get<signal_contracts::NumericContract>(selector->numeric_node()->outputs[1].contract.view());
                CHECK(rate.shape.rank==signal_contracts::Rank::Scalar && rate.unit==signal_contracts::NumericUnit::Hertz);
                const auto wired=[&](domain::NodeId from,domain::PortId output,domain::PortId input){return std::any_of(shown.edges.begin(),shown.edges.end(),[&](const auto& edge){return edge.from==from && edge.to==200 && edge.output_port==output && edge.input_port==input;});};
                CHECK(wired(1,0,0) && wired(1,domain::sample_rate_output,1) && wired(13,0,2) && wired(13,domain::sample_rate_output,3));
                CHECK(selector->presentation()["stream_names"].toArray().size()==2);
                CHECK(selector->stream_name(0)==ui::text("Generator"));
                CHECK(selector->stream_name(1)==ui::text("Loopback"));
            }
            for(const auto& node:example.graph.nodes)if(node.kind=="Spectrogram") {
                pipeline::DeliveredNumericFrame frame;std::uint64_t ordinal=0,filled=0;
                CHECK(wait_until([&]{return app.pipeline().query_numeric({"main",node.id,domain::PortDirection::Output,0},frame,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=3;}));
                CHECK(std::get<signal_contracts::NumericView>(frame.frame.view().payload).actual[0]==400&&filled>0);
            }
            if(QByteArray(example.id)=="stereo-sources") {
                auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
                struct Lane {std::uint64_t revision=0;unsigned frames=0;bool positive=false;std::uint64_t first_sequence=0,last_sequence=0;double energy=0;};
                std::mutex lane_mutex;std::array<Lane,2> lanes{};bool invalid_lane=false;
                const auto tap=app.pipeline().subscribe_numeric_tap({},[&](const pipeline::DeliveredNumericFrame& delivered){
                    if(!delivered.context||!delivered.frame||!delivered.frame.view().stamp.available)return;
                    const auto& endpoint=delivered.context->endpoint;if(endpoint.direction!=domain::PortDirection::Output||(endpoint.node!=104&&endpoint.node!=404))return;
                    std::lock_guard lock(lane_mutex);const unsigned lane=endpoint.node==104?0:1;auto& state=lanes[lane];
                    if(state.revision!=delivered.delivery.graph_revision)state={delivered.delivery.graph_revision,0,false};
                    const auto view=delivered.frame.view();const auto* values=std::get_if<signal_contracts::NumericView>(&view.payload);
                    if(!values||values->contract->dtype!=signal_contracts::DType::Float32||values->actual[0]!=256){invalid_lane=true;return;}
                    for(std::size_t i=0;i<values->actual[0];++i){float value=0;if(signal_contracts::load_element(*values,{0,i,0},value)!=signal_contracts::Status::ok||!std::isfinite(value)){invalid_lane=true;return;}state.positive|=std::abs(value)>1e-12f;state.energy+=std::abs(value);}
                    if(!state.frames)state.first_sequence=view.stamp.sequence;else if(view.stamp.sequence<state.last_sequence)invalid_lane=true;state.last_sequence=view.stamp.sequence;
                    ++state.frames;
                },{});CHECK(tap);
                struct TapGuard {pipeline::NumericEngine& engine;std::uint64_t token;~TapGuard(){engine.unsubscribe_numeric_tap(token);}} guard{app.pipeline(),tap};
                double loopbackEnergy=0,microphoneEnergy=0;
                for(unsigned stream:{1u,2u,0u}) {
                    const auto revision_before=app.presenter().client().snapshot().graph_revision;
                    auto* selector=model->delegateModel<AudioNodeModel>(200);CHECK(selector&&selector->select_input(domain::PortId(stream)));
                    CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision_before;}));
                    const auto revision=app.presenter().client().snapshot().graph_revision;
                    CHECK(wait_until([&]{std::lock_guard lock(lane_mutex);return invalid_lane||(lanes[0].revision==revision&&lanes[1].revision==revision&&lanes[0].frames>=5&&lanes[1].frames>=5&&lanes[0].last_sequence>lanes[0].first_sequence&&lanes[1].last_sequence>lanes[1].first_sequence); }));
                    {std::lock_guard lock(lane_mutex);
                        if(selector->selected_input()!=stream || invalid_lane || !lanes[0].positive || !lanes[1].positive)
                            std::fprintf(stderr,"stereo stream=%u selected=%u revision=%llu invalid=%d lane0=%u/%d/%.9g lane1=%u/%d/%.9g\n",stream,unsigned(selector->selected_input()),(unsigned long long)revision,int(invalid_lane),lanes[0].frames,int(lanes[0].positive),lanes[0].energy,lanes[1].frames,int(lanes[1].positive),lanes[1].energy);
                        CHECK(selector->selected_input()==stream && !invalid_lane && lanes[0].positive && lanes[1].positive);
                        const auto meanEnergy=lanes[0].energy/lanes[0].frames;
                        if(stream==1)loopbackEnergy=meanEnergy;
                        if(stream==2)microphoneEnergy=meanEnergy;
                    }
                }
                CHECK(loopbackEnergy>0 && microphoneEnergy>loopbackEnergy*10);

            }
              if(QByteArray(example.id)=="two-streams") {
                  std::vector<double> signatures[2];
                  for(unsigned channel=0;channel<2;++channel) {
                      pipeline::DeliveredNumericFrame frame;std::uint64_t ordinal=0,filled=0;
                      CHECK(wait_until([&]{return app.pipeline().query_numeric({"main",channel?107:7,domain::PortDirection::Output,0},frame,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=30;}));
                      const auto view=std::get<signal_contracts::NumericView>(frame.frame.view().payload);
                      signatures[channel].resize(view.actual[1]);double energy=0;
                      for(std::size_t row=0;row<view.actual[0];++row)
                          for(std::size_t band=0;band<view.actual[1];++band) {
                              float value=0;
                              CHECK(signal_contracts::load_element(view,{0,row,band},value)==signal_contracts::Status::ok);
                              CHECK(std::isfinite(value));signatures[channel][band]+=std::abs(value);energy+=std::abs(value);
                          }
                      CHECK(energy>0);
                      for(auto& value:signatures[channel])value/=energy;
                  }
                  CHECK(signatures[0].size()==signatures[1].size());double difference=0;
                  for(std::size_t band=0;band<signatures[0].size();++band)difference+=std::abs(signatures[0][band]-signatures[1][band]);
                  CHECK(difference>0.05);
              }
            std::printf("Current example %s: actual menu Apply and all History branches PASS\n",example.id);
        }
        app.save_configuration_as(next);CHECK(QFile::exists(next));return 0;
    }
    if(argc==3 && QByteArray(argv[1])=="--verify-project") {
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        const QString source=QString::fromLocal8Bit(argv[2]);const auto original=read(source);
        CHECK(!original.isEmpty() && QFile::copy(source,path));
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        install_synthetic_capture_discovery(app);
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
            CHECK(desktop::create_numeric_startup_project(numeric_generator_graph(),fixture));
            CHECK(desktop::stage_numeric_presentation(fixture,display,fixture));
        } else CHECK(desktop::create_numeric_project(numeric_generator_graph(),display,fixture));
        CHECK(desktop::save_numeric_project(fixture,path));const auto original_file=read(path);
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::startup_capture_factory());
        install_synthetic_capture_discovery(app);
        MainWindow* window=nullptr;for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(window&&model);window->show();
        const auto stored_presets=app._ctx.file_configs.ui.pipeline_presets;
        CHECK(stored_presets.size()==(old_builtin?1:0));CHECK(model->numeric_graph().nodes.size()==numeric_generator_graph().nodes.size());
        if(old_builtin)CHECK(stored_presets[0].toObject()["graph"].toObject()["nodes"].toArray().size()==int(numeric_generator_graph().nodes.size()));
        CHECK(!app._ctx.file_configs.ui.autosave_enabled&&!app._ctx.file_configs.ui.pipeline_auto_apply);
        CHECK(app.presenter().client().snapshot().graph_revision==0);
        auto* demo=window->findChild<QAction*>("example-startup");CHECK(demo&&demo->isEnabled()&&demo->isVisible());
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
            CHECK(demo->text()==(locale==0?QString::fromUtf8("Спектрограмма: генератор, loopback, микрофон"):QStringLiteral("Spectrogram: generator, loopback, microphone")));
            CHECK(menu->title()==(locale==0?QString::fromUtf8("Пресеты"):QStringLiteral("Presets")));
            CHECK(display_menu->title()==(locale==0?QString::fromUtf8("Отображение"):QStringLiteral("Display")));
        }
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();}));const auto startup_revision=app.presenter().client().snapshot().graph_revision;
        language->setCurrentIndex(original_language);demo->trigger();
        CHECK(model->numeric_graph().nodes.size()==numeric_startup_graph().nodes.size()&&model->numeric_graph().edges.size()==native_mono_example(numeric_startup_graph()).edges.size());
        CHECK(app._ctx.file_configs.ui.pipeline_presets==stored_presets&&read(path)==original_file);
        CHECK(app._ctx.file_configs.ui.show_raw_samples&&app._ctx.file_configs.ui.show_amplitudes&&app._ctx.file_configs.ui.show_spectrogram);
        const auto visibleGraph=model->numeric_graph();CHECK(std::none_of(visibleGraph.nodes.begin(),visibleGraph.nodes.end(),[](const auto& n){return n.kind=="MelView";}));
        CHECK(wait_until([&]{QSet<QPair<int,int>> positions;for(auto id:model->allNodeIds()){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}return positions.size()==numeric_startup_graph().nodes.size();}));
        export_startup_scenes(window->findChild<SchemeEditorWidget*>(),model);
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>startup_revision && !app.presenter().graph_request_pending();}));
        CHECK(verify_startup_selector(app,window,false));
        CHECK(app._ctx.file_configs.ui.pipeline_presets==stored_presets&&read(path)==original_file);
        std::printf("unconditional generatorLoopbackPreset existing config old_builtin=%d: full stereo draft/layout/views/selector/card/History; original file and stored presets unchanged PASS\n",int(old_builtin));return 0;
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
        install_synthetic_capture_discovery(app);
        MainWindow* window=nullptr;for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();
        auto* autosave=window->findChild<QCheckBox*>("enableAutosave");CHECK(autosave);autosave->setChecked(false);
        QObject::connect(&app.presenter(),&GuiPresenter::graph_apply_finished,&app,[](quint64,bool ok,const QString& error){if(!ok)std::fprintf(stderr,"startup rejected: %s\n",qPrintable(error));});
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();},6000));
        CHECK(verify_startup_selector(app,window,unavailable));CHECK(unavailable?read(path)==original:!QFile::exists(path));return 0;
    }
    if(argc>1 && (QByteArray(argv[1])=="--shipped-examples"||QByteArray(argv[1])=="--shipped-examples-ui")) {
        const bool gui_workflow=QByteArray(argv[1])=="--shipped-examples-ui";
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        install_synthetic_capture_discovery(app);
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
        for(const auto& shipped:{numeric_default_graph(),numeric_generator_graph(),numeric_startup_graph()}) {
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
            CHECK(persisted.configuration_bindings==shipped.configuration_bindings);
            const auto editor_bindings=candidate.numeric_graph().configuration_bindings;CHECK(std::is_permutation(editor_bindings.begin(),editor_bindings.end(),shipped.configuration_bindings.begin(),shipped.configuration_bindings.end()));
            QByteArray before;CHECK(desktop::encode_numeric_graph(candidate.numeric_graph(),before));
            // A pre-rank-restriction user draft could contain a Matrix MelView.
            // Reject it atomically and identify the stable offending endpoints.
            auto invalid=persisted;auto invalid_view=*std::find_if(invalid.nodes.begin(),invalid.nodes.end(),[](const auto& n){return n.id==8;});
            invalid_view.id=17;invalid_view.kind="MelView";invalid.nodes.push_back(std::move(invalid_view));
            invalid.edges.push_back({7,17,0,0});
            CHECK(!candidate.set_numeric_graph(invalid,&diagnostic));
            CHECK(diagnostic.contains("[7]")&&diagnostic.contains("[17]"));
            QByteArray after;CHECK(desktop::encode_numeric_graph(candidate.numeric_graph(),after));CHECK(after==before);
        }
        auto inspect=[&](const char* label){
            domain::NumericProcessingGraph graph;QString error;if(!editor->numeric_draft(graph,&error)){std::fprintf(stderr,"%s draft: %s\n",label,qPrintable(error));return false;}
            auto expected=numeric_startup_graph();
            if(QByteArray(label)=="stereo-sources")for(const auto& example:numeric_example_graphs())if(QByteArray(example.id)=="stereo-sources")expected=example.graph;
             const auto channels=QByteArray(label)=="stereo-sources"?2u:1u;
             if(!desktop::resolve_numeric_source_format(expected,13,48000,channels,numeric_editor::limits(),&error)||!desktop::resolve_numeric_source_format(expected,14,48000,channels,numeric_editor::limits(),&error))return false;
            if(graph.nodes.size()!=expected.nodes.size()||graph.edges.size()!=expected.edges.size()||!std::is_permutation(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),expected.configuration_bindings.begin(),expected.configuration_bindings.end()))return false;
            QSet<QPair<int,int>> positions;for(auto id:model->allNodeIds()){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}
            if(positions.size()!=int(graph.nodes.size())){std::fprintf(stderr,"%s overlapping startup positions: %d\n",label,int(positions.size()));return false;}
            return measure_shipped_example(graph,label,pipeline::AudioSourceKind::TestSignal)&&(!gui_workflow||measure_shipped_gui(app,window,label));
        };
        CHECK(wait_until([&]{QSet<QPair<int,int>> positions;for(auto id:model->allNodeIds()){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}return positions.size()==numeric_startup_graph().nodes.size();}));
        CHECK(inspect("missing-config"));
        for(const auto* id:{"startup","stereo-sources"}) {
            auto* action=window->findChild<QAction*>(QStringLiteral("example-")+id);CHECK(action&&action->isVisible()&&action->isEnabled());
            const auto revision=app.presenter().client().snapshot().graph_revision;action->trigger();
            std::fprintf(stderr,"apply example=%s status=%s warning=%s\n",qPrintable(action->objectName()),qPrintable(app.presenter().apply_status()),qPrintable(app.presenter().saved_graph_warning()));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision;}));
            CHECK(inspect(id));
        }
        CHECK(editor->load_preset("builtin-test-signal"));QCoreApplication::processEvents();
        CHECK(inspect("My-presets/builtin-test-signal"));
        CHECK(!QFile::exists(path));
        if(gui_workflow)CHECK(verify_example_autosave(app,window,path));
        std::puts("all shipped examples actual UI selection / synthetic full-chain Apply Run PASS");return 0;
    }
    if(argc>1 && QByteArray(argv[1])=="--first-run") {
        CHECK(!QFile::exists(path));auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        const auto fontFile=qEnvironmentVariable("NUMERIC_TEST_FONT_FILE");
        if(!fontFile.isEmpty()){const auto id=QFontDatabase::addApplicationFont(fontFile);if(id>=0){const auto families=QFontDatabase::applicationFontFamilies(id);if(!families.isEmpty())QApplication::setFont(QFont(families.front(),9));}}
        install_synthetic_capture_discovery(app,48000,2);
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        const auto graph=model->numeric_graph();
        if(graph.nodes.size()!=numeric_startup_graph().nodes.size())std::fprintf(stderr,"startup import warning=%s\n",qPrintable(app.presenter().saved_graph_warning()));
        CHECK(graph.nodes.size()==numeric_startup_graph().nodes.size() && graph.edges.size()==native_mono_example(numeric_startup_graph()).edges.size());
        const auto capture=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==13;});
        const auto clock=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==16;});
        CHECK(capture!=graph.nodes.end() && capture->kind=="AudioSource");
        const auto expected=native_mono_example(numeric_startup_graph());
        CHECK(std::is_permutation(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),expected.configuration_bindings.begin(),expected.configuration_bindings.end()));
        for(auto id:{domain::NodeId{13},domain::NodeId{14}}){
            const auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& n){return n.id==id;});
            CHECK(source!=graph.nodes.end()&&domain::data_outputs(*source).size()==1);
            CHECK(std::none_of(graph.edges.begin(),graph.edges.end(),[=](const auto& e){return e.from==id&&e.output_port!=0&&e.output_port!=domain::sample_rate_output;}));
        }
        for(auto id:{domain::NodeId{2}}){
            const auto buffer=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& n){return n.id==id;});CHECK(buffer!=graph.nodes.end());
            const auto output=std::get<signal_contracts::NumericContract>(buffer->outputs[0].contract.view());
            CHECK(output.shape.extents[0].maximum==(id==2?4096:512));
        }
        CHECK(clock!=graph.nodes.end() && clock->kind=="SampleClock");
        CHECK(std::get<domain::SampleClockProcessingParameters>(clock->parameters).sample_rate==48000);
        const auto selector=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.kind=="InputSelector";});CHECK(selector!=graph.nodes.end() && selector->inputs.size()==6 && domain::data_outputs(*selector).size()==2);
        const auto generator=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==1;});CHECK(generator!=graph.nodes.end()&&generator->kind=="SignalGenerator");
        CHECK(domain::has_configuration_binding(graph,1,domain::ConfigurationRole::SampleClock));
        domain::NumericProcessingGraph resolved;CHECK(domain::resolve_configuration_bindings(graph,numeric_editor::limits(),resolved));
        const auto resolved_generator=std::find_if(resolved.nodes.begin(),resolved.nodes.end(),[](const auto& n){return n.id==1;});CHECK(resolved_generator!=resolved.nodes.end()&&resolved_generator->clock_node==16);
        const auto& tones=std::get<domain::SignalGeneratorProcessingParameters>(generator->parameters).oscillators;
        CHECK(tones.size()==8 && tones[0].frequency_hz==73.25 && tones[0].amplitude==0.045);
        CHECK(numeric_default_graph().nodes[0].kind=="AudioSource");
        CHECK(app._ctx.file_configs.ui.pipeline_presets.size()==1);
        CHECK(app._ctx.file_configs.ui.pipeline_presets[0].toObject()["id"]=="builtin-test-signal");
        CHECK(app._ctx.file_configs.ui.show_raw_samples && app._ctx.file_configs.ui.show_amplitudes && app._ctx.file_configs.ui.show_test_amplitudes && app._ctx.file_configs.ui.show_spectrogram);
        CHECK(app.presenter().file_protection_warning().isEmpty() && app.presenter().client().snapshot().graph_revision==0);
        MainWindow* window=nullptr;for(auto* candidate:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();
        bool accepted=false,rejected=false;int pcm_frames=0;bool nonzero=false;
        QObject::connect(&app.presenter(),&GuiPresenter::graph_apply_finished,&app,[&](quint64,bool ok,const QString& error){accepted|=ok;rejected|=!ok;if(!ok)std::fprintf(stderr,"first-run Apply: %s\n",qPrintable(error));});
        const auto observeSource=[&](const pipeline::DeliveredNumericFrame& delivered){
            if(!delivered.context||delivered.context->endpoint.node!=1||!delivered.frame)return;
            const auto frame=delivered.frame.view();const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload);if(!numeric||!frame.stamp.available)return;
            float value=0;if(signal_contracts::load_element(*numeric,{0,100,0},value)==signal_contracts::Status::ok){++pcm_frames;nonzero|=std::abs(value)>1e-6f;}
        };
        QObject sourceWitness;
        const auto initialSourceTap=app.pipeline().subscribe_numeric_tap({{"main",1,domain::PortDirection::Output,0}},[&](pipeline::DeliveredNumericFrame delivered){QMetaObject::invokeMethod(&sourceWitness,[&,delivered]{observeSource(delivered);},Qt::QueuedConnection);},{});
        CHECK(initialSourceTap);
        struct InitialSourceGuard{pipeline::NumericEngine& engine;std::uint64_t token;~InitialSourceGuard(){engine.unsubscribe_numeric_tap(token);}} initialSourceGuard{app.pipeline(),initialSourceTap};
        CHECK(wait_until([&]{QSet<QPair<int,int>> positions;for(auto id:model->allNodeIds()){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}return positions.size()==numeric_startup_graph().nodes.size();}));
        const bool live=wait_until([&]{return rejected||(accepted && pcm_frames>=3 && nonzero);},4000);
        if(!live)std::fprintf(stderr,"first-run live: accepted=%d rejected=%d frames=%d nonzero=%d revision=%llu pending=%d status=%s\n",int(accepted),int(rejected),pcm_frames,int(nonzero),static_cast<unsigned long long>(app.presenter().client().snapshot().graph_revision),int(app.presenter().graph_request_pending()),qPrintable(app.presenter().apply_status()));
        CHECK(live);CHECK(accepted && !rejected && pcm_frames>=3 && nonzero);
        auto* frequencyDock=window->workspaceDock("fftDock");CHECK(frequencyDock);frequencyDock->toggleView(true);frequencyDock->raise();
        auto* frequencyPanel=qobject_cast<GraphPanel*>(frequencyDock->widget());CHECK(frequencyPanel);
        auto* resamplerModel=model->delegateModel<AudioNodeModel>(332);CHECK(resamplerModel);
        for(const auto rate:{36000u,48000u}){
            struct RateFrames{std::mutex mutex;pipeline::DeliveredNumericFrame latest;unsigned count=0;std::uint64_t sequence=0;bool positive=false,invalid=false;};
            const auto observed=std::make_shared<RateFrames>();
            const auto token=app.pipeline().subscribe_numeric_tap({{"main",332,domain::PortDirection::Output,0}},[observed](const pipeline::DeliveredNumericFrame& frame){
                if(!frame.frame || !frame.frame.view().stamp.available)return;const auto view=frame.frame.view();const auto* values=std::get_if<signal_contracts::NumericView>(&view.payload);if(!values)return;
                std::lock_guard lock(observed->mutex);if(observed->latest.delivery.graph_revision!=frame.delivery.graph_revision){observed->count=0;observed->sequence=0;observed->positive=observed->invalid=false;}
                observed->invalid|=view.stamp.sequence<=observed->sequence;observed->sequence=view.stamp.sequence;
                for(std::size_t i=0;i<values->actual[0];++i){float x=0;observed->invalid|=signal_contracts::load_element(*values,{0,i,0},x)!=signal_contracts::Status::ok || !std::isfinite(x);observed->positive|=std::abs(x)>1e-6f;}
                observed->latest=frame;++observed->count;
            },{});CHECK(token);
            struct TapGuard{pipeline::NumericEngine& engine;std::uint64_t token;~TapGuard(){engine.unsubscribe_numeric_tap(token);}} guard{app.pipeline(),token};
            auto* rateEdit=resamplerModel->embeddedWidget()->findChild<QLineEdit*>("output_rate");CHECK(rateEdit);
            const auto revision=app.presenter().client().snapshot().graph_revision;
            rateEdit->setText(QString::number(rate));rateEdit->setModified(true);QMetaObject::invokeMethod(rateEdit,"editingFinished",Qt::DirectConnection);
            CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>revision && !app.presenter().graph_request_pending();},5000));
            const auto appliedRate=app.presenter().applied_numeric_graph();CHECK(appliedRate);
            const auto actual=std::find_if(appliedRate->nodes.begin(),appliedRate->nodes.end(),[](const auto& n){return n.id==332;});CHECK(actual!=appliedRate->nodes.end());
            CHECK(std::get<signal_contracts::NumericContract>(actual->outputs[0].contract.view()).clock.sample_rate==rate);
            CHECK(std::none_of(appliedRate->nodes.begin(),appliedRate->nodes.end(),[](const auto& n){return n.kind=="SpectrumAxis";}));
            CHECK(std::any_of(appliedRate->edges.begin(),appliedRate->edges.end(),[](const auto& edge){return edge.to==10 && edge.input_port==domain::frequency_step_input;}));
            CHECK(wait_until([&]{auto* chart=frequencyPanel->active_chart();return chart && chart->value_count()>0 && std::abs(chart->projected_x_coordinate(0)-double(rate)/4096)<1e-9 && std::abs(chart->projected_x_coordinate(1)-2.*double(rate)/4096)<1e-9;},3000));
            CHECK(wait_until([&]{std::lock_guard lock(observed->mutex);return observed->latest.delivery.graph_revision>revision && observed->count>=3;},3000));
            {std::lock_guard lock(observed->mutex);CHECK(!observed->invalid && observed->positive);CHECK(std::get<signal_contracts::NumericView>(observed->latest.frame.view().payload).contract->clock.sample_rate==rate);}
        }
        const auto priorRevision=app.presenter().client().snapshot().graph_revision;
        CHECK(resamplerModel->parameter_model()->editLiteral("output_rate","32000",0));
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>priorRevision && !app.presenter().graph_request_pending();},5000));
        const auto applied=app.presenter().applied_numeric_graph();CHECK(applied);
        for(const auto& node:applied->nodes)if(node.id==332){CHECK(domain::data_outputs(node).size()==1);CHECK(std::any_of(node.inputs.begin(),node.inputs.end(),[](const auto& port){return port.id==domain::input_sample_rate_input;}));CHECK(std::get<signal_contracts::NumericContract>(domain::data_outputs(node)[0].contract.view()).clock.sample_rate==32000);}
        for(const auto& node:applied->nodes)CHECK(node.kind!="UInt32Constant" && node.kind!="BarkWeighting" && node.kind!="Mel" && node.kind!="ComplexPolar");
        const auto step=std::find_if(applied->nodes.begin(),applied->nodes.end(),[](const auto& n){return n.kind=="FrequencyStep";});CHECK(step!=applied->nodes.end());
        CHECK(std::get<signal_contracts::NumericContract>(step->outputs[0].contract.view()).dtype==signal_contracts::DType::Float64);
        CHECK(std::any_of(applied->edges.begin(),applied->edges.end(),[&](const auto& edge){return edge.from==step->id && edge.input_port==domain::frequency_step_input;}));
        const auto beforeWindowRevision=app.presenter().client().snapshot().graph_revision;
        auto* buffer=model->delegateModel<AudioNodeModel>(2);CHECK(buffer->parameter_model()->editLiteral("size","2048",0));
        const bool windowApplied=wait_until([&]{return app.presenter().client().snapshot().graph_revision>beforeWindowRevision && !app.presenter().graph_request_pending() && std::get<signal_contracts::NumericContract>(model->delegateModel<AudioNodeModel>(4)->numeric_node()->outputs[0].contract.view()).shape.extents[0].maximum==1024;},5000);
        if(!windowApplied){
            const auto draft=model->numeric_graph();domain::NumericProcessingGraph evaluated,executable;const auto admission=domain::resolve_numeric_parameter_bindings(draft,numeric_editor::limits(),evaluated,executable);
            std::fprintf(stderr,"first-run window: prior=%llu current=%llu pending=%d status=%s admission=%u node=%d port=%u\n",static_cast<unsigned long long>(beforeWindowRevision),static_cast<unsigned long long>(app.presenter().client().snapshot().graph_revision),int(app.presenter().graph_request_pending()),qPrintable(app.presenter().apply_status()),unsigned(admission.error),admission.node,unsigned(admission.port));
            for(const auto& n:draft.nodes)if(n.id==2 || n.id==4 || n.kind=="ArrayLength" || n.kind=="WindowFunction" || n.kind=="ArrayMultiply" || n.kind=="ArrayFill"){
                std::fprintf(stderr,"draft node=%d kind=%s",n.id,n.kind.c_str());for(const auto& p:n.outputs){const auto value=p.contract.view();if(const auto* c=std::get_if<signal_contracts::NumericContract>(&value))std::fprintf(stderr," out%u=%llu",unsigned(p.id),static_cast<unsigned long long>(c->shape.extents[0].maximum));}std::fprintf(stderr,"\n");
            }
            if(const auto applied=app.presenter().applied_numeric_graph())for(const auto& n:applied->nodes)if(n.id==2 || n.id==4)std::fprintf(stderr,"applied node=%d output=%llu\n",n.id,static_cast<unsigned long long>(std::get<signal_contracts::NumericContract>(n.outputs[0].contract.view()).shape.extents[0].maximum));
        }
        CHECK(windowApplied);
        auto* windowFunction=model->delegateModel<AudioNodeModel>(3);CHECK(windowFunction);
        const auto beforeFunctionRevision=app.presenter().client().snapshot().graph_revision;
        CHECK(windowFunction->parameter_model()->edit("function",1));
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>beforeFunctionRevision && !app.presenter().graph_request_pending();},5000));
        const auto changedWindow=app.presenter().applied_numeric_graph();CHECK(changedWindow);
        for(const auto& n:changedWindow->nodes)if(n.kind=="AmplitudePhase"){
            domain::ResolvedProcessingNode r;domain::ProcessingParameterView gain;CHECK(domain::resolve_processing_node(n,numeric_editor::limits(),r));CHECK(domain::resolve_processing_parameter(r,"coherent_gain",gain)==signal_contracts::Status::ok);
            const auto cg=std::visit([](auto v){return double(v);},std::get<signal_contracts::NumericLiteral>(gain));CHECK(std::abs(cg-(.54*2048-.46))<1e-3);
            CHECK(domain::resolve_processing_parameter(r,"power_gain",gain)==signal_contracts::Status::ok);
            const auto energy=std::visit([](auto v){return double(v);},std::get<signal_contracts::NumericLiteral>(gain));CHECK(std::abs(energy-(.3974*2048-.391))<1e-3);
            CHECK(std::none_of(n.inputs.begin(),n.inputs.end(),[](const auto& port){return port.id==domain::window_size_input;}));
        }
        for(const auto& n:changedWindow->nodes){CHECK(n.kind!="Reroute" && n.kind!="FrequencyNormalizer");if(n.kind=="ArrayMultiply")CHECK(n.inputs.size()==2 && domain::data_outputs(n).size()==1);}
        const auto spectral=std::find_if(changedWindow->nodes.begin(),changedWindow->nodes.end(),[](const auto& n){return n.kind=="AmplitudePhase";});CHECK(spectral!=changedWindow->nodes.end());
        auto* projectionModel=model->delegateModel<AudioNodeModel>(spectral->id);CHECK(projectionModel);
        const auto* representation=projectionModel->parameter_model()->field("representation");CHECK(representation && representation->primary && representation->options.size()==4);
        const signal_contracts::Quantity quantities[]{signal_contracts::Quantity::SpectrumMagnitude,signal_contracts::Quantity::SpectrumPeakAmplitude,signal_contracts::Quantity::SpectrumPower,signal_contracts::Quantity::SpectrumPsd};
        for(int mode=0;mode<4;++mode){
            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(projectionModel->parameter_model()->edit("representation",mode));
            CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>revision && !app.presenter().graph_request_pending();},5000));
            const auto current=app.presenter().applied_numeric_graph();CHECK(current);
            for(const auto& n:current->nodes)if(n.kind=="AmplitudePhase" || n.id==15)CHECK(std::get<signal_contracts::NumericContract>(n.outputs[0].contract.view()).quantity==quantities[mode]);
        }
        const auto beforeRestoreAmplitude=app.presenter().client().snapshot().graph_revision;
        CHECK(projectionModel->parameter_model()->edit("representation",1));
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>beforeRestoreAmplitude && !app.presenter().graph_request_pending();},5000));
        auto* rowModel=model->delegateModel<AudioNodeModel>(6);CHECK(rowModel && rowModel->numeric_node()->kind=="BroadbandRow");
        AudioNodeModel* noteModel=nullptr;for(const auto id:model->allNodeIds())if(auto* candidate=model->delegateModel<AudioNodeModel>(id);candidate && candidate->numeric_node()->kind=="NoteSpectrum")noteModel=candidate;CHECK(noteModel);
        const auto noteId=noteModel->numeric_node()->id;
        const auto rowNumber=[&](const domain::ConfiguredProcessingNode& node,const char* key){
            domain::ResolvedProcessingNode resolved;domain::ProcessingParameterView value;
            if(!domain::resolve_processing_node(node,numeric_editor::limits(),resolved) || domain::resolve_processing_parameter(resolved,key,value)!=signal_contracts::Status::ok || !std::holds_alternative<signal_contracts::NumericLiteral>(value))return UINT64_MAX;
            return std::visit([](auto n){return std::uint64_t(n);},std::get<signal_contracts::NumericLiteral>(value));
        };
        const auto rowGeometry=[&]()->bool {
            using namespace signal_contracts;const auto current=app.presenter().applied_numeric_graph();if(!current)return false;
            const auto row=std::find_if(current->nodes.begin(),current->nodes.end(),[](const auto& n){return n.id==6;});
            if(row==current->nodes.end() || domain::data_outputs(*row).size()!=1 || row->inputs.size()!=2)return false;
            const auto note=std::find_if(current->nodes.begin(),current->nodes.end(),[&](const auto& n){return n.id==noteId;});if(note==current->nodes.end() || note->outputs.size()!=4 || note->inputs.size()!=2)return false;
            const auto input=std::get<NumericContract>(row->inputs[0].contract.view());std::array<domain::OwnedProcessingContract,4> expected;std::array<std::size_t,4> sections{};
            if(domain::musical_spectrogram_output_contracts(*row,input,expected,&sections)!=Status::ok)return false;
            for(unsigned p=0;p<domain::data_outputs(*row).size();++p)if(row->outputs[p].id!=p || !same_contract(row->outputs[p].contract.view(),expected[p].view()))return false;
            std::array<domain::OwnedProcessingContract,4> noteExpected;std::array<std::size_t,4> noteSections{};if(domain::musical_spectrogram_output_contracts(*note,std::get<NumericContract>(note->inputs[0].contract.view()),noteExpected,&noteSections)!=Status::ok)return false;
            for(unsigned p=0;p<4;++p)if(!same_contract(note->outputs[p].contract.view(),noteExpected[p].view()))return false;
            const auto total=sections[0]+sections[1]+sections[2]+noteSections[3];
            const auto notes=std::get<NumericContract>(note->outputs[0].contract.view());
            if(noteSections[3]!=rowNumber(*note,"last_midi")-rowNumber(*note,"first_midi")+1 || notes.shape.extents[0].maximum!=noteSections[3] || notes.coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE || notes.coordinates[0].unit!=NumericUnit::Hertz)return false;
            const auto history=std::find_if(current->nodes.begin(),current->nodes.end(),[](const auto& n){return n.id==7;});
            const auto view=std::find_if(current->nodes.begin(),current->nodes.end(),[](const auto& n){return n.id==8;});
            const auto joined=std::find_if(current->nodes.begin(),current->nodes.end(),[](const auto& n){return n.kind=="ArrayConcat";});
            if(history==current->nodes.end() || view==current->nodes.end() || joined==current->nodes.end() || !same_contract(history->inputs[0].contract.view(),joined->outputs[0].contract.view()) || !same_contract(history->outputs[0].contract.view(),view->inputs[0].contract.view()))return false;
            const auto matrix=std::get<NumericContract>(history->outputs[0].contract.view());
            if(matrix.shape.extents[0].maximum!=400 || matrix.shape.extents[1].maximum!=total)return false;
            unsigned scores=0;
            for(const auto& n:current->nodes)if(n.kind=="ScalarView" && n.inputs.size()==2){
                if(!n.enabled)return false;
                const auto* ui=model->delegateModel<AudioNodeModel>(QtNodes::NodeId(n.id));if(!ui)return false;
                for(unsigned p=0;p<2;++p){
                    if(!same_contract(n.inputs[p].contract.view(),note->outputs[p+1].contract.view()) || !ui->presentation()["scalar_inputs"].toObject().value(QString::number(p)).toObject().value("enabled").toBool(true))return false;
                    if(std::none_of(current->edges.begin(),current->edges.end(),[&](const auto& e){return e.from==noteId && e.output_port==p+1 && e.to==n.id && e.input_port==p;}))return false;
                    ++scores;
                }
            }
            return scores==2;
        };
        const auto rowEdit=[&](const char* label,const auto& edit,AudioNodeModel* edited=nullptr)->bool {
            const auto prior=app.presenter().client().snapshot().graph_revision;
            if(!edit()){std::fprintf(stderr,"row control rejected: %s status=%s\n",label,qPrintable(app.presenter().apply_status()));return false;}
            const auto desired=*(edited?edited:rowModel)->numeric_node();
            const bool ready=wait_until([&]{
                if(app.presenter().graph_request_pending() || app.presenter().client().snapshot().graph_revision<=prior)return false;
                const auto current=app.presenter().applied_numeric_graph();if(!current)return false;
                const auto row=std::find_if(current->nodes.begin(),current->nodes.end(),[&](const auto& n){return n.id==desired.id;});
                return row!=current->nodes.end() && domain::same_processing_parameters(row->parameters,desired.parameters) && rowGeometry();
            },5000);
            if(!ready){auto* validation=window->findChild<QLabel*>("pipelineValidation");std::fprintf(stderr,"row control timeout: %s prior=%llu current=%llu pending=%d status=%s validation=%s\n",label,(unsigned long long)prior,(unsigned long long)app.presenter().client().snapshot().graph_revision,int(app.presenter().graph_request_pending()),qPrintable(app.presenter().apply_status()),validation?qPrintable(validation->toolTip()):"");}
            return ready;
        };
        CHECK(rowEdit("first MIDI 60",[&]{return noteModel->parameter_model()->editLiteral("first_midi","60",0);},noteModel));
        CHECK(rowEdit("last MIDI 72",[&]{return noteModel->parameter_model()->editLiteral("last_midi","72",0);},noteModel));
        CHECK(rowEdit("high count 4",[&]{return rowModel->parameter_model()->editLiteral("high_count","4",0);}));
        CHECK(rowEdit("mid RMS",[&]{return rowModel->parameter_model()->edit("mid_reduction",2);}));
        CHECK(rowEdit("mid weighted, uniform default",[&]{return rowModel->parameter_model()->edit("mid_reduction",3);}));
        const auto* weights=rowModel->parameter_model()->field("weights");CHECK(weights && std::get<QString>(weights->read()).isEmpty());
        std::array<domain::OwnedProcessingContract,4> beforeLow;std::array<std::size_t,4> passSections{};
        CHECK(domain::musical_spectrogram_output_contracts(*rowModel->numeric_node(),std::get<signal_contracts::NumericContract>(rowModel->numeric_node()->inputs[0].contract.view()),beforeLow,&passSections)==signal_contracts::Status::ok);
        CHECK(rowEdit("low pass through to mean",[&]{return rowModel->parameter_model()->edit("low_reduction",1);}));
        CHECK(rowNumber(*rowModel->numeric_node(),"low_count")==std::max<std::size_t>(1,passSections[0]));
        CHECK(rowEdit("low count 4",[&]{return rowModel->parameter_model()->editLiteral("low_count","4",0);}));
        CHECK(rowEdit("low mean to pass through",[&]{return rowModel->parameter_model()->edit("low_reduction",0);}));
        CHECK(rowNumber(*rowModel->numeric_node(),"low_count")==0 && rowGeometry());
        std::array<domain::OwnedProcessingContract,4> finalRows;std::array<std::size_t,4> finalSections{};
        CHECK(domain::musical_spectrogram_output_contracts(*rowModel->numeric_node(),std::get<signal_contracts::NumericContract>(rowModel->numeric_node()->inputs[0].contract.view()),finalRows,&finalSections)==signal_contracts::Status::ok);
        CHECK(finalSections[0]==passSections[0] && finalSections[1]==16 && finalSections[2]==4 && finalSections[3]==0 && std::get<signal_contracts::NumericContract>(noteModel->numeric_node()->outputs[0].contract.view()).shape.extents[0].maximum==13);
        bool scoreControlsChecked=false;
        for(const auto id:model->allNodeIds())if(auto* scalar=model->delegateModel<AudioNodeModel>(id);scalar && scalar->numeric_node()->kind=="ScalarView" && scalar->numeric_node()->inputs.size()==2){
            std::unique_ptr<QWidget> controls(scalar->scalar_inputs_widget());
            auto* full=controls->findChild<QCheckBox*>("scalarInput0Enabled");auto* selected=controls->findChild<QCheckBox*>("scalarInput1Enabled");
            CHECK(full && selected && full->isChecked() && selected->isChecked());scoreControlsChecked=true;
        }
        CHECK(scoreControlsChecked);
        auto* pianoEditor=window->findChild<SchemeEditorWidget*>();CHECK(pianoEditor);emit pianoEditor->graph_open_requested(11,0);
        GraphPanel* pianoPanel=nullptr;
        CHECK(wait_until([&]{for(auto* dock:window->workspaceDocks())if(dock->property("graphNode").toInt()==11){auto* panel=qobject_cast<GraphPanel*>(dock->widget());if(panel && panel->piano_view()){pianoPanel=panel;return panel->piano_view()->first_midi()==60 && panel->piano_view()->value_count()==13 && panel->piano_view()->peak_value()>0;}}return false;},5000));
        CHECK(pianoPanel && pianoPanel->display_mode()=="piano" && !pianoPanel->active_chart());
        auto* previewScene=pianoEditor->findChild<QtNodes::DataFlowGraphicsScene*>();CHECK(previewScene);
        emit previewScene->nodeSelected(QtNodes::NodeId(noteModel->numeric_node()->id));
        CHECK(wait_until([&]{auto* values=pianoEditor->findChild<QLabel*>("previewValues");return values && values->text().split('\n').value(0)=="13";},3000));
        emit previewScene->nodeSelected(QtNodes::InvalidNodeId);
        CHECK(!pianoEditor->findChild<QLabel*>("previewValues"));
        std::puts("live independent broadband/note controls: explicit join, notes Hz, independent History geometry, two enabled agreement scalars PASS");
        const auto tracePath=qEnvironmentVariable("NUMERIC_TEST_PERFORMANCE_REPORT");
        if(!tracePath.isEmpty()){
            std::atomic<unsigned> sourceFrames{0};
            const auto sourceTap=app.pipeline().subscribe_numeric_tap({{"main",1,domain::PortDirection::Output,0}},[&](const pipeline::DeliveredNumericFrame& frame){if(frame.frame && frame.frame.view().stamp.available)sourceFrames.fetch_add(1,std::memory_order_relaxed);},{});
            CHECK(sourceTap);
            struct SourceTapGuard{pipeline::NumericEngine& engine;std::uint64_t token;~SourceTapGuard(){engine.unsubscribe_numeric_tap(token);}} sourceGuard{app.pipeline(),sourceTap};
            QElapsedTimer warmup;warmup.start();CHECK(wait_until([&]{return warmup.elapsed()>=750;},2000));
            QElapsedTimer sample;sample.start();qint64 prior=sample.nsecsElapsed(),maxGap=0,totalGap=0;unsigned beats=0;
            QTimer heartbeat;heartbeat.setInterval(10);heartbeat.setTimerType(Qt::PreciseTimer);
            QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{const auto now=sample.nsecsElapsed(),gap=now-prior;prior=now;maxGap=std::max(maxGap,gap);totalGap+=gap;++beats;});
            const auto before=sourceFrames.load(std::memory_order_relaxed);core::perf::enabled=true;heartbeat.start();
            CHECK(wait_until([&]{return sample.elapsed()>=3000;},5000));heartbeat.stop();core::perf::enabled=false;
            const auto fresh=sourceFrames.load(std::memory_order_relaxed)-before;CHECK(fresh>0);
            std::fprintf(stderr,"MONO_PERF duration_ms=%lld nodes=%zu edges=%zu gui_heartbeat_count=%u mean_gap_ms=%.3f max_gap_ms=%.3f source_frames=%u\n",(long long)sample.elapsed(),graph.nodes.size(),graph.edges.size(),beats,beats?totalGap/1e6/beats:0,maxGap/1e6,fresh);
            CHECK(save_performance_report(tracePath));
        }
        std::puts("missing-config mono audio/Fs selector, local resampler and automatic live delivery PASS");return 0;
    }
    if (argc==3 && QByteArray(argv[1])=="--reload") {
        auto encoded=QByteArray(argv[2]); char executable[]="numeric_application_tests", option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr}; int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        install_synthetic_capture_discovery(app);
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
        const QByteArray rejected=R"({"schema_version":4,"scene_graph":{"version":5,"processing_version":4,"nodes":"invalid","connections":[]}})";
        QFile file(path); CHECK(file.open(QIODevice::WriteOnly)); file.write(rejected); file.close();
        auto encoded=path.toLocal8Bit(); char executable[]="numeric_application_tests", option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr}; int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        install_synthetic_capture_discovery(app);
        CHECK(app._ctx.data_flow_graph->allNodeIds().empty());
        CHECK(app.presenter().client().snapshot().graph_revision==0);
        CHECK(!app.presenter().file_protection_warning().isEmpty());
        MainWindow* diagnosticWindow=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))diagnosticWindow=candidate;
        CHECK(diagnosticWindow);
        auto* saveAction=diagnosticWindow->findChild<QAction*>("saveSettings");CHECK(saveAction);
        CHECK(saveAction->shortcut()==QKeySequence("Ctrl+S") && saveAction->shortcutContext()==Qt::ApplicationShortcut);
        // A rejected draft must not swallow the explicit save warning.
        CHECK(respond_to_overwrite(false,[&]{trigger_save_action(diagnosticWindow,saveAction);},path));CHECK(read(path)==rejected);
        CHECK(respond_to_overwrite(true,[&]{trigger_save_action(diagnosticWindow,saveAction);},path));CHECK(read(path)==rejected);

        auto* diagnosticEditor=diagnosticWindow->findChild<SchemeEditorWidget*>();CHECK(diagnosticEditor);
        auto* warning=diagnosticEditor->toolbar()->findChild<QLabel*>("pipelineValidation");
        CHECK(warning && !warning->isHidden() && warning->toolTip().contains(app.presenter().file_protection_warning()));
        CHECK(!diagnosticWindow->findChild<QLabel*>("savedPipelineWarning") && !diagnosticWindow->findChild<QLabel*>("graphApplyStatus"));
        diagnosticWindow->statusBar()->clearMessage();
        CHECK(!warning->isHidden() && !warning->toolTip().isEmpty());
        app.save_configuration_as(next);
        CHECK(!QFile::exists(next) && read(path)==rejected);
        CHECK(diagnosticWindow->findChild<QLabel*>("configurationSaveStatus")->property("saveState").toInt()==int(MainWindow::ConfigurationSaveState::LoadError));
        auto* example=diagnosticWindow->findChild<QAction*>("example-startup");CHECK(example);example->trigger();
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>0;}));
        CHECK(respond_to_overwrite(false,[&]{app.save_configuration_as(path);}));CHECK(read(path)==rejected);
#ifdef Q_OS_WIN
        CHECK(respond_to_overwrite(false,[&]{app.save_configuration_as(path.toUpper());}));CHECK(read(path)==rejected);
        CHECK(!app.presenter().file_protection_warning().isEmpty());
#endif
        unsigned discoveryCalls=0;
        auto* saveModel=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(saveModel);
        saveModel->set_capture_device_provider([&discoveryCalls](int,QString*) {
            ++discoveryCalls;
            return QJsonArray{QJsonObject{{"id","synthetic-capture"},{"name","Synthetic capture"},{"is_default",true},{"native_sample_rate",1000},{"native_channels",1}}};
        });
        discoveryCalls=0; // Provider installation itself synchronizes device formats.
        domain::NumericProcessingGraph storageDraft;QString storageError;
        CHECK(diagnosticEditor->numeric_draft(storageDraft,&storageError,false));CHECK(discoveryCalls==0);
        CHECK(!diagnosticEditor->numeric_draft(storageDraft,&storageError));CHECK(discoveryCalls>0);
        // UI diagnostics may query devices while the modal runs. Saving must
        // nevertheless succeed with the valid stored contracts, despite that error.
        CHECK(respond_to_overwrite(true,[&]{trigger_save_action(diagnosticWindow,saveAction);}));
        install_synthetic_capture_discovery(app);
        const auto replaced=read(path);CHECK(replaced!=rejected && !replaced.isEmpty());
        CHECK(app.presenter().file_protection_warning().isEmpty());
        desktop::NumericProjectDocument replacement;domain::NumericProcessingGraph replacementGraph;CHECK(desktop::load_numeric_project(path,replacement,replacementGraph));
        app.save_configuration_as(next);CHECK(QFile::exists(next)&&read(path)==replaced);
        const auto nextBeforeCancel=read(next);CHECK(save_without_overwrite([&]{app.save_configuration_as(next);}));CHECK(read(path)==replaced && read(next)==nextBeforeCancel);
        CHECK(app.presenter().file_protection_warning().isEmpty());
        desktop::NumericProjectDocument recovered;domain::NumericProcessingGraph recovered_graph;
        CHECK(desktop::load_numeric_project(next,recovered,recovered_graph));CHECK(std::any_of(recovered_graph.nodes.begin(),recovered_graph.nodes.end(),[](const auto& n){return n.id==1&&n.kind=="SignalGenerator";}));
        CHECK(save_without_overwrite([&]{emit diagnosticWindow->on_config_updated();}));CHECK(read(path)==replaced);
        std::puts("numeric rejected startup PASS"); return 0;
    }
    desktop::NumericProjectDocument document;
    CHECK(desktop::read_numeric_project(R"({"schema_version":4,"opaque":{"exact":1e9999,"integer":18446744073709551615},"ui":{"autosave_enabled":true,"show_spectrogram":false,"pipeline_presets":[{"version":1,"id":"opaque-preset","name":"opaque","opaque":1e9999,"graph":{"version":5,"processing_version":4,"graph_id":"main","nodes":[],"connections":[],"configuration_bindings":[]}}],"editor_view":{"version":1,"scale":1,"center_x":123,"center_y":456,"extension":1e9999}},"pipeline_graph":{"version":5,"processing_version":4,"graph_id":"main","nodes":[],"connections":[],"configuration_bindings":[],"extension":7}})",document));
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
    install_synthetic_capture_discovery(app);
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
    CHECK(save_without_overwrite([&]{emit window->on_config_updated();})); // Normal Ctrl+S has no warning.
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
    CHECK(save_without_overwrite([&]{emit window->on_config_updated();}));
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error)); CHECK(graph.nodes.empty());
    QFile external(next); CHECK(external.open(QIODevice::WriteOnly)); external.write("{\"schema_version\":100}"); external.close();
    app._ctx.editor_draft_modified=true; CHECK(respond_to_overwrite(false,[&]{emit window->on_config_updated();}));
    CHECK(read(next)=="{\"schema_version\":100}"); CHECK(app._ctx.editor_draft_modified);
    CHECK(app.presenter().client().snapshot().graph_revision==startup_revision);
    CHECK(respond_to_overwrite(true,[&]{emit window->on_config_updated();}));
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error) && graph.nodes.empty());
    auto* saveAction=window->findChild<QAction*>("saveSettings");CHECK(saveAction);
    CHECK(saveAction->shortcut()==QKeySequence("Ctrl+S"));
    CHECK(QFile::remove(next)); // Only this test's temporary destination.
    CHECK(save_without_overwrite([&]{trigger_save_action(window,saveAction);}));
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error) && graph.nodes.empty());
    auto* flowDock=window->workspaceDock("dataFlowDock");CHECK(flowDock);
    flowDock->setFloating(true);
    CHECK(window->data_flow_editor()==editor);
    CHECK(save_without_overwrite([&]{trigger_save_action(window,saveAction);}));
    CHECK(window->findChild<QLabel*>("configurationSaveStatus")->property("saveState").toInt()==int(MainWindow::ConfigurationSaveState::Saved));
    flowDock->toggleView(false);
    CHECK(window->data_flow_editor()==editor);
    CHECK(save_without_overwrite([&]{trigger_save_action(window,saveAction);}));
    CHECK(window->findChild<QLabel*>("configurationSaveStatus")->property("saveState").toInt()==int(MainWindow::ConfigurationSaveState::Saved));
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error) && graph.nodes.empty());
    std::puts("canonical numeric application/presets/restart PASS"); return 0;
}
