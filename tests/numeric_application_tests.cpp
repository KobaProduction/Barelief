#include "isolated_gui_platform.h"
#include <desktop/numeric_source_format.h>
#include "synthetic_capture.h"
#include "test_assertions.h"
#include "numeric_application_oracle.h"
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
#include <application/graph_lifecycle.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/node_parameter_model.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/example_presentation.h>
#include <barelief/ui_text.h>
#include <barelief/numeric_piano_view.h>
#include <core_app/graph_panel.h>
#include <barelief/abstract/abs.h>
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
#include <utility>
#include <functional>
#include <type_traits>
#include <initializer_list>
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

static WorkspaceDock* fixture_primary_dock(MainWindow* window,int node) {
    if(!window)return nullptr;
    for(const auto docks=window->workspaceDocks();auto* dock:docks)
        if(dock->property("graphNode").isValid() && dock->property("graphNode").toInt()==node && dock->property("viewInstance").toInt()==0)return dock;
    return nullptr;
}
static bool startup_primary_views_open(MainWindow* window) {
    for(int node:{8,1009,1011}) {
        auto* dock=fixture_primary_dock(window,node);
        if(!dock || dock->isClosed())return false;
    }
    return true;
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

#define CHECK(x) if(test_assertions::verify(bool(x),#x,__FILE__,__LINE__)) {} else return 1
// Answer only this test process's explicit overwrite warning.
static bool respond_to_overwrite(bool accept,const std::function<void()>& save,const QString& expectedTarget={}) {
    QTimer timer;timer.setInterval(5);bool seen=false,defaults=false;
    QObject::connect(&timer,&QTimer::timeout,&timer,[&]{
        for(const auto items_1=QApplication::topLevelWidgets();auto* widget:items_1)if(auto* dialog=qobject_cast<QMessageBox*>(widget);dialog && dialog->objectName()=="confirmConfigurationOverwrite"){
            seen=true;defaults=dialog->defaultButton()==dialog->button(QMessageBox::Cancel) && dialog->escapeButton()==dialog->button(QMessageBox::Cancel) && dialog->icon()==QMessageBox::Warning && dialog->testOption(QMessageBox::Option::DontUseNativeDialog) && (expectedTarget.isEmpty() || dialog->text().contains(expectedTarget));timer.stop();
            auto* button=dialog->button(QMessageBox::Cancel);
            if(accept)for(const auto buttons=dialog->buttons();auto* candidate:buttons)if(dialog->buttonRole(candidate)==QMessageBox::AcceptRole)button=candidate;
            button->click();return;
        }
    });
    timer.start();save();return seen && defaults;
}
static bool save_without_overwrite(const std::function<void()>& save) {
    QTimer timer;timer.setInterval(5);bool seen=false;
    QObject::connect(&timer,&QTimer::timeout,&timer,[&]{
        for(const auto items_2=QApplication::topLevelWidgets();auto* widget:items_2)if(auto* dialog=qobject_cast<QMessageBox*>(widget);dialog && dialog->objectName()=="confirmConfigurationOverwrite"){seen=true;dialog->reject();}
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
static bool wait_for_saved(MainWindow* window) {
    return wait_until([&]{auto* status=window->findChild<QLabel*>("configurationSaveStatus");
        return status && status->property("saveState").toInt()==int(MainWindow::ConfigurationSaveState::Saved);});
}
static bool separated_layout(MainWindow* window,NumericEditorGraphModel* model) {
    auto* editor=window->findChild<SchemeEditorWidget*>();if(!editor)return false;
    auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();if(!scene)return false;
    std::vector<QRectF> rectangles;
    for(const auto items_3=model->allNodeIds();auto id:items_3) {
        auto* item=scene->nodeGraphicsObject(id);if(!item)return false;
        const auto bounds=item->sceneBoundingRect();if(bounds.isEmpty())return false;
        for(const auto& previous:rectangles)if(bounds.intersects(previous))return false;
        rectangles.push_back(bounds);
    }
    return !rectangles.empty();
}
static int file_copy_save(auto& path,auto& original,auto& originalBytes,MainWindow*& window,SchemeEditorWidget*& editor) {
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

static int fft_copy_measurements(auto& original,auto& originalBytes,CoreApplication& app,MainWindow*& window) {

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

static int file_copy_fft(auto& argv,auto& path,auto& original,auto& originalBytes,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor) {
        if(QByteArray(argv[1])=="--fft-file-copy"){return fft_copy_measurements(original,originalBytes,app,window);}
        return file_copy_save(path,original,originalBytes,window,editor);
}

static int resampler_downstream(auto& original,auto& originalBytes,CoreApplication& app,NumericEditorGraphModel*& model,auto& tap,auto& mutex,auto& packets,auto& inputFrames,auto& inputGaps,auto& windowFrames,auto& windowGaps,auto& windowPrefix,auto& matched) {
            const auto before=app.presenter().client().snapshot().graph_revision;
            for(const auto id:{6,1009}){auto* node=model->delegateModel<AudioNodeModel>(id);CHECK(node && node->parameter_model()->setBypassed(true));}
            CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>before && !app.presenter().graph_request_pending();},8000));
            {std::lock_guard lock(mutex);packets.clear();inputFrames=inputGaps=windowFrames=windowGaps=0;windowPrefix=4096;}
            CHECK(wait_until([&]{std::lock_guard lock(mutex);return packets.size()>=64 && windowFrames>=16;},8000));
            {std::lock_guard lock(mutex);std::fprintf(stderr,"DOWNSTREAM_BYPASS inputFrames=%u inputGaps=%u windowFrames=%u windowGaps=%u prefix=%zu\n",inputFrames,inputGaps,windowFrames,windowGaps,windowPrefix);}
            app.pipeline().unsubscribe_numeric_tap(tap);CHECK(matched);CHECK(read(original)==originalBytes);
            std::puts("resampler/user chart raw frame comparison PASS");return 0;
        }

static void report_resampler_packets(auto& mutex,auto& packets,auto& inputFrames,auto& inputGaps,auto& points,auto& gaps,auto& matched) {
std::lock_guard lock(mutex);for(const auto& packet:packets){gaps+=packet.stamp.discontinuity;
                bool equal=points.size()==packet.values.size();for(size_t i=0;equal && i<packet.values.size();++i)equal=std::abs(points[int(i)].y()-packet.values[i])<1e-9;
                matched|=equal;
            }
            const auto& p=packets.back();size_t prefix=0;while(prefix<p.values.size() && std::abs(p.values[prefix])<1e-6)++prefix;
            std::fprintf(stderr,"RESAMPLER_PROBE inputFrames=%u inputGaps=%u outputFrames=%zu outputGaps=%u prefix=%zu points=%lld matched=%d firstTick=%lld\n",inputFrames,inputGaps,packets.size(),gaps,prefix,static_cast<long long>(points.size()),int(matched),static_cast<long long>(p.stamp.timestamp_ticks));
            for(size_t i=packets.size()>8?packets.size()-8:0;i<packets.size();++i){const auto& packet=packets[i];size_t zeros=0;while(zeros<packet.values.size()&&std::abs(packet.values[zeros])<1e-6)++zeros;
                std::fprintf(stderr,"RESAMPLER_PACKET size=%zu fs=%u prefix=%zu gap=%d seq=%llu src=%llu gen=%llu revision=%llu\n",packet.values.size(),packet.rate,zeros,int(packet.stamp.discontinuity),(unsigned long long)packet.stamp.sequence,(unsigned long long)packet.stamp.source_sequence,(unsigned long long)packet.stamp.generation,(unsigned long long)packet.stamp.graph_revision);
            }}

static int resampler_chart(auto& original,auto& originalBytes,CoreApplication& app,MainWindow*& window,NumericEditorGraphModel*& model,auto& tap,auto& mutex,auto& packets,auto& inputFrames,auto& inputGaps,auto& windowFrames,auto& windowGaps,auto& windowPrefix) {
            auto* dock=window->workspaceDock("rawSamplesDock");CHECK(dock);dock->toggleView(true);dock->raise();
            auto* panel=qobject_cast<GraphPanel*>(dock->widget());CHECK(panel);auto* chart=panel->active_chart();CHECK(chart);chart->resize(1200,400);
            CHECK(wait_until([&]{std::lock_guard lock(mutex);return packets.size()>=32 && chart->projected_point_count()>100;},8000));
            bool matched=false;unsigned gaps=0;const auto points=chart->projected_points();
            report_resampler_packets(mutex,packets,inputFrames,inputGaps,points,gaps,matched);
            std::fprintf(stderr,"WINDOW_PROBE frames=%u gaps=%u prefix=%zu\n",windowFrames,windowGaps,windowPrefix);
            for(const auto items_26=app.pipeline().snapshot().diagnostics["node_stats"].toArray();const auto item:items_26){
                const auto stats=item.toObject();if(stats["errors"].toVariant().toULongLong())std::fprintf(stderr,"NODE_ERRORS %s\n",QJsonDocument(stats).toJson(QJsonDocument::Compact).constData());
            }
        return resampler_downstream(original,originalBytes,app,model,tap,mutex,packets,inputFrames,inputGaps,windowFrames,windowGaps,windowPrefix,matched);
}

static void observe_resampler_packet(auto& frame,auto& mutex,auto& packets,auto& inputFrames,auto& inputGaps,auto& windowFrames,auto& windowGaps,auto& windowPrefix) {

                if(!frame.context || !frame.frame)return;const auto view=frame.frame.view();if(!view.stamp.available)return;
                const auto* values=std::get_if<signal_contracts::NumericView>(&view.payload);if(!values)return;
                std::lock_guard lock(mutex);
                if(frame.context->endpoint.node==200){++inputFrames;inputGaps+=view.stamp.discontinuity;return;}
                if(frame.context->endpoint.node==2){++windowFrames;windowGaps+=view.stamp.discontinuity;windowPrefix=0;
                    for(size_t i=0;i<values->actual[0];++i){float value=0;signal_contracts::load_element(*values,{0,i,0},value);if(std::abs(value)>1e-6)break;++windowPrefix;}return;}
                typename std::remove_reference_t<decltype(packets)>::value_type packet;packet.stamp=view.stamp;packet.rate=values->contract->clock.sample_rate;packet.values.resize(values->actual[0]);
                for(size_t i=0;i<packet.values.size();++i)signal_contracts::load_element(*values,{0,i,0},packet.values[i]);
                if(packets.size()==64)packets.erase(packets.begin());packets.push_back(std::move(packet));
            }

static int file_copy_resampler(auto& argv,auto& path,auto& original,auto& originalBytes,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor) {
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
            const auto tap=app.pipeline().subscribe_numeric_tap({{"main",200,domain::PortDirection::Output,0},{"main",332,domain::PortDirection::Output,0},{"main",2,domain::PortDirection::Output,0}},[&](const pipeline::DeliveredNumericFrame& frame){ return observe_resampler_packet(frame,mutex,packets,inputFrames,inputGaps,windowFrames,windowGaps,windowPrefix); },{});CHECK(tap);
        return resampler_chart(original,originalBytes,app,window,model,tap,mutex,packets,inputFrames,inputGaps,windowFrames,windowGaps,windowPrefix);
        }
        return file_copy_fft(argv,path,original,originalBytes,app,window,editor);
}

static void observe_music_file(auto& frame,auto& failures,auto& history_id,auto& note_id,auto& rows,auto& origins) {

                if(!frame.context)return;
                if(frame.status!=signal_contracts::Status::ok && frame.status!=signal_contracts::Status::unavailable){
                    if(failures.fetch_add(1)<8)std::fprintf(stderr,"MUSIC_FAILURE node=%d port=%u status=%u\n",int(frame.context->endpoint.node),unsigned(frame.context->endpoint.port),unsigned(frame.status));
                    return;
                }
                if(!frame.frame || !frame.frame.view().stamp.available)return;
                if(frame.context->endpoint.node==history_id)++rows;
                if(frame.context->endpoint.node==note_id && frame.context->endpoint.port==3)++origins;
            }

static int music_copy_measurements(auto& path,auto& original,auto& originalBytes,CoreApplication& app,MainWindow*& window) {

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
            const auto tap=app.pipeline().subscribe_numeric_tap({{"main",note_id,domain::PortDirection::Output,0},{"main",note_id,domain::PortDirection::Output,1},{"main",note_id,domain::PortDirection::Output,2},{"main",note_id,domain::PortDirection::Output,3},{"main",history_id,domain::PortDirection::Output,0}},[&](const pipeline::DeliveredNumericFrame& frame){ return observe_music_file(frame,failures,history_id,note_id,rows,origins); },{});CHECK(tap);
            const bool ready=wait_until([&]{return rows>=8 || failures>=8;},8000);
            app.pipeline().unsubscribe_numeric_tap(tap);
            std::fprintf(stderr,"MUSIC_PROBE ready=%d combined_rows=%u note_origins=%u failures=%u\n",int(ready),rows.load(),origins.load(),failures.load());
            CHECK(read(original)==originalBytes);
            CHECK(ready && rows>=8 && origins>=8 && failures==0);
            std::puts("user music copy: notes, explicit MIDI origin and joined rows PASS");return 0;
        }

static int file_copy_modes(auto& argv,auto& path,auto& original,auto& originalBytes,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor) {
        if(QByteArray(argv[1])=="--music-file-copy"){return music_copy_measurements(path,original,originalBytes,app,window);}
        return file_copy_resampler(argv,path,original,originalBytes,app,window,editor);
}

static int scenario_file_copy(char** argv,const QString& path) {
        CHECK(test_fixture::isolated_gui_platform());
        const auto original=QString::fromLocal8Bit(argv[2]);const auto originalBytes=read(original);
        CHECK(!originalBytes.isEmpty());QFile copy(path);CHECK(copy.open(QIODevice::WriteOnly));CHECK(copy.write(originalBytes)==originalBytes.size());copy.close();
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        MainWindow* window=nullptr;for(const auto items_4=QApplication::topLevelWidgets();auto* widget:items_4)if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;CHECK(window);
        auto* editor=window->findChild<SchemeEditorWidget*>();
        std::fprintf(stderr,"SAVE_PROBE editor_in_main_window=%d\n",int(editor!=nullptr));
        if(!editor)for(const auto items_5=QApplication::allWidgets();auto* widget:items_5)if(auto* candidate=qobject_cast<SchemeEditorWidget*>(widget))editor=candidate;
        CHECK(editor);
        return file_copy_modes(argv,path,original,originalBytes,app,window,editor);
}

static bool workspace_acceptance_child(const QString& phase,const QString& source){QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--workspace-phase",phase,source});if(!child.waitForFinished(40000)){child.kill();child.waitForFinished();std::fprintf(stderr,"workspace phase %s timeout: %s\n",qPrintable(phase),child.readAllStandardError().constData());return false;}if(child.exitCode()!=0)std::fprintf(stderr,"workspace phase %s: %s\n",qPrintable(phase),child.readAllStandardError().constData());return child.exitStatus()==QProcess::NormalExit && child.exitCode()==0;}
static int workspace_acceptance_ini(auto& next){
ConfigManager manager(next);FileSaveApplicationConfig recovered;CHECK(manager.loadWorkspaceRecovery(recovered));
        {QSettings settings(next+".workspace.ini",QSettings::IniFormat);settings.setValue("extension","preserved");settings.sync();}
        CHECK(manager.saveWorkspaceRecovery(recovered));
        {QSettings settings(next+".workspace.ini",QSettings::IniFormat);CHECK(settings.value("extension").toString()=="preserved");settings.setValue("version",2);settings.sync();}
        const auto future=read(next+".workspace.ini");CHECK(!manager.loadWorkspaceRecovery(recovered) && !manager.saveWorkspaceRecovery(recovered) && read(next+".workspace.ini")==future);
        {QSettings settings(next+".workspace.ini",QSettings::IniFormat);settings.setValue("version",1);settings.sync();}
        QFile changed(next);CHECK(changed.open(QIODevice::Append));changed.write(" ");changed.close();CHECK(!manager.loadWorkspaceRecovery(recovered));
        std::puts("workspace closed/deleted/restart/tabbed placement and protected project recovery PASS");return 0;


}
static int workspace_acceptance_recovery(auto& next,auto& run){
const QByteArray rejected=R"({"schema_version":4,"scene_graph":{"version":5,"processing_version":4,"nodes":"invalid","connections":[]}})";
        QFile broken(next);CHECK(broken.open(QIODevice::WriteOnly));broken.write(rejected);broken.close();
        CHECK(run("invalid-save",next));CHECK(read(next)==rejected && QFileInfo::exists(next+".workspace.ini"));
        if(!run("invalid-load",next)){
            const auto evidence=QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../.agent-workspace/workspace-invalid-repro.json");QFile::copy(next,evidence);QFile::copy(next+".workspace.ini",evidence+".workspace.ini");std::fprintf(stderr,"workspace recovery repro: %s\n",qPrintable(evidence));return 1;
        }CHECK(read(next)==rejected);
        return workspace_acceptance_ini(next);
}
static int scenario_workspace_persistence(int argc,char** argv,const QString& path,const QString& next) {
        CHECK(test_fixture::isolated_gui_platform());QCoreApplication controller(argc,argv);
        auto graph=native_mono_example(numeric_startup_graph());graph.execution_interval={1,10};
        // Keep the complete, coherently derived startup analysis fixture.
        domain::NumericProcessingGraph evaluated,executable;CHECK(domain::resolve_numeric_parameter_bindings(graph,numeric_editor::limits(),evaluated,executable,2));graph=std::move(evaluated);
        auto removed=*std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==8;});removed.id=18;graph.nodes.push_back(removed);graph.edges.push_back({7,18,0,0});
        desktop::NumericProjectDocument document;CHECK(desktop::create_numeric_startup_project(graph,document));CHECK(desktop::save_numeric_project(document,path));
        const auto run=workspace_acceptance_child;
        CHECK(run("save",path));
        auto saved=QJsonDocument::fromJson(read(path)).object();auto ui=saved["ui"].toObject(),workspace=ui["workspace"].toObject();workspace.remove("graph_navigation_version");ui["workspace"]=workspace;saved["ui"]=ui;
        QFile output(path);CHECK(output.open(QIODevice::WriteOnly));output.write(QJsonDocument(saved).toJson());output.close();
        CHECK(run("load",path));
        return workspace_acceptance_recovery(next,run);
}

static int workspace_acceptance_invalid_load(CoreApplication& app,MainWindow*& window){

            CHECK(!app.presenter().file_protection_warning().isEmpty());
            const auto restoredDocks=window->workspaceDocks();CHECK(std::none_of(restoredDocks.begin(),restoredDocks.end(),[](auto* dock){return dock->property("viewInstance").toInt()>0;}));
            auto* fftDock=fixture_primary_dock(window,10);auto* spectrogramDock=fixture_primary_dock(window,8);
            CHECK((!fftDock || fftDock->isClosed()) && (!spectrogramDock || spectrogramDock->isClosed()));
            for(const auto items_8=QApplication::topLevelWidgets();auto* widget:items_8)CHECK(!widget->inherits("ads::CFloatingDockContainer") || !widget->isVisible());window->close();return 0;

}
static int workspace_acceptance_import_recovery(CoreApplication& app,MainWindow*& window,NumericEditorGraphModel*& model){
CHECK(!app.presenter().file_protection_warning().isEmpty());auto* example=window->findChild<QAction*>("example-startup");CHECK(example);example->trigger();auto* buffer=model->delegateModel<AudioNodeModel>(2);CHECK(buffer && buffer->parameter_model()->editLiteral("size","512",0));
return 0;
}
static int workspace_acceptance_loaded_windows(MainWindow*& window,auto& dockFor,WorkspaceDock*& flow){
for(const auto items_9=QApplication::topLevelWidgets();auto* widget:items_9)CHECK(!widget->inherits("ads::CFloatingDockContainer") || !widget->isVisible());
            CHECK(dockFor(11,true)->dockAreaWidget()==dockFor(8)->dockAreaWidget() && flow->dockAreaWidget()!=dockFor(8)->dockAreaWidget());window->close();return 0;


}
static int workspace_acceptance_validate_loaded(MainWindow*& window,NumericEditorGraphModel*& model,auto& dockFor,WorkspaceDock*& flow){

            CHECK(dockFor(10) && dockFor(10)->isClosed());CHECK(!model->allNodeIds().contains(9));CHECK(!dockFor(999));
            CHECK(dockFor(11,true) && !dockFor(11,true)->isClosed() && !dockFor(11,true)->isFloating() && dockFor(11,true)->sharesHost(flow));
            CHECK(dockFor(12,true) && dockFor(12,true)->isClosed());CHECK(!model->allNodeIds().contains(18) && !dockFor(18) && !dockFor(18,true));
            return workspace_acceptance_loaded_windows(window,dockFor,flow);
}
static int workspace_acceptance_persist_views(CoreApplication& app,auto& phase,auto& source,MainWindow*& window,SchemeEditorWidget*& editor,NumericEditorGraphModel*& model,auto& dockFor,auto& closedKey){
emit editor->graph_open_requested(18,0);CHECK(wait_until([&]{return dockFor(18,true) && dockFor(18,true)->isFloating();}));
        std::fprintf(stderr,"WORKSPACE %s before delete\n",phase.constData());CHECK(model->deleteNode(18));CHECK(model->deleteNode(9));std::fprintf(stderr,"WORKSPACE %s deleted\n",phase.constData());
        auto preferences=app._ctx.file_configs.ui.graph_views.value(closedKey).toObject();preferences["node"]=999;preferences["open"]=true;
        auto identity=QJsonDocument::fromJson(QByteArray::fromHex(closedKey.toLatin1())).array();identity[2]=999;identity[3]=999;
        const auto ghost=QString::fromLatin1(QJsonDocument(identity).toJson(QJsonDocument::Compact).toHex());preferences["instance"]=999;app._ctx.file_configs.ui.graph_views[ghost]=preferences;
        CHECK(wait_until([&]{const auto current=QJsonDocument::fromJson(read(source)).object();return current["ui"].toObject()["graph_views"].toObject()[closedKey].toObject()["open"].isBool() && !current["ui"].toObject()["graph_views"].toObject()[closedKey].toObject()["open"].toBool();},6000));
        std::fprintf(stderr,"WORKSPACE %s before close\n",phase.constData());window->close();std::fprintf(stderr,"WORKSPACE %s closed\n",phase.constData());return 0;




}
static int workspace_acceptance_secondary_views(CoreApplication& app,auto& phase,auto& source,MainWindow*& window,SchemeEditorWidget*& editor,NumericEditorGraphModel*& model,auto& dockFor,auto& invalid){
emit editor->graph_open_requested(12,0);CHECK(wait_until([&]{return dockFor(12,true) && dockFor(12,true)->isFloating();}));
        std::fprintf(stderr,"WORKSPACE %s opened\n",phase.constData());
        const auto closedKey=dockFor(12,true)->property("graphViewKey").toString();dockFor(12,true)->close();dockFor(10)->close();
        if(invalid){dockFor(8)->close();window->close();CHECK(wait_until([&]{return QFileInfo::exists(source+".workspace.ini");}));return 0;}
        return workspace_acceptance_persist_views(app,phase,source,window,editor,model,dockFor,closedKey);

}
static int workspace_acceptance_open_views(CoreApplication& app,auto& phase,auto& source,MainWindow*& window,SchemeEditorWidget*& editor,NumericEditorGraphModel*& model,auto& dockFor,auto& invalid,WorkspaceDock*& flow){
for(const auto items_10=window->workspaceDocks();auto* dock:items_10)CHECK(dock->isClosed() || !dock->isFloating());
        CHECK(dockFor(11) && dockFor(11)->isClosed());CHECK(dockFor(12) && dockFor(12)->isClosed());
        auto* anchor=dockFor(8);CHECK(anchor);
        anchor->toggleView(true);anchor->raise();QCoreApplication::processEvents();
        QFocusEvent focus(QEvent::FocusIn);QApplication::sendEvent(anchor,&focus);
        emit editor->graph_open_requested(11,1);CHECK(wait_until([&]{return dockFor(11,true) && !dockFor(11,true)->isClosed();}));
        CHECK(!dockFor(11,true)->isFloating() && dockFor(11,true)->sharesHost(flow) && dockFor(11,true)->dockAreaWidget()==dockFor(8)->dockAreaWidget());
        return workspace_acceptance_secondary_views(app,phase,source,window,editor,model,dockFor,invalid);
}
static int workspace_acceptance_ready_phase(CoreApplication& app,auto& phase,auto& source,MainWindow*& window,SchemeEditorWidget*& editor,NumericEditorGraphModel*& model,auto& dockFor,auto& invalid,WorkspaceDock*& flow){
if(phase=="load"){return workspace_acceptance_validate_loaded(window,model,dockFor,flow);}
        return workspace_acceptance_open_views(app,phase,source,window,editor,model,dockFor,invalid,flow);
}
static int workspace_acceptance_running_phase(CoreApplication& app,auto& phase,auto& source,MainWindow*& window,SchemeEditorWidget*& editor,NumericEditorGraphModel*& model,auto& dockFor,auto& invalid){
if(invalid){if(const int failure=workspace_acceptance_import_recovery(app,window,model);failure)return failure;}
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending() && dockFor(8);},8000));
        std::fprintf(stderr,"WORKSPACE %s ready\n",phase.constData());
        auto* flow=window->findChild<WorkspaceDock*>("dataFlowDock");CHECK(flow);
        return workspace_acceptance_ready_phase(app,phase,source,window,editor,model,dockFor,invalid,flow);
}
static int scenario_workspace_phase(char** argv) {
        CHECK(test_fixture::isolated_gui_platform());const QByteArray phase=argv[2];const QString source=QString::fromLocal8Bit(argv[3]);
        auto encoded=source.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());install_synthetic_capture_discovery(app);std::fprintf(stderr,"WORKSPACE %s constructed\n",phase.constData());
        MainWindow* window=nullptr;for(const auto items_6=QApplication::topLevelWidgets();auto* widget:items_6)if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;CHECK(window);window->show();
        auto* editor=window->findChild<SchemeEditorWidget*>();CHECK(editor);auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        const auto dockFor=[&](int id,bool instance=false)->WorkspaceDock*{for(const auto items_7=window->workspaceDocks();auto* dock:items_7)if(dock->property("graphNode").isValid() && dock->property("graphNode").toInt()==id && (dock->property("viewInstance").toInt()>0)==instance)return dock;return nullptr;};
        const bool invalid=phase.startsWith("invalid");
        if(phase=="invalid-load"){return workspace_acceptance_invalid_load(app,window);}
        return workspace_acceptance_running_phase(app,phase,source,window,editor,model,dockFor,invalid);
}

static int matrix_bypass_add_transform(auto& graph,auto& history,auto& history_data,auto& id){

            domain::ConfiguredProcessingNode node;node.id=id;node.kind=id==23?"MatrixCenter":"MatrixWindowFunction";
            node.enabled=false;node.parameters=domain::TransformProcessingParameters{};
            domain::OwnedProcessingContract output;
            const auto& input=std::get<signal_contracts::NumericContract>(history_data);
            CHECK((id==23?domain::matrix_center_output_contract(input,output):domain::matrix_window_output_contract(input,output))==signal_contracts::Status::ok);
            node.inputs={{0,domain::PortDirection::Input,history}};node.outputs={{0,domain::PortDirection::Output,output}};
            graph.nodes.push_back(std::move(node));graph.edges.push_back({7,id,0,0});

return 0;
}
static int matrix_bypass_validate_frame(CoreApplication& app,auto& id){

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

return 0;
}
static int matrix_bypass_observe(CoreApplication& app,auto& path,auto& original){
const auto tap=app.pipeline().subscribe_numeric_tap({{"main",23,domain::PortDirection::Output,0},{"main",22,domain::PortDirection::Output,0}},[](const pipeline::DeliveredNumericFrame&){},{});
        CHECK(tap);
        struct Guard{pipeline::NumericEngine& engine;std::uint64_t tap;~Guard(){engine.unsubscribe_numeric_tap(tap);}} guard{app.pipeline(),tap};
        QObject::connect(&app.presenter(),&GuiPresenter::graph_apply_finished,&app,[](quint64,bool ok,const QString& error){if(!ok)std::fprintf(stderr,"startup rejected: %s\n",qPrintable(error));});
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();},6000));
        pipeline::DeliveredNumericFrame history_frame;std::uint64_t history_ordinal=0;
        CHECK(wait_until([&]{return app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history_frame,&history_ordinal)==signal_contracts::Status::ok&&history_ordinal>=10;},6000));
        for(int id:{23,22}){if(const int failure=matrix_bypass_validate_frame(app,id);failure)return failure;}
        CHECK(read(path)==original);
        std::puts("Direct History ring -> bypassed MatrixCenter/MatrixWindow: ordinary matrices delivered and retained, synthetic/offscreen PASS");return 0;

}
static int scenario_matrix_bypass(const QString& path) {
        CHECK(test_fixture::isolated_gui_platform());
        auto graph=numeric_generator_graph();
        const auto retained=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==7;});
        CHECK(retained!=graph.nodes.end());const auto history=retained->outputs[0].contract;
        const auto history_data=history.view();
        for(int id:{23,22}){if(const int failure=matrix_bypass_add_transform(graph,history,history_data,id);failure)return failure;}
        desktop::NumericDraftPresentation display;
        display.application_settings={{"ui",QJsonObject{{"autosave_enabled",false}}}};
        desktop::NumericProjectDocument project;CHECK(desktop::create_numeric_project(graph,display,project));
        CHECK(desktop::save_numeric_project(project,path));const auto original=read(path);
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]{executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());install_synthetic_capture_discovery(app);
        return matrix_bypass_observe(app,path,original);
}

static int scenario_export_temporal(char** argv,const QString& path) {
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
        MainWindow* window=nullptr;for(const auto items_11=QApplication::topLevelWidgets();auto* candidate:items_11)if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        CHECK(wait_until([&]{return separated_layout(window,model)&&app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();},6000));
        app.save_configuration_as(destination);
        desktop::NumericProjectDocument exported;domain::NumericProcessingGraph loaded;desktop::NumericDraftPresentation saved_display;
        CHECK(desktop::load_numeric_project(destination,exported,loaded)&&desktop::read_numeric_presentation(exported,saved_display));
        CHECK(saved_display.positions.size()==int(loaded.nodes.size()));
        std::puts("Matrix transpose/window/row FFT example exported with measured non-overlapping layout; existing files untouched");return 0;
    }

static void report_temporal_phase(const char* phase,CoreApplication& app,int detail=-1) {
    std::fprintf(stderr,"Temporal phase=%s detail=%d revision=%llu pending=%d\n",phase,detail,
        static_cast<unsigned long long>(app.presenter().client().snapshot().graph_revision),int(app.presenter().graph_request_pending()));
    std::fflush(stderr);
}

static int temporal_saved_selection(CoreApplication& app,auto& step,auto& saved,auto& saved_graph,auto& custom_names) {
const auto saved_selector=std::find_if(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.id==200;});
        CHECK(saved_selector!=saved_graph.nodes.end());
        const auto& selector_values=std::get<domain::TransformProcessingParameters>(saved_selector->parameters).values;
        const auto saved_choice=std::find_if(selector_values.begin(),selector_values.end(),[](const auto& field){return field.key=="selected_input";});
        CHECK(saved_choice!=selector_values.end()&&std::visit([](auto value){return value==1;},std::get<signal_contracts::NumericLiteral>(saved_choice->value)));
        desktop::NumericDraftPresentation saved_display;CHECK(desktop::read_numeric_presentation(saved,saved_display));
        CHECK(saved_display.node_presentations["200"].toObject()["stream_names"].toArray()==custom_names);
        bool center_stats_seen=false;
        for(const auto items_27=app.pipeline().snapshot().diagnostics["node_stats"].toArray();const auto item:items_27){
            const auto stats=item.toObject();if(stats["node"].toInt()!=23)continue;
            center_stats_seen=true;
            CHECK(stats["calls"].toVariant().toULongLong()>0&&stats["errors"].toVariant().toULongLong()==0);
            std::printf("MatrixCenter diagnostics: calls=%s errors=%s max_self_ns=%s\n",qPrintable(stats["calls"].toVariant().toString()),qPrintable(stats["errors"].toVariant().toString()),qPrintable(stats["max_self_nanoseconds"].toVariant().toString()));
        }
        CHECK(center_stats_seen);
        std::printf("Temporal FFT application: mono generator/loopback switching without resampling, 8 silent changing tones, 50x256 matrix, cadence %.9f Hz/bin, selected first/last/middle bands, window on/off, History continuity, canonical SaveAs PASS\n",step);
        return 0;
    }

static int temporal_saved_nodes(CoreApplication& app,auto& step,auto& saved,auto& saved_graph,auto& custom_names) {
for(const auto& node:saved_graph.nodes)CHECK(node.kind!="PcmResampler"&&node.id!=14&&node.id!=332&&node.id!=364);
        CHECK(std::any_of(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.kind=="MatrixFFT";}));
        CHECK(std::any_of(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.kind=="MatrixTranspose";}));
        CHECK(std::any_of(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.kind=="MatrixWindowFunction";}));
        CHECK(std::any_of(saved_graph.nodes.begin(),saved_graph.nodes.end(),[](const auto& node){return node.kind=="MatrixCenter"&&node.id==23;}));
        return temporal_saved_selection(app,step,saved,saved_graph,custom_names);
}

static int temporal_save(auto& path,auto& next,auto& runtime_graph,auto& original,CoreApplication& app,auto& step,AudioNodeModel*& selector) {
    report_temporal_phase("save",app);
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
        return temporal_saved_nodes(app,step,saved,saved_graph,custom_names);
}

static int temporal_switched_matrix(CoreApplication& app,auto& matrix_frame,auto& history,auto& ordinal,auto& filled,auto& expected_bin_hz,auto& previous_ordinal) {
const auto switched=std::get<signal_contracts::NumericView>(matrix_frame.frame.view().payload);
            CHECK(switched.actual[0]==50&&switched.actual[1]==256);
            double switched_bin_hz=0;std::memcpy(&switched_bin_hz,switched.coordinates.axes[1].data()+sizeof(double),sizeof(double));
            CHECK(std::abs(switched_bin_hz-expected_bin_hz)<1e-6);
            for(std::size_t band=0;band<50;++band)for(std::size_t bin=0;bin<256;++bin) {
                float value=0;CHECK(signal_contracts::load_element(switched,{0,band,bin},value)==signal_contracts::Status::ok&&std::isfinite(value));
            }
            CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=previous_ordinal);

return 0;
}

static int temporal_pcm_stream(auto& stream,CoreApplication& app,auto& matrix_frame,auto& history,auto& ordinal,auto& filled,auto& temporal_rate,auto& loopback_energy,auto& expected_bin_hz,auto& pcm_frame,auto& previous_ordinal) {
const auto pcm=std::get<signal_contracts::NumericView>(pcm_frame.frame.view().payload);
            CHECK(pcm.contract->channel_order.size()==1&&pcm.contract->clock.sample_rate==temporal_rate);
            double pcm_energy=0;
            CHECK(numeric_application_oracle::accumulate_square(pcm,pcm_energy));
            CHECK(pcm.actual[0]>0);pcm_energy/=pcm.actual[0];CHECK(pcm_energy>0);
            if(stream==1)loopback_energy=pcm_energy;
            else CHECK(pcm_energy>loopback_energy*10);
            return temporal_switched_matrix(app,matrix_frame,history,ordinal,filled,expected_bin_hz,previous_ordinal);
}

static int temporal_selector_stream(auto& stream,CoreApplication& app,AudioNodeModel*& selector,auto& query,auto& matrix_frame,auto& selected_frame,auto& history,auto& ordinal,auto& filled,auto& temporal_rate,auto& loopback_energy,auto& expected_bin_hz) {
    report_temporal_phase("selector",app,int(stream));

            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok);
            const auto previous_ordinal=ordinal;
            CHECK(selector->select_input(domain::PortId(stream)));
            pipeline::DeliveredNumericFrame pcm_frame;
            const bool switched_ready=wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision
                &&query(17,matrix_frame)&&query(19,selected_frame)&&query(200,pcm_frame);},6000);
            if(!switched_ready)std::fprintf(stderr,"Temporal switch %d: %s\n",stream,QJsonDocument(app.pipeline().snapshot().diagnostics).toJson(QJsonDocument::Compact).constData());
            CHECK(switched_ready);
            return temporal_pcm_stream(stream,app,matrix_frame,history,ordinal,filled,temporal_rate,loopback_energy,expected_bin_hz,pcm_frame,previous_ordinal);
}

static int temporal_selector(auto& path,auto& next,auto& temporal_rate,auto& runtime_graph,auto& expected_bin_hz,auto& original,CoreApplication& app,NumericEditorGraphModel*& model,auto& query,auto& matrix_frame,auto& selected_frame,auto& step,auto& ordinal,auto& filled,auto& history) {
    report_temporal_phase("selector",app);
    query.set_required({17,19,200});
        auto* selector=model->delegateModel<AudioNodeModel>(200);CHECK(selector);
        CHECK(selector->numeric_node()->inputs.size()==4&&domain::data_outputs(*selector->numeric_node()).size()==2&&selector->numeric_node()->outputs.size()==2);
        for(const auto& node:model->numeric_graph().nodes)CHECK(node.kind!="PcmResampler"&&node.kind!="Mixer"&&node.id!=14&&node.id!=332&&node.id!=364);
        double loopback_energy=0;
        for(int stream:{1,0}) { if(const int failure=temporal_selector_stream(stream,app,selector,query,matrix_frame,selected_frame,history,ordinal,filled,temporal_rate,loopback_energy,expected_bin_hz);failure)return failure; }
        query.set_required({17});
        return temporal_save(path,next,runtime_graph,original,app,step,selector);
}

static domain::ConfiguredProcessingNode temporal_silent_generator(const AudioNodeModel& generator) {
    auto node=*generator.numeric_node();
    for(auto& oscillator:std::get<domain::SignalGeneratorProcessingParameters>(node.parameters).oscillators)oscillator.amplitude=0;
    return node;
}

static int temporal_silence(auto& path,auto& next,auto& temporal_rate,auto& runtime_graph,auto& expected_bin_hz,auto& original,CoreApplication& app,NumericEditorGraphModel*& model,auto& query,auto& matrix_frame,auto& selected_frame,auto& step,auto& ordinal,auto& filled,auto& history) {
    report_temporal_phase("silence",app);
    query.set_required({17,19});
        auto* generator=model->delegateModel<AudioNodeModel>(1);CHECK(generator);
        const auto original_generator=*generator->numeric_node();
        const auto pause_revision=app.presenter().client().snapshot().graph_revision;
        CHECK(model->edit_node(original_generator.id,temporal_silent_generator(*generator)));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>pause_revision&&query(17,matrix_frame);},6000));
        CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok);
        const auto pause_ordinal=ordinal;bool pause_unavailable=false;
        CHECK(wait_until([&]{
            pause_unavailable|=!query(17,matrix_frame)||!query(19,selected_frame);
            return app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=pause_ordinal+420;
        },7000));
        CHECK(!pause_unavailable&&query(17,matrix_frame));
        const auto silence=std::get<signal_contracts::NumericView>(matrix_frame.frame.view().payload);
        CHECK(numeric_application_oracle::zero_matrix(silence));
        CHECK(model->edit_node(original_generator.id,original_generator));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&query(17,matrix_frame);},6000));
        return temporal_selector(path,next,temporal_rate,runtime_graph,expected_bin_hz,original,app,model,query,matrix_frame,selected_frame,step,ordinal,filled,history);
}

static int temporal_center_method(CoreApplication& app,auto& query,auto& matrix_frame,AudioNodeModel*& center,auto& centered_frame,auto& method) {
    report_temporal_phase("center",app,int(method));

            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(center->parameter_model()->edit("method",method));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(23,centered_frame)&&query(17,matrix_frame);},6000));
            if(method==2){
                const auto offset_revision=app.presenter().client().snapshot().graph_revision;
                CHECK(center->parameter_model()->edit("offset",signal_contracts::NumericLiteral{1.25}));
                CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>offset_revision&&query(23,centered_frame);},6000));
            }

return 0;
}

static int temporal_center_changes(auto& path,auto& next,auto& temporal_rate,auto& runtime_graph,auto& expected_bin_hz,auto& original,CoreApplication& app,NumericEditorGraphModel*& model,auto& query,auto& matrix_frame,auto& selected_frame,auto& step,AudioNodeModel*& center,auto& ordinal,auto& filled,auto& history,auto& centered_frame) {
    report_temporal_phase("center edits",app);
    query.set_required({17,23});
        for(int method:{1,2,0}){if(const int failure=temporal_center_method(app,query,matrix_frame,center,centered_frame,method);failure)return failure;}
        for(bool bypass:{true,false}){
            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(center->parameter_model()->setBypassed(bypass));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(23,centered_frame)&&query(17,matrix_frame);},6000));
        }
        return temporal_silence(path,next,temporal_rate,runtime_graph,expected_bin_hz,original,app,model,query,matrix_frame,selected_frame,step,ordinal,filled,history);
}

static int temporal_window_burst(auto& path,auto& next,auto& temporal_rate,auto& runtime_graph,auto& expected_bin_hz,auto& original,CoreApplication& app,NumericEditorGraphModel*& model,auto& query,auto& matrix_frame,auto& selected_frame,auto& step,AudioNodeModel*& window_function,AudioNodeModel*& center,auto& ordinal,auto& filled,auto& history,auto& centered_frame) {
    report_temporal_phase("window burst",app);
const auto before_burst=app.presenter().client().snapshot().graph_revision;
        CHECK(app.presenter().apply_pipeline_graph({})>0&&app.presenter().graph_request_pending());
        for(int function:{1,2,3})CHECK(window_function->parameter_model()->edit("function",function));
        QElapsedTimer burst;burst.start();
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>=before_burst+2&&query(17,matrix_frame);},6000));
        CHECK(std::get<int>(window_function->parameter_model()->field("function")->read())==3);
        std::printf("Discrete window burst during pending Apply: latest selection delivered in %lld ms\n",static_cast<long long>(burst.elapsed()));
        CHECK(window_function->parameter_model()->setBypassed(true));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&query(17,matrix_frame);},6000));
        return temporal_center_changes(path,next,temporal_rate,runtime_graph,expected_bin_hz,original,app,model,query,matrix_frame,selected_frame,step,center,ordinal,filled,history,centered_frame);
}

static int temporal_window_changes(auto& path,auto& next,auto& temporal_rate,auto& runtime_graph,auto& expected_bin_hz,auto& original,CoreApplication& app,NumericEditorGraphModel*& model,auto& query,auto& matrix_frame,auto& selected_frame,auto& step,AudioNodeModel*& window_function,AudioNodeModel*& center,auto& ordinal,auto& filled,auto& history,auto& before_ordinal,auto& centered_frame) {
    report_temporal_phase("window functions",app);
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
        return temporal_window_burst(path,next,temporal_rate,runtime_graph,expected_bin_hz,original,app,model,query,matrix_frame,selected_frame,step,window_function,center,ordinal,filled,history,centered_frame);
}

static int temporal_window_bypass(auto& path,auto& next,auto& temporal_rate,auto& runtime_graph,auto& expected_bin_hz,auto& original,CoreApplication& app,NumericEditorGraphModel*& model,auto& query,auto& matrix_frame,auto& selected_frame,auto& step,auto& centered_frame,AudioNodeModel*& window_function,AudioNodeModel*& center,auto& ordinal,auto& filled,auto& history,auto& before_ordinal) {
    report_temporal_phase("window bypass",app);
    query.set_required({17});
for(bool bypass:{false,true}) {
            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(window_function->parameter_model()->setBypassed(bypass));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(17,matrix_frame);},6000));
        }
        CHECK(window_function->parameter_model()->setBypassed(false));
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&query(17,matrix_frame);},6000));
        return temporal_window_changes(path,next,temporal_rate,runtime_graph,expected_bin_hz,original,app,model,query,matrix_frame,selected_frame,step,window_function,center,ordinal,filled,history,before_ordinal,centered_frame);
}

static int temporal_selected_band(CoreApplication& app,auto& query,auto& selected_frame,auto& band,AudioNodeModel*& row) {
    report_temporal_phase("row",app,int(band));

            const auto revision=app.presenter().client().snapshot().graph_revision;
            CHECK(row->parameter_model()->edit("row",signal_contracts::NumericLiteral{std::uint64_t(band)}));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision&&query(19,selected_frame);},6000));
            CHECK(std::get<signal_contracts::NumericView>(selected_frame.frame.view().payload).actual[0]==256);

return 0;
}

static int temporal_rejected_row(CoreApplication& app,NumericEditorGraphModel* model,AudioNodeModel* row) {
    auto executable=model->numeric_graph();QByteArray before;CHECK(desktop::encode_numeric_graph(executable,before));
    const auto revision=app.presenter().client().snapshot().graph_revision;
    CHECK(row->parameter_model()->edit("row",signal_contracts::NumericLiteral{std::uint64_t{50}}));
    CHECK(row->numeric_node()&&!model->node_error(*row->numeric_node()).isEmpty());
    QString error;CHECK(!model->export_numeric_graph(executable,&error));CHECK(!error.isEmpty());
    QByteArray after;CHECK(desktop::encode_numeric_graph(executable,after));CHECK(after==before);
    CHECK(!app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision==revision);
    CHECK(row->parameter_model()->edit("row",signal_contracts::NumericLiteral{std::uint64_t{24}}));
    CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&model->node_error(*row->numeric_node()).isEmpty();}));
    return 0;
}

static int temporal_row_window(auto& path,auto& next,auto& temporal_rate,auto& runtime_graph,auto& expected_bin_hz,auto& original,CoreApplication& app,NumericEditorGraphModel*& model,auto& query,auto& matrix_frame,auto& selected_frame,auto& step,auto& centered_frame) {
    report_temporal_phase("row edits",app);
    query.set_required({17,19});
        auto* row=model->delegateModel<AudioNodeModel>(19);auto* window_function=model->delegateModel<AudioNodeModel>(22);
        auto* center=model->delegateModel<AudioNodeModel>(23);
        CHECK(center&&center->numeric_node()->kind=="MatrixCenter"&&center->numeric_node()->enabled);
        CHECK(row&&window_function&&window_function->parameter_model()->bypassed());
        std::uint64_t ordinal=0,filled=0;pipeline::DeliveredNumericFrame history;
        CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok);
        const auto before_ordinal=ordinal;
        for(std::uint64_t band:{49u,0u,24u}) {if(const int failure=temporal_selected_band(app,query,selected_frame,band,row);failure)return failure;}
        if(const int failure=temporal_rejected_row(app,model,row);failure)return failure;
        CHECK(app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},history,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=before_ordinal);
        return temporal_window_bypass(path,next,temporal_rate,runtime_graph,expected_bin_hz,original,app,model,query,matrix_frame,selected_frame,step,centered_frame,window_function,center,ordinal,filled,history,before_ordinal);
}

static int temporal_matrices(auto& path,auto& next,auto& temporal_rate,auto& runtime_graph,auto& expected_bin_hz,auto& original,CoreApplication& app,NumericEditorGraphModel*& model,auto& query) {
    report_temporal_phase("initial matrices",app);
    query.set_required({17,19});
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
        query.set_required({17,23});
        pipeline::DeliveredNumericFrame centered_frame;
        CHECK(wait_until([&]{return query(23,centered_frame);},6000));
        const auto centered=std::get<signal_contracts::NumericView>(centered_frame.frame.view().payload);
        CHECK(numeric_application_oracle::centered_history(centered));
        const auto matrix=std::get<signal_contracts::NumericView>(matrix_frame.frame.view().payload);
        double step=0;
        CHECK(numeric_application_oracle::temporal_matrix(matrix,expected_bin_hz,step));
        CHECK(numeric_application_oracle::positive_matrix_energy(matrix));
        return temporal_row_window(path,next,temporal_rate,runtime_graph,expected_bin_hz,original,app,model,query,matrix_frame,selected_frame,step,centered_frame);
}

static bool query_temporal_frame(CoreApplication& app,auto& id,auto& frame,auto& selected_mutex,auto& selected_latest,auto& pcm_latest,auto& center_latest) {

            if(id==19||id==200||id==23) {
                std::lock_guard lock(selected_mutex);frame=id==19?selected_latest:id==200?pcm_latest:center_latest;
                return frame.frame&&frame.status==signal_contracts::Status::ok&&frame.frame.view().stamp.available
                    &&frame.delivery.graph_revision==app.presenter().client().snapshot().graph_revision;
            }
            return app.pipeline().query_numeric({"main",id,domain::PortDirection::Output,0},frame)==signal_contracts::Status::ok
                &&frame.frame&&frame.frame.view().stamp.available
                &&frame.delivery.graph_revision==app.presenter().client().snapshot().graph_revision;
        }

struct TemporalFrameQuery {
    pipeline::NumericEngine& engine;
    std::uint64_t tap;
    std::function<bool(int,pipeline::DeliveredNumericFrame&)> read;
    void set_required(std::initializer_list<int> nodes) {
        std::vector<pipeline::EndpointIdentity> endpoints;endpoints.reserve(nodes.size());
        for(const auto node:nodes)endpoints.push_back({"main",node,domain::PortDirection::Output,0});
        engine.update_numeric_tap(tap,std::move(endpoints));
    }
    bool operator()(int id,pipeline::DeliveredNumericFrame& frame)const {return read(id,frame);}
};

static int temporal_delivery(auto& path,auto& next,auto& temporal_rate,auto& runtime_graph,auto& expected_bin_hz,auto& original,CoreApplication& app,NumericEditorGraphModel*& model) {
    report_temporal_phase("startup",app);
        std::mutex selected_mutex;pipeline::DeliveredNumericFrame selected_latest,pcm_latest,center_latest;
        const auto tap=app.pipeline().subscribe_numeric_tap({{"main",17,domain::PortDirection::Output,0},
            {"main",19,domain::PortDirection::Output,0}},[&](const pipeline::DeliveredNumericFrame& delivered){
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
        TemporalFrameQuery query{app.pipeline(),tap,[&](int id,pipeline::DeliveredNumericFrame& frame) {
            return query_temporal_frame(app,id,frame,selected_mutex,selected_latest,pcm_latest,center_latest);
        }};
        return temporal_matrices(path,next,temporal_rate,runtime_graph,expected_bin_hz,original,app,model,query);
}

static int temporal_start(auto& argv,auto& path,auto& next) {
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
        return temporal_delivery(path,next,temporal_rate,runtime_graph,expected_bin_hz,original,app,model);
}

static int validate_temporal_ports(auto& node,auto& rate,auto& port) {

                    const auto contract=port.contract.view();const auto* value=std::get_if<signal_contracts::NumericContract>(&contract);CHECK(value);
                    if(value->shape.rank!=signal_contracts::Rank::Scalar){CHECK(value->clock.sample_rate==rate&&value->clock.tick_seconds==(signal_contracts::Ratio{1,rate}));}
                    CHECK(value->channel_order.size()==1);
                    if(node.id==4&&port.direction==domain::PortDirection::Output){
                        CHECK(value->coordinates[0].step.numerator*4096==std::int64_t(rate)*std::int64_t(value->coordinates[0].step.denominator));}

return 0;
}

static int validate_temporal_capture(auto& rate,auto& node,auto& captures,auto& admitted) {
++captures;CHECK(node.id==13&&domain::data_outputs(node).size()==1);
                    domain::ProcessingParameterView source_rate;CHECK(domain::resolve_processing_parameter(admitted,"sample_rate",source_rate)==signal_contracts::Status::ok);
                    CHECK(std::visit([&](auto value){return value==rate;},std::get<signal_contracts::NumericLiteral>(source_rate)));
return 0;
}

static int validate_temporal_clock_ports(auto& rate,auto& node,auto& clocks) {
if(node.kind=="SampleClock"){++clocks;CHECK(std::get<domain::SampleClockProcessingParameters>(node.parameters).sample_rate==rate);}
                if(node.kind=="InputSelector"){CHECK(node.id==200&&node.inputs.size()==4&&domain::data_outputs(node).size()==2);}
                for(const auto* ports:{&node.inputs,&node.outputs})for(const auto& port:*ports) { if(const int failure=validate_temporal_ports(node,rate,port);failure)return failure; }

return 0;
}

static int validate_temporal_node(auto& rate,auto& node,auto& generators,auto& captures,auto& clocks) {

                CHECK(node.kind!="PcmResampler"&&node.kind!="Mixer"&&node.id!=14&&node.id!=332&&node.id!=364);
                domain::ResolvedProcessingNode admitted;CHECK(domain::resolve_processing_node(node,numeric_editor::limits(),admitted));
                if(node.kind=="SignalGenerator"){++generators;CHECK(domain::data_outputs(node).size()==1&&node.clock_node==16);}
                if(node.kind=="AudioSource"){if(const int failure=validate_temporal_capture(rate,node,captures,admitted);failure)return failure;}
                return validate_temporal_clock_ports(rate,node,clocks);
}

static int validate_temporal_rate(auto& rate) {

            auto negotiated=numeric_temporal_fft_graph(rate);
            QString error;CHECK(desktop::resolve_numeric_source_format(negotiated,13,rate,1,numeric_editor::limits(),&error));
            CHECK(negotiated.execution_interval==(signal_contracts::Ratio{1,120}));
            domain::NumericProcessingGraph resolved;CHECK(domain::resolve_configuration_bindings(negotiated,numeric_editor::limits(),resolved));
            unsigned generators=0,captures=0,clocks=0;
            for(const auto& node:negotiated.nodes) { if(const int failure=validate_temporal_node(rate,node,generators,captures,clocks);failure)return failure; }
            CHECK(generators==1&&captures==1&&clocks==1);

return 0;
}

static int scenario_temporal_fft(char** argv,const QString& path,const QString& next) {
        CHECK(test_fixture::isolated_gui_platform());
        for(unsigned rate:{44100u,48000u}) { if(const int failure=validate_temporal_rate(rate);failure)return failure; }
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
        return temporal_start(argv,path,next);
}

static int scenario_export_current(char** argv) {
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

static int current_two_streams(auto& example) {

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

return 0;
}

static int current_startup(CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor) {

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

return 0;
}

static int current_temporal(CoreApplication& app) {

                auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
                auto* selector=model->delegateModel<AudioNodeModel>(200);CHECK(selector&&selector->selected_input()==0);
                const auto& shown=model->numeric_graph();
                CHECK(selector->numeric_node()->inputs.size()==4 && domain::data_outputs(*selector->numeric_node()).size()==2 && selector->numeric_node()->outputs.size()==2);
                const auto rate=std::get<signal_contracts::NumericContract>(selector->numeric_node()->outputs[1].contract.view());
                CHECK(rate.shape.rank==signal_contracts::Rank::Scalar && rate.unit==signal_contracts::NumericUnit::Hertz);
                const auto wired=[&](domain::NodeId from,domain::PortId output,domain::PortId input){return std::any_of(shown.edges.begin(),shown.edges.end(),[&](const auto& edge){return edge.from==from && edge.to==200 && edge.output_port==output && edge.input_port==input;});};
                CHECK(wired(1,0,0) && wired(1,domain::sample_rate_output,1) && wired(13,0,2) && wired(13,domain::sample_rate_output,3));
                CHECK(selector->presentation().value("stream_names").toArray().size()==2);
                CHECK(selector->stream_name(0)==ui::text("Generator"));
                CHECK(selector->stream_name(1)==ui::text("Loopback"));

return 0;
}

static int current_stereo_choice(auto& stream,CoreApplication& app,NumericEditorGraphModel*& model,auto& lane_mutex,auto& lanes,auto& invalid_lane,auto& loopbackEnergy,auto& microphoneEnergy) {

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

return 0;
}

static void observe_stereo_frame(auto& delivered,auto& lane_mutex,auto& lanes,auto& invalid_lane) {

                    if(!delivered.context||!delivered.frame||!delivered.frame.view().stamp.available)return;
                    const auto& endpoint=delivered.context->endpoint;if(endpoint.direction!=domain::PortDirection::Output||(endpoint.node!=104&&endpoint.node!=404))return;
                    std::lock_guard lock(lane_mutex);const unsigned lane=endpoint.node==104?0:1;auto& state=lanes[lane];
                    if(state.revision!=delivered.delivery.graph_revision)state={delivered.delivery.graph_revision,0,false};
                    const auto view=delivered.frame.view();const auto* values=std::get_if<signal_contracts::NumericView>(&view.payload);
                    if(!values||values->contract->dtype!=signal_contracts::DType::Float32||values->actual[0]!=256){invalid_lane=true;return;}
                    for(std::size_t i=0;i<values->actual[0];++i){float value=0;if(signal_contracts::load_element(*values,{0,i,0},value)!=signal_contracts::Status::ok||!std::isfinite(value)){invalid_lane=true;return;}state.positive|=std::abs(value)>1e-12f;state.energy+=std::abs(value);}
                    if(!state.frames)state.first_sequence=view.stamp.sequence;else if(view.stamp.sequence<state.last_sequence)invalid_lane=true;state.last_sequence=view.stamp.sequence;
                    ++state.frames;
                }

static int current_stereo(CoreApplication& app) {

                auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
                struct Lane {std::uint64_t revision=0;unsigned frames=0;bool positive=false;std::uint64_t first_sequence=0,last_sequence=0;double energy=0;};
                std::mutex lane_mutex;std::array<Lane,2> lanes{};bool invalid_lane=false;
                const auto tap=app.pipeline().subscribe_numeric_tap({},[&](const pipeline::DeliveredNumericFrame& delivered){ return observe_stereo_frame(delivered,lane_mutex,lanes,invalid_lane); },{});CHECK(tap);
                struct TapGuard {pipeline::NumericEngine& engine;std::uint64_t token;~TapGuard(){engine.unsubscribe_numeric_tap(token);}} guard{app.pipeline(),tap};
                double loopbackEnergy=0,microphoneEnergy=0;
                for(unsigned stream:{1u,2u,0u}) { if(const int failure=current_stereo_choice(stream,app,model,lane_mutex,lanes,invalid_lane,loopbackEnergy,microphoneEnergy);failure)return failure; }
                CHECK(loopbackEnergy>0 && microphoneEnergy>loopbackEnergy*10);


return 0;
}

static int current_channel_signature(CoreApplication& app,auto& channel,auto& signatures) {

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

return 0;
}

static int current_stream_measurements(CoreApplication& app) {

                  std::vector<double> signatures[2];
                  for(unsigned channel=0;channel<2;++channel) { if(const int failure=current_channel_signature(app,channel,signatures);failure)return failure; }
                  CHECK(signatures[0].size()==signatures[1].size());double difference=0;
                  for(std::size_t band=0;band<signatures[0].size();++band)difference+=std::abs(signatures[0][band]-signatures[1][band]);
                  CHECK(difference>0.05);

return 0;
}

static int report_current_timeout(auto& example,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor,auto& revision,auto& action) {
const auto current=app.presenter().client().snapshot();auto* validation=window->findChild<QLabel*>("pipelineValidation");
                 const auto status=app.presenter().apply_status().toUtf8(),message=window->statusBar()->currentMessage().toUtf8();
                 const auto detail=validation?validation->toolTip().toUtf8():QByteArray{};
                  std::fprintf(stderr,"example=%s apply timeout before=%llu after=%llu pending=%d enabled=%d status=%s statusBar=%s validation=%s\n",example.id,(unsigned long long)revision,(unsigned long long)current.graph_revision,int(app.presenter().graph_request_pending()),int(action->isEnabled()),status.constData(),message.constData(),detail.constData());
                  domain::NumericProcessingGraph draft,evaluated,executable;QString draftError;
                  if(editor->numeric_draft(draft,&draftError)){
                      const auto result=domain::resolve_numeric_parameter_bindings(draft,numeric_editor::limits(),evaluated,executable);
                      std::fprintf(stderr,"example=%s draft resolver error=%u node=%d port=%u\n",example.id,unsigned(result.error),int(result.node),unsigned(result.port));
                  }else std::fprintf(stderr,"example=%s draft unavailable: %s\n",example.id,draftError.toUtf8().constData());
return 0;
}

static int validate_current_history(CoreApplication& app,auto& node) {

                pipeline::DeliveredNumericFrame frame;std::uint64_t ordinal=0,filled=0;
                CHECK(wait_until([&]{return app.pipeline().query_numeric({"main",node.id,domain::PortDirection::Output,0},frame,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=3;}));
                CHECK(std::get<signal_contracts::NumericView>(frame.frame.view().payload).actual[0]==400&&filled>0);

return 0;
}

static int current_example_outputs(auto& example,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor) {
if(QByteArray(example.id)=="startup") { if(const int failure=current_startup(app,window,editor);failure)return failure; }
            if(QByteArray(example.id)=="temporal-fft") { if(const int failure=current_temporal(app);failure)return failure; }
            for(const auto& node:example.graph.nodes)if(node.kind=="Spectrogram") {if(const int failure=validate_current_history(app,node);failure)return failure;}
            if(QByteArray(example.id)=="stereo-sources") { if(const int failure=current_stereo(app);failure)return failure; }
              if(QByteArray(example.id)=="two-streams") { if(const int failure=current_stream_measurements(app);failure)return failure; }
            std::printf("Current example %s: actual menu Apply and all History branches PASS\n",example.id);

return 0;
}

static int current_example(auto& example,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor) {

              if(QByteArray(example.id)=="two-streams") { if(const int failure=current_two_streams(example);failure)return failure; }
            auto* action=window->findChild<QAction*>(QStringLiteral("example-")+example.id);CHECK(action);
            const auto revision=app.presenter().client().snapshot().graph_revision;action->trigger();
            std::fprintf(stderr,"apply example=%s status=%s warning=%s\n",qPrintable(action->objectName()),qPrintable(app.presenter().apply_status()),qPrintable(app.presenter().saved_graph_warning()));
            const bool exampleApplied=wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision;});
             if(!exampleApplied){if(const int failure=report_current_timeout(example,app,window,editor,revision,action);failure)return failure;}
            CHECK(exampleApplied);
            return current_example_outputs(example,app,window,editor);
}

static int scenario_current_examples(const QString& path,const QString& next) {
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::startup_capture_factory(false,2));install_synthetic_capture_discovery(app,48000,2);
        app._ctx.file_configs.ui.autosave_enabled=false;
        const bool started=wait_until([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();});
        if(!started){const auto snapshot=app.presenter().client().snapshot();std::fprintf(stderr,"Startup Apply: %s\n",qPrintable(app.presenter().apply_status()));for(auto it=snapshot.statuses.begin();it!=snapshot.statuses.end();++it)std::fprintf(stderr,"component %s state=%d error=%s\n",qPrintable(it.key()),int(it.value().state),qPrintable(it.value().error));}
        CHECK(started);
        MainWindow* window=nullptr;for(const auto items_12=QApplication::topLevelWidgets();auto* widget:items_12)if(auto* found=qobject_cast<MainWindow*>(widget))window=found;
        CHECK(window);auto* editor=window->findChild<SchemeEditorWidget*>();CHECK(editor);
        for(const auto& example:numeric_example_graphs()) { if(const int failure=current_example(example,app,window,editor);failure)return failure; }
        app.save_configuration_as(next);CHECK(QFile::exists(next));return 0;
    }

static int scenario_verify_project(char** argv,const QString& path) {
        CHECK(test_fixture::isolated_gui_platform());
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

static int startup_menu_locale(auto& locale,QComboBox*& language,QAction*& demo,QMenu*& menu,QMenu*& display_menu){

            language->setCurrentIndex(locale);QCoreApplication::processEvents();
            CHECK(demo->text()==(locale==0?QString::fromUtf8("Спектрограмма: генератор, loopback, микрофон"):QStringLiteral("Spectrogram: generator, loopback, microphone")));
            CHECK(menu->title()==(locale==0?QString::fromUtf8("Пресеты"):QStringLiteral("Presets")));
            CHECK(display_menu->title()==(locale==0?QString::fromUtf8("Отображение"):QStringLiteral("Display")));

return 0;
}
static int startup_menu_verify_selector(CoreApplication& app,MainWindow*& window,NumericEditorGraphModel*& model,auto& path,auto& original_file,auto& old_builtin,auto& stored_presets,auto& startup_revision){
export_startup_scenes(window->findChild<SchemeEditorWidget*>(),model);
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>startup_revision && !app.presenter().graph_request_pending();}));
        CHECK(verify_startup_selector(app,window,false));
        CHECK(app._ctx.file_configs.ui.pipeline_presets==stored_presets&&read(path)==original_file);
        std::printf("unconditional generatorLoopbackPreset existing config old_builtin=%d: full stereo draft/layout/views/selector/card/History; original file and stored presets unchanged PASS\n",int(old_builtin));return 0;




}
static int startup_menu_apply(CoreApplication& app,MainWindow*& window,NumericEditorGraphModel*& model,auto& path,auto& original_file,auto& old_builtin,auto& stored_presets,QComboBox*& language,auto& original_language,QAction*& demo){
CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();}));const auto startup_revision=app.presenter().client().snapshot().graph_revision;
        language->setCurrentIndex(original_language);demo->trigger();
        CHECK(model->numeric_graph().nodes.size()==numeric_startup_graph().nodes.size()&&model->numeric_graph().edges.size()==native_mono_example(numeric_startup_graph()).edges.size());
        CHECK(app._ctx.file_configs.ui.pipeline_presets==stored_presets&&read(path)==original_file);
        CHECK(startup_primary_views_open(window));
        const auto visibleGraph=model->numeric_graph();CHECK(std::none_of(visibleGraph.nodes.begin(),visibleGraph.nodes.end(),[](const auto& n){return n.kind=="MelView";}));
        CHECK(wait_until([&]{QSet<QPair<int,int>> positions;for(const auto items_14=model->allNodeIds();auto id:items_14){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}return positions.size()==numeric_startup_graph().nodes.size();}));
        return startup_menu_verify_selector(app,window,model,path,original_file,old_builtin,stored_presets,startup_revision);
}
static int startup_menu_routes(CoreApplication& app,MainWindow*& window,NumericEditorGraphModel*& model,auto& path,auto& original_file,auto& old_builtin,auto& stored_presets){
auto* demo=window->findChild<QAction*>("example-startup");CHECK(demo&&demo->isEnabled()&&demo->isVisible());
        auto* menu=window->findChild<QMenu*>("pipelinePresetsMenu");auto* display_menu=window->findChild<QMenu*>("graphsMenu");
        auto* presets_button=window->findChild<QToolButton*>("pipelinePresets");
        CHECK(menu&&display_menu&&presets_button&&menu->actions().contains(demo));
        CHECK(display_menu->actions().contains(menu->menuAction())&&window->menuBar()->actions().contains(display_menu->menuAction()));
        CHECK(display_menu->menuAction()->isVisible()&&menu->menuAction()->isVisible());
        CHECK(presets_button->menu()==menu&&presets_button->isVisible());
        auto* language=window->findChild<QComboBox*>("interfaceLanguage");CHECK(language);
        const auto original_language=language->currentIndex();
        for(int locale:{0,1}) {if(const int failure=startup_menu_locale(locale,language,demo,menu,display_menu);failure)return failure;}
        return startup_menu_apply(app,window,model,path,original_file,old_builtin,stored_presets,language,original_language,demo);
}
static int startup_menu_stored_state(CoreApplication& app,MainWindow*& window,NumericEditorGraphModel*& model,auto& path,auto& original_file,auto& old_builtin){
const auto stored_presets=app._ctx.file_configs.ui.pipeline_presets;
        CHECK(stored_presets.size()==(old_builtin?1:0));CHECK(model->numeric_graph().nodes.size()==numeric_generator_graph().nodes.size());
        if(old_builtin){CHECK(stored_presets[0].toObject()["graph"].toObject()["nodes"].toArray().size()==int(numeric_generator_graph().nodes.size()));}
        CHECK(!app._ctx.file_configs.ui.autosave_enabled&&!app._ctx.file_configs.ui.pipeline_auto_apply);
        CHECK(app.presenter().client().snapshot().graph_revision==0);
        return startup_menu_routes(app,window,model,path,original_file,old_builtin,stored_presets);
}
static int scenario_startup_menu(char** argv,const QString& path) {
        CHECK(test_fixture::isolated_gui_platform());
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
        MainWindow* window=nullptr;for(const auto items_13=QApplication::topLevelWidgets();auto* candidate:items_13)if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(window&&model);window->show();
        return startup_menu_stored_state(app,window,model,path,original_file,old_builtin);
}

static int startup_selector_fixture(auto& path,auto& original){

            desktop::NumericProjectDocument fixture;desktop::NumericDraftPresentation display;
            const auto example=numeric_startup_example();
            auto ui=example_presentation(example,QStringLiteral("local"),QString::fromStdString(example.graph.graph_id));
            ui["autosave_enabled"]=false;ui["pipeline_auto_apply"]=false;
            display.application_settings=QJsonObject{{"ui",ui}};
            for(const auto& node:numeric_startup_graph().nodes)
                display.positions[QString::number(node.id)]=QJsonObject{{"x",double(node.id%4)*420},{"y",double(node.id/4)*260}};
            CHECK(desktop::create_numeric_startup_project(numeric_startup_graph(),fixture));
            CHECK(desktop::stage_numeric_presentation(fixture,display,fixture));CHECK(desktop::save_numeric_project(fixture,path));original=read(path);

return 0;
}
static int scenario_startup_selector(char** argv,const QString& path) {
        CHECK(test_fixture::isolated_gui_platform());
        const bool unavailable=QByteArray(argv[1])=="--startup-selector-unavailable";
        QByteArray original;
        if(unavailable){if(const int failure=startup_selector_fixture(path,original);failure)return failure;}
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::startup_capture_factory(unavailable));
        install_synthetic_capture_discovery(app);
        MainWindow* window=nullptr;for(const auto items_15=QApplication::topLevelWidgets();auto* candidate:items_15)if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();
        auto* autosave=window->findChild<QCheckBox*>("enableAutosave");CHECK(autosave);autosave->setChecked(false);
        QObject::connect(&app.presenter(),&GuiPresenter::graph_apply_finished,&app,[](quint64,bool ok,const QString& error){if(!ok)std::fprintf(stderr,"startup rejected: %s\n",qPrintable(error));});
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();},6000));
        CHECK(verify_startup_selector(app,window,unavailable));CHECK(unavailable?read(path)==original:!QFile::exists(path));return 0;
    }

static int shipped_rejected_admission(auto& persisted,auto& candidate,auto& diagnostic,auto& before) {
    auto executable=candidate.numeric_graph();
    auto invalid=persisted;
    auto invalid_view=*std::find_if(invalid.nodes.begin(),invalid.nodes.end(),[](const auto& n){return n.id==8;});
    invalid_view.id=17;invalid_view.kind="MelView";invalid.nodes.push_back(std::move(invalid_view));
    invalid.edges.push_back({7,17,0,0});
    CHECK(candidate.set_numeric_graph(invalid,&diagnostic));
    const auto retained=candidate.numeric_graph();
    CHECK(std::find(retained.edges.begin(),retained.edges.end(),domain::Edge{7,17,0,0})!=retained.edges.end());
    const auto rejected=std::find_if(retained.nodes.begin(),retained.nodes.end(),[](const auto& n){return n.id==17;});
    CHECK(rejected!=retained.nodes.end()&&!candidate.node_error(*rejected).isEmpty());
    CHECK(!candidate.export_numeric_graph(executable,&diagnostic));CHECK(!diagnostic.isEmpty());
    QByteArray after;CHECK(desktop::encode_numeric_graph(executable,after));CHECK(after==before);
    return 0;
}

static int shipped_persisted_admission(auto& shipped,NumericEditorGraphModel*& model) {

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
            // Incompatible user drafts remain editable; execution export rejects the
            // offending 7 -> 17 route atomically without replacing its output.
            return shipped_rejected_admission(persisted,candidate,diagnostic,before);
}

static bool inspect_shipped_example(auto& label,SchemeEditorWidget*& editor,NumericEditorGraphModel*& model,auto& gui_workflow,CoreApplication& app,MainWindow*& window) {

            domain::NumericProcessingGraph graph;QString error;if(!editor->numeric_draft(graph,&error)){std::fprintf(stderr,"%s draft: %s\n",label,qPrintable(error));return false;}
            auto expected=numeric_startup_graph();
            if(QByteArray(label)=="stereo-sources")for(const auto& example:numeric_example_graphs())if(QByteArray(example.id)=="stereo-sources")expected=example.graph;
             const auto channels=QByteArray(label)=="stereo-sources"?2u:1u;
             if(!desktop::resolve_numeric_source_format(expected,13,48000,channels,numeric_editor::limits(),&error)||!desktop::resolve_numeric_source_format(expected,14,48000,channels,numeric_editor::limits(),&error))return false;
            if(graph.nodes.size()!=expected.nodes.size()||graph.edges.size()!=expected.edges.size()||!std::is_permutation(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),expected.configuration_bindings.begin(),expected.configuration_bindings.end()))return false;
            QSet<QPair<int,int>> positions;for(const auto items_17=model->allNodeIds();auto id:items_17){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}
            if(positions.size()!=int(graph.nodes.size())){std::fprintf(stderr,"%s overlapping startup positions: %d\n",label,int(positions.size()));return false;}
            const bool delivered=gui_workflow?measure_shipped_gui(app,window,label):measure_shipped_example(graph,label,pipeline::AudioSourceKind::TestSignal);
            std::fflush(stdout);
            return delivered;
        }

static int shipped_menu_example(CoreApplication& app,MainWindow*& window,auto& id,auto& inspect) {

            auto* action=window->findChild<QAction*>(QStringLiteral("example-")+id);CHECK(action&&action->isVisible()&&action->isEnabled());
            const auto revision=app.presenter().client().snapshot().graph_revision;action->trigger();
            std::fprintf(stderr,"apply example=%s status=%s warning=%s\n",qPrintable(action->objectName()),qPrintable(app.presenter().apply_status()),qPrintable(app.presenter().saved_graph_warning()));
            CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>revision;}));
            CHECK(inspect(id));

return 0;
}

static int shipped_ui_examples(const QString& path,auto& gui_workflow,CoreApplication& app,NumericEditorGraphModel*& model,MainWindow*& window,SchemeEditorWidget*& editor) {
auto inspect=[&](const char* label){ return inspect_shipped_example(label,editor,model,gui_workflow,app,window); };
        CHECK(wait_until([&]{QSet<QPair<int,int>> positions;for(const auto items_18=model->allNodeIds();auto id:items_18){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}return positions.size()==numeric_startup_graph().nodes.size();}));
        CHECK(inspect("missing-config"));
        for(const auto* id:{"startup","stereo-sources"}) {if(const int failure=shipped_menu_example(app,window,id,inspect);failure)return failure;}
        CHECK(editor->load_preset("builtin-test-signal"));QCoreApplication::processEvents();
        CHECK(inspect("My-presets/builtin-test-signal"));
        CHECK(!QFile::exists(path));
        if(gui_workflow){CHECK(verify_example_autosave(app,window,path));}
        std::puts("all shipped examples actual UI selection / synthetic full-chain Apply Run PASS");return 0;
    }

static int scenario_shipped_examples(char** argv,const QString& path) {
        const bool gui_workflow=QByteArray(argv[1])=="--shipped-examples-ui";
        CHECK(test_fixture::isolated_gui_platform());
        auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::startup_capture_factory(false,2));
        install_synthetic_capture_discovery(app,48000,2);
        app._ctx.file_configs.ui.autosave_enabled=false;app._ctx.file_configs.ui.pipeline_auto_apply=false;
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        MainWindow* window=nullptr;for(const auto items_16=QApplication::topLevelWidgets();auto* candidate:items_16)if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();auto* editor=window->findChild<SchemeEditorWidget*>();CHECK(editor);
        auto* automatic=window->findChild<QCheckBox*>("pipelineAutoApply");auto* autosave=window->findChild<QCheckBox*>("enableAutosave");
        CHECK(!automatic&&autosave);autosave->setChecked(false);
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();},5000));
        CHECK(app.presenter().client().snapshot().statuses.value("analyzer").state==pipeline::State::Running);
        // Exercise the actual editor admission after both JSON persistence
        // boundaries, independently of whichever draft is currently displayed.
        for(const auto& shipped:{numeric_default_graph(),numeric_generator_graph(),numeric_startup_graph()}) { if(const int failure=shipped_persisted_admission(shipped,model);failure)return failure; }
        return shipped_ui_examples(path,gui_workflow,app,model,window,editor);
}

static int first_run_performance(CoreApplication& app,auto& graph) {
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

static bool first_run_scalar_view(const domain::ConfiguredProcessingNode& node) {
    const auto kind=domain::node_kind(node.kind);
    return kind && domain::is_scalar_view(*kind) && node.inputs.size()==2;
}

static int first_run_scalar_controls(auto& scalar,auto& scoreControlsChecked) {

            std::unique_ptr<QWidget> controls(scalar->scalar_inputs_widget());
            auto* full=controls->findChild<QCheckBox*>("scalarInput0Enabled");auto* selected=controls->findChild<QCheckBox*>("scalarInput1Enabled");
            CHECK(full && selected && full->isChecked() && selected->isChecked());scoreControlsChecked=true;

return 0;
}

static bool find_live_piano(MainWindow*& window,auto& pianoPanel) {
for(const auto items_20=window->workspaceDocks();auto* dock:items_20)if(dock->property("graphNode").toInt()==11){auto* panel=qobject_cast<GraphPanel*>(dock->widget());if(panel && panel->piano_view()){pianoPanel=panel;return panel->piano_view()->first_midi()==60 && panel->piano_view()->value_count()==13 && panel->piano_view()->peak_value()>0;}}return false;}

static int first_run_score_views(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window,AudioNodeModel*& noteModel) {
        bool scoreControlsChecked=false;
        for(const auto items_19=model->allNodeIds();const auto id:items_19)if(auto* scalar=model->delegateModel<AudioNodeModel>(id);scalar && first_run_scalar_view(*scalar->numeric_node())){if(const int failure=first_run_scalar_controls(scalar,scoreControlsChecked);failure)return failure;}
        CHECK(scoreControlsChecked);
        auto* pianoEditor=window->findChild<SchemeEditorWidget*>();CHECK(pianoEditor);emit pianoEditor->graph_open_requested(11,0);
        GraphPanel* pianoPanel=nullptr;
        CHECK(wait_until([&]{return find_live_piano(window,pianoPanel);},5000));
        CHECK(pianoPanel && pianoPanel->display_mode()=="piano" && !pianoPanel->active_chart());
        auto* previewScene=pianoEditor->findChild<QtNodes::DataFlowGraphicsScene*>();CHECK(previewScene);
        emit previewScene->nodeSelected(QtNodes::NodeId(noteModel->numeric_node()->id));
        CHECK(wait_until([&]{auto* values=pianoEditor->findChild<QLabel*>("previewValues");return values && values->text().split('\n').value(0)=="13";},3000));
        emit previewScene->nodeSelected(QtNodes::InvalidNodeId);
        CHECK(!pianoEditor->findChild<QLabel*>("previewValues"));
        std::puts("live independent broadband/note controls: explicit join, notes Hz, independent History geometry, two enabled agreement scalars PASS");
        return first_run_performance(app,graph);
}

static bool set_low_row_parameter(domain::ConfiguredProcessingNode& node,const char* key,domain::ProcessingParameterValue value) {
    auto* parameters=std::get_if<domain::TransformProcessingParameters>(&node.parameters);
    if(!parameters)return false;
    const auto field=std::find_if(parameters->values.begin(),parameters->values.end(),[key](const auto& candidate){return candidate.key==key;});
    if(field==parameters->values.end())parameters->values.push_back({key,std::move(value)});else field->value=std::move(value);
    return true;
}
static bool edit_low_row_mode(NumericEditorGraphModel* model,AudioNodeModel* row,const char* reduction,std::uint64_t count) {
    auto candidate=*row->numeric_node();
    if(candidate.kind!="BroadbandRow")return false;
    if(!set_low_row_parameter(candidate,"low_reduction",std::string(reduction)))return false;
    if(!set_low_row_parameter(candidate,"low_count",signal_contracts::NumericLiteral{count}))return false;
    return model->edit_node(QtNodes::NodeId(candidate.id),candidate);
}
static int reject_isolated_low_reduction(AudioNodeModel* row) {
    const auto before=row->save();
    CHECK(!row->parameter_model()->edit("low_reduction",1));
    CHECK(row->save()==before);
    return 0;
}

static int first_run_low_rows(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window,AudioNodeModel*& rowModel,AudioNodeModel*& noteModel,auto& rowNumber,auto& rowGeometry,auto& rowEdit) {
std::array<domain::OwnedProcessingContract,4> beforeLow;std::array<std::size_t,4> passSections{};
        CHECK(domain::musical_spectrogram_output_contracts(*rowModel->numeric_node(),std::get<signal_contracts::NumericContract>(rowModel->numeric_node()->inputs[0].contract.view()),beforeLow,&passSections)==signal_contracts::Status::ok);
        if(const int failure=reject_isolated_low_reduction(rowModel);failure)return failure;
        CHECK(rowEdit("low mean with explicit count",[&]{return edit_low_row_mode(model,rowModel,"mean",std::max<std::size_t>(1,passSections[0]));}));
        CHECK(rowNumber(*rowModel->numeric_node(),"low_count")==std::max<std::size_t>(1,passSections[0]));
        CHECK(rowEdit("low count 4",[&]{return rowModel->parameter_model()->editLiteral("low_count","4",0);}));
        CHECK(rowEdit("low pass through with explicit zero count",[&]{return edit_low_row_mode(model,rowModel,"pass_through",0);}));
        CHECK(rowNumber(*rowModel->numeric_node(),"low_count")==0 && rowGeometry());
        std::array<domain::OwnedProcessingContract,4> finalRows;std::array<std::size_t,4> finalSections{};
        CHECK(domain::musical_spectrogram_output_contracts(*rowModel->numeric_node(),std::get<signal_contracts::NumericContract>(rowModel->numeric_node()->inputs[0].contract.view()),finalRows,&finalSections)==signal_contracts::Status::ok);
        CHECK(finalSections[0]==passSections[0] && finalSections[1]==16 && finalSections[2]==4 && finalSections[3]==0 && std::get<signal_contracts::NumericContract>(noteModel->numeric_node()->outputs[0].contract.view()).shape.extents[0].maximum==13);
        return first_run_score_views(app,model,graph,window,noteModel);
}

static int first_run_row_edits(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window,AudioNodeModel*& rowModel,AudioNodeModel*& noteModel,auto& rowNumber,auto& rowGeometry,auto& rowEdit) {
        CHECK(rowEdit("first MIDI 60",[&]{return noteModel->parameter_model()->editLiteral("first_midi","60",0);},noteModel));
        CHECK(rowEdit("last MIDI 72",[&]{return noteModel->parameter_model()->editLiteral("last_midi","72",0);},noteModel));
        CHECK(rowEdit("high count 4",[&]{return rowModel->parameter_model()->editLiteral("high_count","4",0);}));
        CHECK(rowEdit("mid RMS",[&]{return rowModel->parameter_model()->edit("mid_reduction",2);}));
        CHECK(rowEdit("mid weighted, uniform default",[&]{return rowModel->parameter_model()->edit("mid_reduction",3);}));
        const auto* weights=rowModel->parameter_model()->field("weights");CHECK(weights && std::get<QString>(weights->read()).isEmpty());
        return first_run_low_rows(app,model,graph,window,rowModel,noteModel,rowNumber,rowGeometry,rowEdit);
}

static bool musical_scalar_agreement(NumericEditorGraphModel*& model,auto& noteId,auto& current,auto& note) {
using namespace signal_contracts;
            unsigned scores=0;
            for(const auto& n:current->nodes)if(first_run_scalar_view(n)){
                if(!n.enabled)return false;
                const auto* ui=model->delegateModel<AudioNodeModel>(QtNodes::NodeId(n.id));if(!ui)return false;
                for(unsigned p=0;p<2;++p){
                    if(!same_contract(n.inputs[p].contract.view(),note->outputs[p+1].contract.view()) || !ui->presentation().value("scalar_inputs").toObject().value(QString::number(p)).toObject().value("enabled").toBool(true))return false;
                    if(std::none_of(current->edges.begin(),current->edges.end(),[&](const auto& e){return e.from==noteId && e.output_port==p+1 && e.to==n.id && e.input_port==p;}))return false;
                    ++scores;
                }
            }
            return scores==2;
        }

static bool musical_history_geometry(NumericEditorGraphModel*& model,auto& noteId,auto& current,auto& note,auto& total) {
using namespace signal_contracts;
            const auto history=std::find_if(current->nodes.begin(),current->nodes.end(),[](const auto& n){return n.id==7;});
            const auto view=std::find_if(current->nodes.begin(),current->nodes.end(),[](const auto& n){return n.id==8;});
            const auto joined=std::find_if(current->nodes.begin(),current->nodes.end(),[](const auto& n){return n.kind=="ArrayConcat";});
            if(history==current->nodes.end() || view==current->nodes.end() || joined==current->nodes.end() || !same_contract(history->inputs[0].contract.view(),joined->outputs[0].contract.view()) || !same_contract(history->outputs[0].contract.view(),view->inputs[0].contract.view()))return false;
            const auto matrix=std::get<NumericContract>(history->outputs[0].contract.view());
            if(matrix.shape.extents[0].maximum!=400 || matrix.shape.extents[1].maximum!=total)return false;
return musical_scalar_agreement(model,noteId,current,note);
}

static bool musical_row_geometry(CoreApplication& app,NumericEditorGraphModel*& model,auto& noteId,auto& rowNumber) {

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
return musical_history_geometry(model,noteId,current,note,total);
}

static std::uint64_t processing_row_number(auto& node,auto& key) {

            domain::ResolvedProcessingNode resolved;domain::ProcessingParameterView value;
            if(!domain::resolve_processing_node(node,numeric_editor::limits(),resolved) || domain::resolve_processing_parameter(resolved,key,value)!=signal_contracts::Status::ok || !std::holds_alternative<signal_contracts::NumericLiteral>(value))return UINT64_MAX;
            return std::visit([](auto n){return std::uint64_t(n);},std::get<signal_contracts::NumericLiteral>(value));
        }

static bool edit_musical_row(CoreApplication& app,MainWindow*& window,auto& label,auto& edit,auto& edited,auto& rowModel,auto& rowGeometry) {

            const auto prior=app.presenter().client().snapshot().graph_revision;
            const auto prior_parameters=(edited?edited:rowModel)->numeric_node()->parameters;
            if(!edit()){std::fprintf(stderr,"row control rejected: %s status=%s\n",label,qPrintable(app.presenter().apply_status()));return false;}
            const auto desired=*(edited?edited:rowModel)->numeric_node();
            const bool unchanged=domain::same_processing_parameters(prior_parameters,desired.parameters);
            const bool ready=wait_until([&]{
                const auto revision=app.presenter().client().snapshot().graph_revision;
                if(app.presenter().graph_request_pending() || (unchanged?revision!=prior:revision<=prior))return false;
                const auto current=app.presenter().applied_numeric_graph();if(!current)return false;
                const auto row=std::find_if(current->nodes.begin(),current->nodes.end(),[&](const auto& n){return n.id==desired.id;});
                return row!=current->nodes.end() && domain::same_processing_parameters(row->parameters,desired.parameters) && rowGeometry();
            },5000);
            if(!ready){auto* validation=window->findChild<QLabel*>("pipelineValidation");std::fprintf(stderr,"row control timeout: %s prior=%llu current=%llu pending=%d status=%s validation=%s\n",label,(unsigned long long)prior,(unsigned long long)app.presenter().client().snapshot().graph_revision,int(app.presenter().graph_request_pending()),qPrintable(app.presenter().apply_status()),validation?qPrintable(validation->toolTip()):"");}
            return ready;
        }

static int first_run_musical(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window) {
        auto* rowModel=model->delegateModel<AudioNodeModel>(6);CHECK(rowModel && rowModel->numeric_node()->kind=="BroadbandRow");
        AudioNodeModel* noteModel=nullptr;for(const auto items_21=model->allNodeIds();const auto id:items_21)if(auto* candidate=model->delegateModel<AudioNodeModel>(id);candidate && candidate->numeric_node()->kind=="NoteSpectrum")noteModel=candidate;CHECK(noteModel);
        const auto noteId=noteModel->numeric_node()->id;
        const auto rowNumber=[&](const domain::ConfiguredProcessingNode& node,const char* key){ return processing_row_number(node,key); };
        const auto rowGeometry=[&]()->bool { return musical_row_geometry(app,model,noteId,rowNumber); };
        const auto rowEdit=[&](const char* label,const auto& edit,AudioNodeModel* edited=nullptr)->bool {return edit_musical_row(app,window,label,edit,edited,rowModel,rowGeometry);};
        return first_run_row_edits(app,model,graph,window,rowModel,noteModel,rowNumber,rowGeometry,rowEdit);
}

static int validate_window_calibration(auto& n) {

            domain::ResolvedProcessingNode r;domain::ProcessingParameterView gain;CHECK(domain::resolve_processing_node(n,numeric_editor::limits(),r));CHECK(domain::resolve_processing_parameter(r,"coherent_gain",gain)==signal_contracts::Status::ok);
            const auto cg=std::visit([](auto v){return double(v);},std::get<signal_contracts::NumericLiteral>(gain));CHECK(std::abs(cg-(.54*2048-.46))<1e-3);
            CHECK(domain::resolve_processing_parameter(r,"power_gain",gain)==signal_contracts::Status::ok);
            const auto energy=std::visit([](auto v){return double(v);},std::get<signal_contracts::NumericLiteral>(gain));CHECK(std::abs(energy-(.3974*2048-.391))<1e-3);
            CHECK(std::none_of(n.inputs.begin(),n.inputs.end(),[](const auto& port){return port.id==domain::window_size_input;}));

return 0;
}

static int set_first_spectrum_text(domain::NumericProcessingGraph& graph,int id,const char* kind,const char* key,const char* value) {
    const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[id](const auto& candidate){return candidate.id==id;});
    CHECK(node!=graph.nodes.end()&&node->kind==kind);
    auto* parameters=std::get_if<domain::TransformProcessingParameters>(&node->parameters);CHECK(parameters);
    const auto field=std::find_if(parameters->values.begin(),parameters->values.end(),[key](const auto& candidate){return candidate.key==key;});
    if(field==parameters->values.end())parameters->values.push_back({key,std::string(value)});else field->value=std::string(value);
    return 0;
}
static int edit_first_spectrum_representation(CoreApplication& app,AudioNodeModel*& projection,int mode) {
    static constexpr const char* representations[]{"magnitude","peak_amplitude","power","psd"};
    CHECK(mode>=0&&mode<4);
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
    auto requested=model->numeric_graph();const auto id=projection->numeric_node()->id;
    const auto* value=representations[mode];
    if(const int failure=set_first_spectrum_text(requested,id,"AmplitudePhase","representation",value);failure)return failure;
    if(const int failure=set_first_spectrum_text(requested,6,"BroadbandRow","input_representation",value);failure)return failure;
    if(const int failure=set_first_spectrum_text(requested,1006,"NoteSpectrum","input_representation",value);failure)return failure;
    domain::NumericProcessingGraph evaluated,executable;
    CHECK(domain::resolve_numeric_parameter_bindings(requested,numeric_editor::limits(),evaluated,executable,id));
    CHECK(model->edit_nodes(evaluated.nodes));
    projection=model->delegateModel<AudioNodeModel>(QtNodes::NodeId(id));CHECK(projection);
    return 0;
}
static int isolated_first_spectrum_draft(CoreApplication& app,NumericEditorGraphModel* model,AudioNodeModel*& projection) {
    const auto original=model->numeric_graph();const auto id=projection->numeric_node()->id;
    const auto revision=app.presenter().client().snapshot().graph_revision;
    CHECK(projection->parameter_model()->edit("representation",0));
    auto exported=original;QString error;
    CHECK(!model->export_numeric_graph(exported,&error)&&!error.isEmpty());
    CHECK(application::same_numeric_processing_graph(original,exported));
    CHECK(!app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision==revision);
    const auto working=app.presenter().applied_numeric_graph();CHECK(working&&application::same_numeric_processing_graph(original,*working));
    CHECK(model->edit_nodes(original.nodes));
    CHECK(wait_until([&]{const auto restored=app.presenter().applied_numeric_graph();return !app.presenter().graph_request_pending()&&restored&&application::same_numeric_processing_graph(original,*restored);},5000));
    projection=model->delegateModel<AudioNodeModel>(QtNodes::NodeId(id));CHECK(projection);
    return 0;
}

static int validate_spectrum_representation(auto& mode,CoreApplication& app,AudioNodeModel*& projectionModel,auto& quantities) {

            const auto revision=app.presenter().client().snapshot().graph_revision;
            const auto* descriptor=projectionModel->parameter_model()->field("representation");CHECK(descriptor&&descriptor->read);
            const bool unchanged=descriptor->read()==NodeParameterModel::Value{mode};
            if(const int failure=edit_first_spectrum_representation(app,projectionModel,mode);failure)return failure;
            CHECK(wait_until([&]{const auto current=app.presenter().client().snapshot().graph_revision;return (unchanged?current==revision:current>revision) && !app.presenter().graph_request_pending();},5000));
            const auto current=app.presenter().applied_numeric_graph();CHECK(current);
            for(const auto& n:current->nodes)if(n.kind=="AmplitudePhase" || n.id==15){CHECK(std::get<signal_contracts::NumericContract>(n.outputs[0].contract.view()).quantity==quantities[mode]);}

return 0;
}

static int first_run_spectrum_controls(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window,auto& changedWindow) {
const auto spectral=std::find_if(changedWindow->nodes.begin(),changedWindow->nodes.end(),[](const auto& n){return n.kind=="AmplitudePhase";});CHECK(spectral!=changedWindow->nodes.end());
        auto* projectionModel=model->delegateModel<AudioNodeModel>(spectral->id);CHECK(projectionModel);
        const auto* representation=projectionModel->parameter_model()->field("representation");CHECK(representation && representation->primary && representation->options.size()==4);
        const signal_contracts::Quantity quantities[]{signal_contracts::Quantity::SpectrumMagnitude,signal_contracts::Quantity::SpectrumPeakAmplitude,signal_contracts::Quantity::SpectrumPower,signal_contracts::Quantity::SpectrumPsd};
        if(const int failure=isolated_first_spectrum_draft(app,model,projectionModel);failure)return failure;
        for(int mode=0;mode<4;++mode){ if(const int failure=validate_spectrum_representation(mode,app,projectionModel,quantities);failure)return failure; }
        const auto beforeRestoreAmplitude=app.presenter().client().snapshot().graph_revision;
        if(const int failure=edit_first_spectrum_representation(app,projectionModel,1);failure)return failure;
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>beforeRestoreAmplitude && !app.presenter().graph_request_pending();},5000));
        return first_run_musical(app,model,graph,window);
}

static int validate_first_spectrum_node(auto& n) {
CHECK(n.kind!="Reroute" && n.kind!="FrequencyNormalizer");if(n.kind=="ArrayMultiply"){CHECK(n.inputs.size()==2 && domain::data_outputs(n).size()==1);}
return 0;
}

static int first_run_spectrum(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window) {
        auto* windowFunction=model->delegateModel<AudioNodeModel>(3);CHECK(windowFunction);
        const auto beforeFunctionRevision=app.presenter().client().snapshot().graph_revision;
        CHECK(windowFunction->parameter_model()->edit("function",1));
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>beforeFunctionRevision && !app.presenter().graph_request_pending();},5000));
        const auto changedWindow=app.presenter().applied_numeric_graph();CHECK(changedWindow);
        for(const auto& n:changedWindow->nodes)if(n.kind=="AmplitudePhase"){ if(const int failure=validate_window_calibration(n);failure)return failure; }
        for(const auto& n:changedWindow->nodes){if(const int failure=validate_first_spectrum_node(n);failure)return failure;}
        return first_run_spectrum_controls(app,model,graph,window,changedWindow);
}

static int validate_first_resampler_node(auto& node) {
CHECK(domain::data_outputs(node).size()==1);CHECK(std::any_of(node.inputs.begin(),node.inputs.end(),[](const auto& port){return port.id==domain::input_sample_rate_input;}));CHECK(std::get<signal_contracts::NumericContract>(domain::data_outputs(node)[0].contract.view()).clock.sample_rate==32000);
return 0;
}

static int report_first_window_failure(CoreApplication& app,NumericEditorGraphModel*& model,auto& applied,auto& beforeWindowRevision) {

            const auto draft=model->numeric_graph();domain::NumericProcessingGraph evaluated,executable;const auto admission=domain::resolve_numeric_parameter_bindings(draft,numeric_editor::limits(),evaluated,executable);
            std::fprintf(stderr,"first-run window: prior=%llu current=%llu pending=%d status=%s admission=%u node=%d port=%u\n",static_cast<unsigned long long>(beforeWindowRevision),static_cast<unsigned long long>(app.presenter().client().snapshot().graph_revision),int(app.presenter().graph_request_pending()),qPrintable(app.presenter().apply_status()),unsigned(admission.error),admission.node,unsigned(admission.port));
            for(const auto& n:draft.nodes)if(n.id==2 || n.id==4 || n.kind=="ArrayLength" || n.kind=="WindowFunction" || n.kind=="ArrayMultiply" || n.kind=="ArrayFill"){
                std::fprintf(stderr,"draft node=%d kind=%s",n.id,n.kind.c_str());for(const auto& p:n.outputs){const auto value=p.contract.view();if(const auto* c=std::get_if<signal_contracts::NumericContract>(&value))std::fprintf(stderr," out%u=%llu",unsigned(p.id),static_cast<unsigned long long>(c->shape.extents[0].maximum));}std::fprintf(stderr,"\n");
            }
            if(const auto applied=app.presenter().applied_numeric_graph())for(const auto& n:applied->nodes)if(n.id==2 || n.id==4)std::fprintf(stderr,"applied node=%d output=%llu\n",n.id,static_cast<unsigned long long>(std::get<signal_contracts::NumericContract>(n.outputs[0].contract.view()).shape.extents[0].maximum));

return 0;
}

static void first_run_fft_literal(domain::ConfiguredProcessingNode& node,const char* key) {
    auto& fields=std::get<domain::TransformProcessingParameters>(node.parameters).values;
    const auto found=std::find_if(fields.begin(),fields.end(),[key](const auto& field){return field.key==key;});
    const signal_contracts::NumericLiteral size{std::uint64_t{2048}};
    if(found==fields.end())fields.push_back({key,size});else found->value=size;
}
static int first_run_window_geometry(NumericEditorGraphModel* model) {
    auto requested=model->numeric_graph();
    const auto window=std::find_if(requested.nodes.begin(),requested.nodes.end(),[](const auto& node){return node.id==2&&node.kind=="Window";});
    const auto fourier=std::find_if(requested.nodes.begin(),requested.nodes.end(),[](const auto& node){return node.id==4&&node.kind=="RealFourier";});
    CHECK(window!=requested.nodes.end()&&fourier!=requested.nodes.end());
    first_run_fft_literal(*window,"size");first_run_fft_literal(*fourier,"fft_size");
    domain::NumericProcessingGraph evaluated,executable;
    CHECK(bool(domain::resolve_numeric_parameter_bindings(requested,numeric_editor::limits(),evaluated,executable,2)));
    CHECK(model->edit_nodes(evaluated.nodes));
    return 0;
}

static int first_run_window_resize(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window,auto& applied) {
const auto beforeWindowRevision=app.presenter().client().snapshot().graph_revision;
        if(const int failure=first_run_window_geometry(model);failure)return failure;
        const bool windowApplied=wait_until([&]{return app.presenter().client().snapshot().graph_revision>beforeWindowRevision && !app.presenter().graph_request_pending() && std::get<signal_contracts::NumericContract>(model->delegateModel<AudioNodeModel>(4)->numeric_node()->outputs[0].contract.view()).shape.extents[0].maximum==1024;},5000);
        if(!windowApplied){if(const int failure=report_first_window_failure(app,model,applied,beforeWindowRevision);failure)return failure;}
        CHECK(windowApplied);
        return first_run_spectrum(app,model,graph,window);
}

static int first_run_window(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window,AudioNodeModel*& resamplerModel) {
        const auto priorRevision=app.presenter().client().snapshot().graph_revision;
        CHECK(resamplerModel->parameter_model()->editLiteral("output_rate","32000",0));
        CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>priorRevision && !app.presenter().graph_request_pending();},5000));
        const auto applied=app.presenter().applied_numeric_graph();CHECK(applied);
        for(const auto& node:applied->nodes)if(node.id==332){if(const int failure=validate_first_resampler_node(node);failure)return failure;}
        for(const auto& node:applied->nodes)CHECK(node.kind!="UInt32Constant" && node.kind!="BarkWeighting" && node.kind!="Mel" && node.kind!="ComplexPolar");
        const auto step=std::find_if(applied->nodes.begin(),applied->nodes.end(),[](const auto& n){return n.kind=="FrequencyStep";});CHECK(step!=applied->nodes.end());
        CHECK(std::get<signal_contracts::NumericContract>(step->outputs[0].contract.view()).dtype==signal_contracts::DType::Float64);
        CHECK(std::any_of(applied->edges.begin(),applied->edges.end(),[&](const auto& edge){return edge.from==step->id && edge.input_port==domain::frequency_step_input;}));
        return first_run_window_resize(app,model,graph,window,applied);
}

static void observe_rate_packet(auto& frame,auto& observed) {

                if(!frame.frame || !frame.frame.view().stamp.available)return;const auto view=frame.frame.view();const auto* values=std::get_if<signal_contracts::NumericView>(&view.payload);if(!values)return;
                std::lock_guard lock(observed->mutex);if(observed->latest.delivery.graph_revision!=frame.delivery.graph_revision){observed->count=0;observed->sequence=0;observed->positive=observed->invalid=false;}
                observed->invalid|=view.stamp.sequence<=observed->sequence;observed->sequence=view.stamp.sequence;
                for(std::size_t i=0;i<values->actual[0];++i){float x=0;observed->invalid|=signal_contracts::load_element(*values,{0,i,0},x)!=signal_contracts::Status::ok || !std::isfinite(x);observed->positive|=std::abs(x)>1e-6f;}
                observed->latest=frame;++observed->count;
            }

static int first_run_rate_delivery(auto& rate,GraphPanel*& frequencyPanel,auto& observed,auto& revision) {
CHECK(wait_until([&]{auto* chart=frequencyPanel->active_chart();return chart && chart->value_count()>0 && std::abs(chart->projected_x_coordinate(0)-double(rate)/4096)<1e-9 && std::abs(chart->projected_x_coordinate(1)-2.*double(rate)/4096)<1e-9;},3000));
            CHECK(wait_until([&]{std::lock_guard lock(observed->mutex);return observed->latest.delivery.graph_revision>revision && observed->count>=3;},3000));
            {std::lock_guard lock(observed->mutex);CHECK(!observed->invalid && observed->positive);CHECK(std::get<signal_contracts::NumericView>(observed->latest.frame.view().payload).contract->clock.sample_rate==rate);}

return 0;
}

static int first_run_rate_contract(CoreApplication& app,auto& rate,GraphPanel*& frequencyPanel,auto& observed,auto& revision) {
const auto appliedRate=app.presenter().applied_numeric_graph();CHECK(appliedRate);
            const auto actual=std::find_if(appliedRate->nodes.begin(),appliedRate->nodes.end(),[](const auto& n){return n.id==332;});CHECK(actual!=appliedRate->nodes.end());
            CHECK(std::get<signal_contracts::NumericContract>(actual->outputs[0].contract.view()).clock.sample_rate==rate);
            CHECK(std::none_of(appliedRate->nodes.begin(),appliedRate->nodes.end(),[](const auto& n){return n.kind=="SpectrumAxis";}));
            CHECK(std::any_of(appliedRate->edges.begin(),appliedRate->edges.end(),[](const auto& edge){return edge.to==10 && edge.input_port==domain::frequency_step_input;}));
            return first_run_rate_delivery(rate,frequencyPanel,observed,revision);
}

static int first_run_rate(CoreApplication& app,auto& rate,GraphPanel*& frequencyPanel,AudioNodeModel*& resamplerModel) {

            struct RateFrames{std::mutex mutex;pipeline::DeliveredNumericFrame latest;unsigned count=0;std::uint64_t sequence=0;bool positive=false,invalid=false;};
            const auto observed=std::make_shared<RateFrames>();
            const auto token=app.pipeline().subscribe_numeric_tap({{"main",332,domain::PortDirection::Output,0}},[observed](const pipeline::DeliveredNumericFrame& frame){ return observe_rate_packet(frame,observed); },{});CHECK(token);
            struct TapGuard{pipeline::NumericEngine& engine;std::uint64_t token;~TapGuard(){engine.unsubscribe_numeric_tap(token);}} guard{app.pipeline(),token};
            auto* rateEdit=resamplerModel->embeddedWidget()->findChild<QLineEdit*>("output_rate");CHECK(rateEdit);
            const auto revision=app.presenter().client().snapshot().graph_revision;
            rateEdit->setText(QString::number(rate));rateEdit->setModified(true);QMetaObject::invokeMethod(rateEdit,"editingFinished",Qt::DirectConnection);
            CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>revision && !app.presenter().graph_request_pending();},5000));
            return first_run_rate_contract(app,rate,frequencyPanel,observed,revision);
}

static int first_run_resampling(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window) {
        auto* frequencyDock=fixture_primary_dock(window,10);CHECK(frequencyDock);frequencyDock->toggleView(true);frequencyDock->raise();
        auto* frequencyPanel=qobject_cast<GraphPanel*>(frequencyDock->widget());CHECK(frequencyPanel);
        auto* resamplerModel=model->delegateModel<AudioNodeModel>(332);CHECK(resamplerModel);
        for(const auto rate:{36000u,48000u}){if(const int failure=first_run_rate(app,rate,frequencyPanel,resamplerModel);failure)return failure;}
        return first_run_window(app,model,graph,window,resamplerModel);
}

static void observe_first_source(auto& delivered,auto& pcm_frames,auto& nonzero) {

            if(!delivered.context||delivered.context->endpoint.node!=1||!delivered.frame)return;
            const auto frame=delivered.frame.view();const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload);if(!numeric||!frame.stamp.available)return;
            float value=0;if(signal_contracts::load_element(*numeric,{0,100,0},value)==signal_contracts::Status::ok){++pcm_frames;nonzero|=std::abs(value)>1e-6f;}
        }

static int first_run_live_delivery(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,MainWindow*& window,auto& accepted,auto& rejected,auto& pcm_frames,auto& nonzero) {
const bool live=wait_until([&]{return rejected||(accepted && pcm_frames>=3 && nonzero);},4000);
        if(!live)std::fprintf(stderr,"first-run live: accepted=%d rejected=%d frames=%d nonzero=%d revision=%llu pending=%d status=%s\n",int(accepted),int(rejected),pcm_frames,int(nonzero),static_cast<unsigned long long>(app.presenter().client().snapshot().graph_revision),int(app.presenter().graph_request_pending()),qPrintable(app.presenter().apply_status()));
        CHECK(live);CHECK(accepted && !rejected && pcm_frames>=3 && nonzero);
        CHECK(wait_until([&]{return startup_primary_views_open(window);}));
        return first_run_resampling(app,model,graph,window);
}

static int first_run_delivery(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph) {
        MainWindow* window=nullptr;for(const auto items_22=QApplication::topLevelWidgets();auto* candidate:items_22)if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
        CHECK(window);window->show();
        bool accepted=false,rejected=false;int pcm_frames=0;bool nonzero=false;
        QObject applyWitness;
        QObject::connect(&app.presenter(),&GuiPresenter::graph_apply_finished,&applyWitness,[&](quint64,bool ok,const QString& error){accepted|=ok;rejected|=!ok;if(!ok)std::fprintf(stderr,"first-run Apply: %s\n",qPrintable(error));});
        const auto observeSource=[&](const pipeline::DeliveredNumericFrame& delivered){return observe_first_source(delivered,pcm_frames,nonzero);};
        QObject sourceWitness;
        const auto initialSourceTap=app.pipeline().subscribe_numeric_tap({{"main",1,domain::PortDirection::Output,0}},[&](pipeline::DeliveredNumericFrame delivered){QMetaObject::invokeMethod(&sourceWitness,[&,delivered]{observeSource(delivered);},Qt::QueuedConnection);},{});
        CHECK(initialSourceTap);
        struct InitialSourceGuard{pipeline::NumericEngine& engine;std::uint64_t token;~InitialSourceGuard(){engine.unsubscribe_numeric_tap(token);}} initialSourceGuard{app.pipeline(),initialSourceTap};
        CHECK(wait_until([&]{QSet<QPair<int,int>> positions;for(const auto items_23=model->allNodeIds();auto id:items_23){const auto p=model->nodeData(id,QtNodes::NodeRole::Position).toPointF();positions.insert({qRound(p.x()),qRound(p.y())});}return positions.size()==numeric_startup_graph().nodes.size();}));
        return first_run_live_delivery(app,model,graph,window,accepted,rejected,pcm_frames,nonzero);
}

static int first_run_tones_and_ui(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,auto& generator) {
const auto& tones=std::get<domain::SignalGeneratorProcessingParameters>(generator->parameters).oscillators;
        CHECK(tones.size()==8 && tones[0].frequency_hz==73.25 && tones[0].amplitude==0.045);
        CHECK(numeric_default_graph().nodes[0].kind=="AudioSource");
        CHECK(app._ctx.file_configs.ui.pipeline_presets.size()==1);
        CHECK(app._ctx.file_configs.ui.pipeline_presets[0].toObject()["id"]=="builtin-test-signal");
        CHECK(app.presenter().file_protection_warning().isEmpty() && app.presenter().client().snapshot().graph_revision==0);
        return first_run_delivery(app,model,graph);
}

static int first_run_graph_contracts(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,auto& clock) {
        CHECK(clock!=graph.nodes.end() && clock->kind=="SampleClock");
        CHECK(std::get<domain::SampleClockProcessingParameters>(clock->parameters).sample_rate==48000);
        const auto selector=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.kind=="InputSelector";});CHECK(selector!=graph.nodes.end() && selector->inputs.size()==6 && domain::data_outputs(*selector).size()==2);
        const auto generator=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& n){return n.id==1;});CHECK(generator!=graph.nodes.end()&&generator->kind=="SignalGenerator");
        CHECK(domain::has_configuration_binding(graph,1,domain::ConfigurationRole::SampleClock));
        domain::NumericProcessingGraph resolved;CHECK(domain::resolve_configuration_bindings(graph,numeric_editor::limits(),resolved));
        const auto resolved_generator=std::find_if(resolved.nodes.begin(),resolved.nodes.end(),[](const auto& n){return n.id==1;});CHECK(resolved_generator!=resolved.nodes.end()&&resolved_generator->clock_node==16);
        return first_run_tones_and_ui(app,model,graph,generator);
}

static int install_first_font(auto& fontFile) {
const auto id=QFontDatabase::addApplicationFont(fontFile);if(id>=0){const auto families=QFontDatabase::applicationFontFamilies(id);if(!families.isEmpty())QApplication::setFont(QFont(families.front(),9));}
return 0;
}

static int first_run_native_source(const auto& graph,domain::NodeId id) {
            const auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& n){return n.id==id;});
            CHECK(source!=graph.nodes.end()&&domain::data_outputs(*source).size()==2);
            for(const auto& port:domain::data_outputs(*source)) {
                const auto pcm=std::get<signal_contracts::NumericContract>(port.contract.view());
                CHECK(port.id<2 && pcm.shape.rank==signal_contracts::Rank::Vector && pcm.channel_order.size()==1);
            }
            const auto ratePort=std::find_if(source->outputs.begin(),source->outputs.end(),[](const auto& port){return port.id==domain::sample_rate_output;});
            CHECK(ratePort!=source->outputs.end());
            const auto rate=std::get<signal_contracts::NumericContract>(ratePort->contract.view());
            CHECK(rate.dtype==signal_contracts::DType::UInt32 && rate.shape.rank==signal_contracts::Rank::Scalar);
            CHECK(std::none_of(graph.edges.begin(),graph.edges.end(),[=](const auto& e){return e.from==id&&e.output_port!=0&&e.output_port!=domain::sample_rate_output;}));
    return 0;
}

static int first_run_source_contracts(CoreApplication& app,NumericEditorGraphModel*& model,auto& graph,auto& clock) {
for(auto id:{domain::NodeId{13},domain::NodeId{14}}) {
            if(const int failure=first_run_native_source(graph,id);failure)return failure;
        }
        for(auto id:{domain::NodeId{2}}){
            const auto buffer=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& n){return n.id==id;});CHECK(buffer!=graph.nodes.end());
            const auto output=std::get<signal_contracts::NumericContract>(buffer->outputs[0].contract.view());
            CHECK(output.shape.extents[0].maximum==(id==2?4096:512));
        }
        return first_run_graph_contracts(app,model,graph,clock);
}

static int scenario_first_run(const QString& path) {
        CHECK(!QFile::exists(path));auto encoded=path.toLocal8Bit();char executable[]="numeric_application_tests",option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        const auto fontFile=qEnvironmentVariable("NUMERIC_TEST_FONT_FILE");
        if(!fontFile.isEmpty()){if(const int failure=install_first_font(fontFile);failure)return failure;}
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
        return first_run_source_contracts(app,model,graph,clock);
}

static int scenario_reload_project(char** argv) {
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

static int rejected_cancel_and_reload(auto& path,auto& next,CoreApplication& app,MainWindow*& diagnosticWindow,auto& replaced) {
const auto nextBeforeCancel=read(next);CHECK(save_without_overwrite([&]{app.save_configuration_as(next);}));CHECK(read(path)==replaced && read(next)==nextBeforeCancel);
        CHECK(app.presenter().file_protection_warning().isEmpty());
        desktop::NumericProjectDocument recovered;domain::NumericProcessingGraph recovered_graph;
        CHECK(desktop::load_numeric_project(next,recovered,recovered_graph));CHECK(std::any_of(recovered_graph.nodes.begin(),recovered_graph.nodes.end(),[](const auto& n){return n.id==1&&n.kind=="SignalGenerator";}));
        CHECK(save_without_overwrite([&]{emit diagnosticWindow->on_config_updated();}));CHECK(read(path)==replaced);
        std::puts("numeric rejected startup PASS"); return 0;
    }

static int rejected_replacement(auto& path,auto& next,auto& rejected,CoreApplication& app,MainWindow*& diagnosticWindow) {
const auto replaced=read(path);CHECK(replaced!=rejected && !replaced.isEmpty());
        CHECK(app.presenter().file_protection_warning().isEmpty());
        desktop::NumericProjectDocument replacement;domain::NumericProcessingGraph replacementGraph;CHECK(desktop::load_numeric_project(path,replacement,replacementGraph));
        app.save_configuration_as(next);CHECK(QFile::exists(next)&&read(path)==replaced);
        return rejected_cancel_and_reload(path,next,app,diagnosticWindow,replaced);
}

static int rejected_recovery(auto& path,auto& next,auto& rejected,CoreApplication& app,MainWindow*& diagnosticWindow,auto& saveAction,SchemeEditorWidget*& diagnosticEditor) {
        // An out-of-range native rate deterministically fails execution export,
        // while persistence retains the last valid stored contracts.
        unsigned discoveryCalls=0;
        auto* saveModel=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(saveModel);
        saveModel->set_capture_device_provider([&discoveryCalls](int,QString*) {
            ++discoveryCalls;
            return QJsonArray{QJsonObject{{"id","synthetic-capture"},{"name","Synthetic capture"},{"is_default",true},{"native_sample_rate",1},{"native_channels",1}}};
        });
        discoveryCalls=0; // Provider installation itself synchronizes device formats.
        domain::NumericProcessingGraph storageDraft;QString storageError;
        CHECK(diagnosticEditor->numeric_draft(storageDraft,&storageError,false));CHECK(discoveryCalls==0);
        CHECK(!diagnosticEditor->numeric_draft(storageDraft,&storageError));CHECK(discoveryCalls>0);
        // UI diagnostics may query devices while the modal runs. Saving must
        // nevertheless succeed with the valid stored contracts, despite that error.
        CHECK(respond_to_overwrite(true,[&]{trigger_save_action(diagnosticWindow,saveAction);}));
        install_synthetic_capture_discovery(app);
        return rejected_replacement(path,next,rejected,app,diagnosticWindow);
}

static int rejected_example_recovery(const QString& path,const QString& next,CoreApplication& app,auto& rejected,MainWindow*& diagnosticWindow,auto& saveAction,SchemeEditorWidget*& diagnosticEditor) {
auto* example=diagnosticWindow->findChild<QAction*>("example-startup");CHECK(example);example->trigger();
        CHECK(wait_until([&]{return !app.presenter().graph_request_pending()&&app.presenter().client().snapshot().graph_revision>0;}));
        CHECK(respond_to_overwrite(false,[&]{app.save_configuration_as(path);}));CHECK(read(path)==rejected);
#ifdef Q_OS_WIN
        CHECK(respond_to_overwrite(false,[&]{app.save_configuration_as(path.toUpper());}));CHECK(read(path)==rejected);
        CHECK(!app.presenter().file_protection_warning().isEmpty());
#endif
        return rejected_recovery(path,next,rejected,app,diagnosticWindow,saveAction,diagnosticEditor);
}

static int rejected_warning_details(const QString& path,const QString& next,CoreApplication& app,auto& rejected,MainWindow*& diagnosticWindow,auto& saveAction) {
auto* diagnosticEditor=diagnosticWindow->findChild<SchemeEditorWidget*>();CHECK(diagnosticEditor);
        auto* warning=diagnosticEditor->toolbar()->findChild<QLabel*>("pipelineValidation");
        CHECK(warning && !warning->isHidden() && warning->toolTip().contains(app.presenter().file_protection_warning()));
        CHECK(!diagnosticWindow->findChild<QLabel*>("savedPipelineWarning") && !diagnosticWindow->findChild<QLabel*>("graphApplyStatus"));
        diagnosticWindow->statusBar()->clearMessage();
        CHECK(!warning->isHidden() && !warning->toolTip().isEmpty());
        app.save_configuration_as(next);
        CHECK(!QFile::exists(next) && read(path)==rejected);
        CHECK(diagnosticWindow->findChild<QLabel*>("configurationSaveStatus")->property("saveState").toInt()==int(MainWindow::ConfigurationSaveState::LoadError));
        return rejected_example_recovery(path,next,app,rejected,diagnosticWindow,saveAction,diagnosticEditor);
}

static int rejected_save_warning(const QString& path,const QString& next,CoreApplication& app,auto& rejected,MainWindow*& diagnosticWindow) {
auto* saveAction=diagnosticWindow->findChild<QAction*>("saveSettings");CHECK(saveAction);
        CHECK(saveAction->shortcut()==QKeySequence("Ctrl+S") && saveAction->shortcutContext()==Qt::ApplicationShortcut);
        // A rejected draft must not swallow the explicit save warning.
        CHECK(respond_to_overwrite(false,[&]{trigger_save_action(diagnosticWindow,saveAction);},path));CHECK(read(path)==rejected);
        CHECK(respond_to_overwrite(true,[&]{trigger_save_action(diagnosticWindow,saveAction);},path));CHECK(read(path)==rejected);

        return rejected_warning_details(path,next,app,rejected,diagnosticWindow,saveAction);
}

static int scenario_reject_project(const QString& path,const QString& next) {
        const QByteArray rejected=R"({"schema_version":4,"scene_graph":{"version":5,"processing_version":4,"nodes":"invalid","connections":[]}})";
        QFile file(path); CHECK(file.open(QIODevice::WriteOnly)); file.write(rejected); file.close();
        auto encoded=path.toLocal8Bit(); char executable[]="numeric_application_tests", option[]="--config";
        char* arguments[]={executable,option,encoded.data(),nullptr}; int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        install_synthetic_capture_discovery(app);
        CHECK(app._ctx.data_flow_graph->allNodeIds().empty());
        CHECK(app.presenter().client().snapshot().graph_revision==0);
        CHECK(!app.presenter().file_protection_warning().isEmpty());
        MainWindow* diagnosticWindow=nullptr;for(const auto items_24=QApplication::topLevelWidgets();auto* widget:items_24)if(auto* candidate=qobject_cast<MainWindow*>(widget))diagnosticWindow=candidate;
        CHECK(diagnosticWindow);
        return rejected_save_warning(path,next,app,rejected,diagnosticWindow);
}

static int default_floating_save(auto& next,auto& document,auto& graph,auto& error,MainWindow*& window,SchemeEditorWidget*& editor,auto& saveAction) {
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

static int default_explicit_save(auto& next,auto& document,auto& graph,auto& error,MainWindow*& window,SchemeEditorWidget*& editor) {
auto* saveAction=window->findChild<QAction*>("saveSettings");CHECK(saveAction);
    CHECK(saveAction->shortcut()==QKeySequence("Ctrl+S"));
    CHECK(QFile::remove(next)); // Only this test's temporary destination.
    CHECK(save_without_overwrite([&]{trigger_save_action(window,saveAction);}));
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error) && graph.nodes.empty());
        return default_floating_save(next,document,graph,error,window,editor,saveAction);
}

static int default_overwrite_confirmation(auto& next,auto& document,auto& graph,auto& error,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor,auto& startup_revision) {
app._ctx.editor_draft_modified=true; CHECK(respond_to_overwrite(false,[&]{emit window->on_config_updated();}));
    CHECK(read(next)=="{\"schema_version\":100}"); CHECK(app._ctx.editor_draft_modified);
    CHECK(app.presenter().client().snapshot().graph_revision==startup_revision);
    CHECK(respond_to_overwrite(true,[&]{emit window->on_config_updated();}));
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error) && graph.nodes.empty());
    return default_explicit_save(next,document,graph,error,window,editor);
}

static int default_external_overwrite(auto& next,auto& document,auto& graph,auto& error,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor,auto& startup_revision) {
    graph.nodes.clear(); CHECK(editor->load_numeric_draft(graph,&error));
    CHECK(save_without_overwrite([&]{emit window->on_config_updated();}));
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error)); CHECK(graph.nodes.empty());
    QFile external(next); CHECK(external.open(QIODevice::WriteOnly)); external.write("{\"schema_version\":100}"); external.close();
    return default_overwrite_confirmation(next,document,graph,error,app,window,editor,startup_revision);
}

static int default_restart_process(auto& next,auto& document,auto& graph,auto& error,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor,auto& startup_revision) {
const auto before_restart=read(next);
    QProcess restart;
    restart.start(QCoreApplication::applicationFilePath(),{"--reload",next});
    CHECK(restart.waitForFinished(6000));
    if(restart.exitCode()!=0)std::fprintf(stderr,"restart: %s\n",restart.readAllStandardError().constData());
    CHECK(restart.exitStatus()==QProcess::NormalExit && restart.exitCode()==0);
    CHECK(read(next)==before_restart);
    CHECK(app.presenter().client().snapshot().graph_revision==startup_revision);
        return default_external_overwrite(next,document,graph,error,app,window,editor,startup_revision);
}

static int default_restart(auto& next,auto& document,auto& graph,auto& error,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor,auto& settings,auto& startup_revision) {
    app.save_configuration_as(next);
    CHECK(ConfigManager(next).loadNumeric(document,graph,&error));
    CHECK(exact_policy(graph));
    std::optional<domain::NumericProcessingGraph> applied;
    CHECK(desktop::read_numeric_applied(document,applied));CHECK(applied && exact_policy(*applied));
    CHECK(read(next).contains("1e9999") && read(next).contains("\"extension\":7"));
    CHECK(ConfigManager(next).readNumericSettings(document,settings,&error));
    CHECK(settings.ui.pipeline_presets.size()==2);
    return default_restart_process(next,document,graph,error,app,window,editor,startup_revision);
}

static int default_repeated_preset(auto& next,auto& document,auto& graph,auto& manager,auto& error,CoreApplication& app,MainWindow*& window,SchemeEditorWidget*& editor,auto& settings,auto& startup_revision) {
CHECK(editor->save_preset("repeat"));
    CHECK(wait_for_saved(window));
    CHECK(manager.loadNumeric(document,graph,&error));
    CHECK(manager.readNumericSettings(document,settings,&error));
    CHECK(settings.ui.pipeline_presets.size()==3);
    QString repeat_id;
    for(const auto& item:std::as_const(settings.ui.pipeline_presets))if(item.toObject()["name"].toString()=="repeat")repeat_id=item.toObject()["id"].toString();
    CHECK(!repeat_id.isEmpty() && editor->load_preset(repeat_id));
    CHECK(editor->remove_preset(repeat_id));
        return default_restart(next,document,graph,error,app,window,editor,settings,startup_revision);
}

static int default_load_presets(auto& next,auto& document,auto& graph,auto& manager,auto& error,auto& presentation,CoreApplication& app,NumericEditorGraphModel*& model,MainWindow*& window,SchemeEditorWidget*& editor,auto& settings,auto& startup_revision) {
    QString first_id;
    for(const auto& item:std::as_const(settings.ui.pipeline_presets))if(item.toObject()["name"].toString()=="first")first_id=item.toObject()["id"].toString();
    CHECK(!first_id.isEmpty() && editor->load_preset(first_id));
    CHECK(editor->numeric_draft(graph,&error)); CHECK(graph.nodes.size()==1);
    CHECK(exact_policy(graph));
    CHECK(model->delegateModel<AudioNodeModel>(7)->presentation()["inline_parameters"].toArray()==QJsonArray{"enabled"});
    CHECK(std::get<std::uint64_t>(std::get<domain::ConverterProcessingParameters>(graph.nodes[0].parameters).policy.scaling.gain.numerator)==std::numeric_limits<std::uint64_t>::max());
    CHECK(editor->remove_preset(first_id));
    return default_repeated_preset(next,document,graph,manager,error,app,window,editor,settings,startup_revision);
}

static int default_named_presets(auto& path,auto& next,auto& document,auto& graph,auto& manager,auto& error,auto& presentation,CoreApplication& app,NumericEditorGraphModel*& model,MainWindow*& window,SchemeEditorWidget*& editor,auto& startup_revision,auto& settings) {
CHECK(editor->save_preset("first"));
    CHECK(wait_for_saved(window));
    CHECK(manager.loadNumeric(document,graph,&error));
    CHECK(manager.readNumericSettings(document,settings,&error));
    CHECK(settings.ui.pipeline_presets.size()==2);
    const auto first_bytes=read(path);
    CHECK(editor->save_preset("second"));
    CHECK(wait_for_saved(window));
    CHECK(read(path)!=first_bytes);
    CHECK(manager.loadNumeric(document,graph,&error));
    CHECK(manager.readNumericSettings(document,settings,&error));
    CHECK(settings.ui.pipeline_presets.size()==3);
    app._ctx.file_configs.ui.pipeline_presets=settings.ui.pipeline_presets;
        return default_load_presets(next,document,graph,manager,error,presentation,app,model,window,editor,settings,startup_revision);
}

static int default_save_preferences(auto& path,auto& next,auto& document,auto& graph,auto& manager,auto& error,auto& presentation,CoreApplication& app,NumericEditorGraphModel*& model,MainWindow*& window,SchemeEditorWidget*& editor,auto& startup_revision) {
    app._ctx.file_configs.ui.language="en";
    app._ctx.file_configs.ui.camera=QJsonObject{{"x",42}};
    CHECK(save_without_overwrite([&]{emit window->on_config_updated();})); // Normal Ctrl+S has no warning.
    CHECK(manager.loadNumeric(document,graph,&error));
    FileSaveApplicationConfig settings;
    CHECK(manager.readNumericSettings(document,settings,&error));
    CHECK(settings.ui.language=="en" && settings.ui.camera["x"].toDouble()==42);
    // Actual editor operations reconcile existing preset identities and opaque
    // source while preserving exact typed node snapshots.
    return default_named_presets(path,next,document,graph,manager,error,presentation,app,model,window,editor,startup_revision,settings);
}

static int default_autosave(auto& path,auto& next,auto& document,auto& graph,auto& manager,auto& error,auto& presentation,CoreApplication& app,NumericEditorGraphModel*& model,MainWindow*& window,SchemeEditorWidget*& editor,auto& startup_revision) {
    graph.nodes[0].enabled=false;
    CHECK(editor->load_numeric_draft(graph,&error));
    // Model keeps unknown visual/preset fields; the application must save only
    // registered flags and let storage retain the original raw scene metadata.
    model->delegateModel<AudioNodeModel>(7)->set_presentation(QJsonObject{{"inline_parameters",QJsonArray{"enabled"}},{"future_visual_flag",9}});
    model->setNodeData(7,QtNodes::NodeRole::Position,QPointF(99,88));
    app._ctx.editor_draft_modified=true;
    emit editor->numeric_draft_changed(); // Actual autosave callback, not execution.
    CHECK(wait_for_saved(window));
    CHECK(manager.loadNumeric(document,graph,&error)); CHECK(!graph.nodes[0].enabled);
    CHECK(read(path).contains("1e9999") && read(path).contains("18446744073709551615"));
    CHECK(desktop::read_numeric_presentation(document,presentation));
    CHECK(presentation.positions["7"].toObject()["x"].toDouble()==99);
    CHECK(presentation.node_presentations["7"].toObject()["inline_parameters"].toArray()==QJsonArray{"enabled"});
    CHECK(read(path).contains("\"opaque_flags\":9007199254740993"));
        return default_save_preferences(path,next,document,graph,manager,error,presentation,app,model,window,editor,startup_revision);
}

static int default_restored_state(auto& path,auto& next,auto& document,auto& graph,auto& manager,auto& error,auto& presentation,CoreApplication& app,NumericEditorGraphModel*& model,MainWindow*& window,SchemeEditorWidget*& editor) {
CHECK(model->nodeData(7,QtNodes::NodeRole::Position).toPointF()==QPointF(321,654));
    CHECK(model->delegateModel<AudioNodeModel>(7)->presentation()["inline_parameters"].toArray()==QJsonArray{"enabled"});
    CHECK(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();}));const auto startup_revision=app.presenter().client().snapshot().graph_revision;
    const auto saved_layout_bytes=read(path);QCoreApplication::processEvents();
    CHECK(model->nodeData(7,QtNodes::NodeRole::Position).toPointF()==QPointF(321,654));CHECK(read(path)==saved_layout_bytes);
    CHECK(app.presenter().client().snapshot().graph_revision==startup_revision);
    CHECK(editor->numeric_draft(graph,&error));
    CHECK(exact_policy(graph));
    CHECK(std::get<std::uint64_t>(std::get<domain::ConverterProcessingParameters>(graph.nodes[0].parameters).policy.scaling.gain.numerator)==std::numeric_limits<std::uint64_t>::max());
        return default_autosave(path,next,document,graph,manager,error,presentation,app,model,window,editor,startup_revision);
}

static int default_application(auto& path,auto& next,auto& document,auto& graph,auto& manager,auto& error,auto& presentation) {
    auto encoded=path.toLocal8Bit(); char executable[]="numeric_application_tests", option[]="--config";
    char* arguments[]={executable,option,encoded.data(),nullptr}; int count=3;
    CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
    install_synthetic_capture_discovery(app);
    auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());
    CHECK(model && model->has_numeric_nodes() && model->nodeExists(7));
    CHECK(!app._ctx.file_configs.ui.show_spectrogram);
    MainWindow* window=nullptr;
    // The composition owns the window; the model deliberately has no QWidget parent.
    for(const auto items_25=QApplication::topLevelWidgets();auto* candidate:items_25)if(auto* found=qobject_cast<MainWindow*>(candidate))window=found;
    CHECK(window);
    auto* editor=window->findChild<SchemeEditorWidget*>(); CHECK(editor);
    return default_restored_state(path,next,document,graph,manager,error,presentation,app,model,window,editor);
}

static int scenario_default(const QString& path,const QString& next) {
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
        return default_application(path,next,document,graph,manager,error,presentation);
}

static bool scenario_mode(int argc,char** argv,int count,std::initializer_list<const char*> modes) {
    if(argc<2||(count&&argc!=count))return false;
    return std::any_of(modes.begin(),modes.end(),[&](const char* value){return QByteArray(argv[1])==value;});
}

int main(int argc, char** argv) {
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto path=directory.filePath("project.json"), next=directory.filePath("saved.json");
    if(scenario_mode(argc,argv,3,{"--save-file-copy","--fft-file-copy","--resampler-file-copy","--music-file-copy"}))return scenario_file_copy(argv,path);
    if(scenario_mode(argc,argv,2,{"--workspace-persistence"}))return scenario_workspace_persistence(argc,argv,path,next);
    if(scenario_mode(argc,argv,4,{"--workspace-phase"}))return scenario_workspace_phase(argv);
    if(scenario_mode(argc,argv,2,{"--matrix-center-ring-bypass"}))return scenario_matrix_bypass(path);
    if(scenario_mode(argc,argv,3,{"--export-temporal-project"}))return scenario_export_temporal(argv,path);
    if(scenario_mode(argc,argv,0,{"--temporal-fft","--temporal-fft-44100"}))return scenario_temporal_fft(argv,path,next);
    if(scenario_mode(argc,argv,3,{"--export-current-project"}))return scenario_export_current(argv);
    if(scenario_mode(argc,argv,0,{"--current-examples"}))return scenario_current_examples(path,next);
    if(scenario_mode(argc,argv,3,{"--verify-project"}))return scenario_verify_project(argv,path);
    if(scenario_mode(argc,argv,0,{"--startup-menu-empty","--startup-menu-old"}))return scenario_startup_menu(argv,path);
    if(scenario_mode(argc,argv,0,{"--startup-selector","--startup-selector-unavailable"}))return scenario_startup_selector(argv,path);
    if(scenario_mode(argc,argv,0,{"--shipped-examples","--shipped-examples-ui"}))return scenario_shipped_examples(argv,path);
    if(scenario_mode(argc,argv,0,{"--first-run"}))return scenario_first_run(path);
    if(scenario_mode(argc,argv,3,{"--reload"}))return scenario_reload_project(argv);
    if(scenario_mode(argc,argv,0,{"--rejected"}))return scenario_reject_project(path,next);
    return scenario_default(path,next);
}
