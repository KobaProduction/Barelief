#include "synthetic_capture.h"
#include <QWheelEvent>
#include <QMenu>
#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "core_app/scheme_editor/editor_widget.h"
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/GraphicsView>
#include <QtNodes/DataFlowGraphicsScene>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDockWidget>
#include <QToolButton>
#include <QScreen>
#include <QProcess>
#include <QToolBar>
#include "core_app/scheme_editor/audio_node_model.h"
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QUndoStack>
#include <QMouseEvent>
#include <QUdpSocket>
#include <thread>
#include <cmath>
#include <QtTest/QTest>
#include <stdexcept>
static void require(bool ok,const char *message) { if (!ok) throw std::runtime_error(message); }
int main(int argc,char **argv) {
    try {
        QTemporaryDir directory;
        const bool dockControls = argc > 1 && QString::fromLocal8Bit(argv[1]) == "--dock-controls";
        bool restoring = argc > 1 && !dockControls;
        const auto path = restoring ? QString::fromLocal8Bit(argv[1]) : directory.filePath("workspace.json");
        const auto initial_graph = pipeline::GraphProgram::defaults();
        if (!restoring) {
            QFile file(path); require(file.open(QIODevice::WriteOnly),"Cannot create fixture");
            auto fixture=QJsonDocument::fromJson(R"({"core":{"run_loopback":false,"run_analyser":false},"ui":{"autosave_enabled":false,"show_spectrogram":true,"workspace":{"extension":{"keep":24}},"editor_view":{"extension":{"keep":25}}},"extension":{"keep":23}})").object();
            fixture["scene_graph"] = initial_graph;
            file.write(QJsonDocument(fixture).toJson());
        }
        auto bytes=path.toLocal8Bit(); char name[]="workspace_tests",flag[]="--config";
        char *args[]{name,flag,bytes.data(),nullptr};int count=3; CoreApplication app(count,args, test_fixture::synthetic_capture_factory(false));
        MainWindow *window=nullptr;
        for(auto *widget:QApplication::topLevelWidgets()) if(auto *candidate=qobject_cast<MainWindow *>(widget)) window=candidate;
        require(window,"Main window absent"); window->show(); QTest::qWait(450);
        if(dockControls) {
            auto *dock=window->findChild<QDockWidget *>("fftDock");require(dock,"FFT dock absent");
            dock->show();auto *strip=dock->titleBarWidget();
            auto *toggle=dock->findChild<QToolButton *>("graphPlacement");
            require(strip && toggle,"Dock controls absent");
            for(int round=0;round<3;++round) {
                toggle->menu()->actions()[0]->trigger();QTest::qWait(10);
                require(dock->isFloating() && dock->titleBarWidget()==strip && strip->isVisible() && toggle->isVisible(),
                        "Floating graph lost draggable title and return control");
                toggle->menu()->actions()[1]->trigger();QTest::qWait(10);
                require(!dock->isFloating() && dock->titleBarWidget()==strip,"Cannot reattach graph");
            }
            window->close();qInfo("Dock detach/reattach controls passed");return 0;
        }
        if (argc > 2) {
            const auto mode = QString::fromLocal8Bit(argv[2]);
            auto *surface = window->findChild<QDockWidget *>("spectrogramDock");
            auto *pipeline = window->findChild<QDockWidget *>("dataFlowDock");
            auto *frequency = window->findChild<QDockWidget *>("fftDock");
            require(surface && pipeline && frequency, "Persistence probe docks absent");
            QFile config_file(path);
            auto bytes_on_disk = [&] { require(config_file.open(QIODevice::ReadOnly), "Probe read failed");
                const auto bytes = config_file.readAll(); config_file.close(); return bytes; };
            if (mode == "--probe-initial" || mode == "--probe-corrupt") {
                require(!surface->isHidden() && !pipeline->isHidden(), "Stale flags or damaged workspace hid defaults");
                if (mode == "--probe-initial") {
                    frequency->toggleViewAction()->trigger();
                    QTest::qWait(30);
                    require(!surface->isHidden(), "Switching dock tab closed Spectrogram");
                    frequency->setFloating(true);
                    surface->hide();
                    QTest::qWait(450);
                }
            } else if (mode == "--probe-restored") {
                require(surface->isHidden() && !pipeline->isHidden() && !frequency->isHidden() && frequency->isFloating(),
                        "Process restart lost hidden/open/floating dock preferences");
            } else if (mode == "--probe-manual") {
                require(!app._ctx.file_configs.ui.autosave_enabled, "Manual probe needs autosave off");
                const auto original = bytes_on_disk();
                surface->show(); frequency->hide(); pipeline->show(); QTest::qWait(450);
                require(bytes_on_disk() == original, "Autosave-off dock actions wrote the config");
                window->findChild<QAction *>("saveSettings")->trigger();
                require(bytes_on_disk() != original, "Manual Save did not flush current dock visibility");
            } else if (mode == "--probe-manual-restored") {
                require(!surface->isHidden() && !pipeline->isHidden() && frequency->isHidden(),
                        "Manual Save was not restored after process restart");
            } else require(false, "Unknown persistence probe");
            window->close();
            qInfo("Workspace process persistence probe passed: %s", qPrintable(mode));
            return 0;
        }
        auto *editor=window->findChild<SchemeEditorWidget *>(); auto *scene=editor->findChild<QtNodes::DataFlowGraphicsScene *>();
        auto *view=editor->findChild<QtNodes::GraphicsView *>();
        auto *flow=window->findChild<QDockWidget *>("dataFlowDock"), *fft=window->findChild<QDockWidget *>("fftDock");
        require(editor && scene && view && flow && fft,"Dock/editor missing");
        require(window->findChildren<QDockWidget *>().size()==8,"Views not represented by eight unique docks");
        auto &graph=*app._ctx.data_flow_graph;
        if (restoring) {
            require(fft->isFloating() && !flow->isFloating(),"Floating/attached workspace not restored");
            require(graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()==QPointF(1234,567),"Position not restored after restart");
            require(!app._ctx.file_configs.ui.editor_view.isEmpty(),"Editor viewport absent after restart");
            require(app._ctx.file_configs.ui.pipeline_presets.size()==1,"Custom presets lost after restart");
            window->close(); return 0;
        }
        auto *spectrogram = window->findChild<QDockWidget *>("spectrogramDock");
        require(spectrogram && !spectrogram->isHidden() && !flow->isHidden() && !fft->isHidden() &&
                    window->tabifiedDockWidgets(spectrogram).contains(fft),
                "Default workspace must expose graph nodes as tabs above Data Flow");
        require(flow->geometry().top() >= spectrogram->geometry().bottom(),
                "Pipeline is not below Spectrogram in default workspace");
        auto *graphs_menu=window->findChild<QMenu *>("graphsMenu");
        require(graphs_menu && !graphs_menu->actions().contains(fft->toggleViewAction()),"Display retained removed graph selector");
        auto *placement = fft->findChild<QToolButton *>("graphPlacement");
        require(placement && placement->menu() && placement->menu()->actions().size()==4,"Graph placement controls missing");
        placement->menu()->actions()[1]->trigger();
        require(!fft->isHidden() && !spectrogram->isHidden() && window->tabifiedDockWidgets(spectrogram).contains(fft),
                "Graph placement did not add a switchable tab");
        fft->close();
        require(fft->isHidden() && !spectrogram->isHidden() && !flow->isHidden(),"Graph close affected other panels");
        window->findChild<QAction *>("resetWorkspace")->trigger(); QTest::qWait(40);
        auto *pipeline_action = window->findChild<QAction *>("openPipeline");
        pipeline_action->trigger();
        require(flow->isHidden() && !spectrogram->isHidden(), "Pipeline toggle hid the top graph");
        pipeline_action->trigger();
        require(!flow->isHidden(), "Pipeline toolbar toggle did not reopen the bottom panel");
        auto *zoom=editor->findChild<QDoubleSpinBox *>("pipelineZoom");
        require(zoom && editor->findChild<QPushButton *>("fitPipeline"),"Editor zoom controls missing");
        const auto zoom_revision=app.pipeline().graph_revision();
        const QPoint cursor=view->viewport()->rect().center()+QPoint(25,15);
        const auto anchor=view->mapToScene(cursor); const auto previous_scale=view->getScale();
        QWheelEvent wheel(cursor,view->viewport()->mapToGlobal(cursor),QPoint(),QPoint(0,60),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(view->viewport(),&wheel);
        require(view->getScale()>previous_scale && view->getScale()<previous_scale*1.1,"Wheel ignores proportional delta");
        require(QLineF(anchor,view->mapToScene(cursor)).length()<3/view->getScale(),"Zoom moved point under cursor");
        zoom->setValue(300); require(std::abs(view->getScale()-3)<1e-6,"Explicit zoom not applied");
        zoom->setValue(20); require(std::abs(view->getScale()-0.2)<1e-6,"Zoom lower bound not applied");
        zoom->setValue(100); QTest::qWait(400);
        require(app.pipeline().graph_revision()==zoom_revision,"Editor zoom applied DSP graph");
        for(auto a:graph.allNodeIds()) for(auto b:graph.allNodeIds()) if(a<b)
            require(!scene->nodeGraphicsObject(a)->sceneBoundingRect().intersects(scene->nodeGraphicsObject(b)->sceneBoundingRect()),"Initial layout overlaps");
        const auto revision=app.pipeline().graph_revision();
        auto *node=scene->nodeGraphicsObject(0); const auto old=graph.nodeData(0,QtNodes::NodeRole::Position).toPointF();
        view->centerOn(node->sceneBoundingRect().center());
        const auto press=view->mapFromScene(node->sceneBoundingRect().topLeft()+QPointF(30,15));
        QTest::mousePress(view->viewport(),Qt::LeftButton,Qt::NoModifier,press);
        QMouseEvent drag(QEvent::MouseMove,QPointF(press+QPoint(45,25)),
            QPointF(view->viewport()->mapToGlobal(press+QPoint(45,25))),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(view->viewport(),&drag);
        QTest::mouseRelease(view->viewport(),Qt::LeftButton,Qt::NoModifier,press+QPoint(45,25));
        require(graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()!=old,"Real node drag did not move it");
        const auto dragged=graph.nodeData(0,QtNodes::NodeRole::Position).toPointF();
        scene->undoStack().undo();require(graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()==old,"Drag undo failed");
        scene->undoStack().redo();require(graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()==dragged,"Drag redo failed");
        const auto before_arrange=scene->undoStack().count();
        QTest::mouseClick(editor->findChild<QPushButton *>("arrangePipeline"),Qt::LeftButton);
        require(scene->undoStack().count()==before_arrange+1,"Arrange not a single undo operation");
        scene->undoStack().undo(); require(graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()==dragged,"Arrange undo lost position");
        QTest::qWait(450); require(app.pipeline().graph_revision()==revision,"Layout changed processing revision");
        QFile file(path); require(file.open(QIODevice::ReadOnly),"Cannot read fixture");
        require(QJsonDocument::fromJson(file.readAll()).object().value("scene_graph").toObject()==initial_graph,"Autosave disabled but draft persisted");file.close();
        graph.setNodeData(0,QtNodes::NodeRole::Position,QPointF(42,86));
        window->findChild<QAction *>("saveSettings")->trigger();
        require(file.open(QIODevice::ReadOnly),"Cannot read explicit save");
        const auto manual=QJsonDocument::fromJson(file.readAll()).object();file.close();
        require(!manual.value("scene_graph").toObject().value("layout_pending").toBool(),
                "Saved layout retained the initial-layout marker");
        bool position_saved=false;
        for (const auto node : manual.value("scene_graph").toObject().value("nodes").toArray())
            if (node.toObject().value("id").toInt()==0) position_saved=node.toObject().value("position").toObject().value("x").toDouble()==42;
        require(position_saved,"Manual save did not flush pending position");
        const auto source_position = graph.nodeData(0,QtNodes::NodeRole::Position).toPointF();
        const auto connections_before = graph.save().value("connections").toArray().size();
        const auto undo_before = scene->undoStack().index();
        auto *test_source_action=window->findChild<QAction *>("useTestSource");
        require(test_source_action,"Status-bar generator preset action missing");
        test_source_action->trigger();
        auto *generator = graph.delegateModel<AudioNodeModel>(0);
        require(generator && generator->name()=="TestSignal", "Test source button did not replace the input node");
        require(generator->embeddedWidget()->findChild<QDoubleSpinBox *>("tones")->value()==8 &&
            generator->embeddedWidget()->findChild<QDoubleSpinBox *>("period_seconds"), "Generator controls missing");
        require(graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()==source_position &&
            graph.save().value("connections").toArray().size()==connections_before, "Input replacement lost position or routes");
        QString signal_error; auto signal_graph = graph.save(); signal_graph["version"] = 1;
        require(bool(pipeline::GraphProgram::compile(signal_graph,signal_error)), "Generator preset produced an invalid graph");
        require(scene->undoStack().index()==undo_before+1, "Generator preset is not one undo operation");
        scene->undoStack().undo();
        require(graph.delegateModel<AudioNodeModel>(0)->name()=="AudioSource", "Generator preset undo lost the original input");
        scene->undoStack().redo(); require(graph.delegateModel<AudioNodeModel>(0)->name()=="TestSignal", "Generator preset redo failed");
        scene->undoStack().undo();
        const auto preset_revision=app.pipeline().graph_revision();
        require(editor->save_preset(QString::fromUtf8("Проверка")),"Cannot save custom preset");
        const auto preset=app._ctx.file_configs.ui.pipeline_presets.last().toObject();
        const auto preset_id=preset.value("id").toString();
        const auto preset_position=graph.nodeData(0,QtNodes::NodeRole::Position).toPointF();
        graph.setNodeData(0,QtNodes::NodeRole::Position,QPointF(777,888));
        require(editor->load_preset(preset_id) && graph.nodeData(0,QtNodes::NodeRole::Position).toPointF()==preset_position,
            "Preset did not restore graph coordinates");
        QTest::qWait(400); require(app.pipeline().graph_revision()==preset_revision,"Same-processing preset recompiled DSP");
        require(editor->save_preset("To delete"),"Cannot save second preset");
        const auto delete_id=app._ctx.file_configs.ui.pipeline_presets.last().toObject().value("id").toString();
        require(editor->remove_preset(delete_id) && app._ctx.file_configs.ui.pipeline_presets.size()==1,"Preset delete failed");
        require(!editor->load_preset("missing"),"Missing preset accepted");
        window->findChild<QAction *>("saveSettings")->trigger();
        require(file.open(QIODevice::ReadOnly),"Cannot read presets");
        require(QJsonDocument::fromJson(file.readAll()).object().value("ui").toObject().value("pipeline_presets").toArray().size()==1,
            "Manual save did not persist custom presets"); file.close();
        QTest::mouseClick(editor->findChild<QPushButton *>("fitPipeline"),Qt::LeftButton);
        const auto bounds=scene->itemsBoundingRect();
        require(QLineF(view->mapToScene(view->viewport()->rect().center()),bounds.center()).length()<5/view->getScale(),
            "Center all blocks misses graph center");
        QUdpSocket udp;require(udp.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)),"UDP bind failed");
        app.pipeline().start_analyzer();app.pipeline().start_wled(QHostAddress::LocalHost,udp.localPort(),QHostAddress::LocalHost);
        const auto before_block=app.pipeline().processed_frames();
        std::jthread audio([&](std::stop_token stop) {
            pipeline::AudioFrame frame;frame.sample_rate=48000;frame.left.resize(240);frame.right.resize(240);
            uint64_t offset=0;auto deadline=std::chrono::steady_clock::now();
            while(!stop.stop_requested()) {
                for(size_t i=0;i<240;++i) {
                    const double t=double(offset+i)/48000;
                    double sample=0;
                    for(int tone=0;tone<8;++tone) {
                        const double carrier=73+tone*811;
                        sample+=std::sin(2*3.141592653589793*carrier*t+0.18*std::sin(t*(tone+1)))/8;
                    }
                    frame.left[i]=frame.right[i]=float(sample);
                }
                offset+=240;app.pipeline().feed(frame);deadline+=std::chrono::milliseconds(5);std::this_thread::sleep_until(deadline);
            }
        });
        std::this_thread::sleep_for(std::chrono::seconds(5));
        audio.request_stop();audio.join();
        require(app.pipeline().processed_frames()-before_block>270,"Blocked GUI stopped DSP");
        require(app.pipeline().history_count()>270 && app.pipeline().history_count()<=400,"Blocked GUI stopped/broke history");
        require(udp.hasPendingDatagrams(),"Blocked GUI stopped network output");
        app.pipeline().stop_wled();app.pipeline().stop_analyzer();
        fft->setFloating(true); QTest::qWait(30);
        fft->move(50000,50000);
        QMetaObject::invokeMethod(qApp,"screenRemoved",Qt::DirectConnection,Q_ARG(QScreen *,QGuiApplication::primaryScreen()));
        require(QGuiApplication::primaryScreen()->availableGeometry().intersects(fft->frameGeometry()),"Detached monitor left dock unreachable");
        flow->close();
        require(flow->isHidden() && !app._ctx.file_configs.ui.show_data_flow,"Dock close not reflected in preferences");
        window->findChild<QAction *>("openPipeline")->trigger();require(!flow->isHidden() && !flow->isFloating(),"Data Flow did not reopen attached");
        graph.setNodeData(0,QtNodes::NodeRole::Position,QPointF(1234,567));
        app._ctx.file_configs.ui.autosave_enabled=true;
        // Close before either debounce timer fires.
        window->close();
        require(file.open(QIODevice::ReadOnly),"Cannot read close flush");
        const auto saved=QJsonDocument::fromJson(file.readAll()).object();file.close();
        require(saved.value("extension").toObject().value("keep").toInt()==23,"Unknown config lost");
        const auto saved_ui = saved.value("ui").toObject();
        require(saved_ui.value("workspace").toObject().value("extension").toObject().value("keep").toInt()==24,
                "Unknown workspace settings lost");
        require(saved_ui.value("editor_view").toObject().value("extension").toObject().value("keep").toInt()==25,
                "Unknown editor settings lost");
        require(saved.value("ui").toObject().value("workspace").toObject().value("version").toInt()==1,"Workspace not saved on close");
        auto legacy_workspace=saved;
        window->findChild<QDockWidget *>("spectrogramDock")->setObjectName("surfaceDock");
        window->findChild<QDockWidget *>("windowSignalDock")->setObjectName("samplesDock");
        window->findChild<QDockWidget *>("windowedSignalDock")->setObjectName("windowedSamplesDock");
        auto legacy_ui=legacy_workspace.value("ui").toObject(); auto legacy_layout=legacy_ui.value("workspace").toObject();
        legacy_layout["layout_version"]=2;
        legacy_layout["state"]=QString::fromLatin1(window->saveState(1).toBase64());
        legacy_ui["workspace"]=legacy_layout; legacy_workspace["ui"]=legacy_ui;
        window->findChild<QDockWidget *>("surfaceDock")->setObjectName("spectrogramDock");
        window->findChild<QDockWidget *>("samplesDock")->setObjectName("windowSignalDock");
        window->findChild<QDockWidget *>("windowedSamplesDock")->setObjectName("windowedSignalDock");
        const auto legacy_path=directory.filePath("legacy-workspace.json");
        QFile legacy_file(legacy_path); require(legacy_file.open(QIODevice::WriteOnly),"Cannot save old dock layout");
        legacy_file.write(QJsonDocument(legacy_workspace).toJson()); legacy_file.close();
        QProcess legacy_child; legacy_child.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
        legacy_child.start(QCoreApplication::applicationFilePath(),{legacy_path});
        require(legacy_child.waitForStarted(3000) && legacy_child.waitForFinished(10000) && legacy_child.exitCode()==0,
            "Legacy dock names failed to restore");
        QProcess child;child.start(QCoreApplication::applicationFilePath(),{path});
        require(child.waitForStarted(3000) && child.waitForFinished(10000),"Restart verification did not finish");
        require(child.exitCode()==0 && child.exitStatus()==QProcess::NormalExit,child.readAllStandardError().constData());
        qInfo("Dock layout, drag/undo/redo, close flush and restart checks passed");
    } catch(const std::exception &error) { qCritical("Workspace regression: %s",error.what());return 1; }
}
