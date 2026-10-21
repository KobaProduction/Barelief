#include "numeric_gui_fixture.h"
#include <core_app/graph_panel.h>
#include <core_app/config_manager.h>
#include <barelief/charts/line.h>
#include <barelief/ui_text.h>
#include <QFile>
#include <QPointer>
#include <QStatusBar>
#include <QProcess>
#include <DockAreaWidget.h>
#include <FloatingDockContainer.h>
#include <cstdio>
static QString save_preset(gui_fixture::App& app,const QString& name){gui_fixture::check(app.editor->save_preset(name),"actual preset Save");return app.application->_ctx.file_configs.ui.pipeline_presets.last().toObject().value("id").toString();}
static QList<WorkspaceDock*> dynamic_docks(MainWindow* window){QList<WorkspaceDock*> result;for(const auto docks=window->workspaceDocks();auto* dock:docks)if(dock->objectName().startsWith("nodeView_"))result.append(dock);return result;}
static QRect area_rect(MainWindow* window,WorkspaceDock* dock){auto* area=dock->dockAreaWidget();gui_fixture::check(area,"dock area missing");return {area->mapTo(window,QPoint()),area->size()};}
static QStringList open_tabs(WorkspaceDock* dock){QStringList names;for(const auto peers=dock->dockAreaWidget()->dockWidgets();auto* peer:peers)if(!peer->isClosed())names.append(peer->objectName());names.sort();return names;}
static GraphPanel* fixed_plot(gui_fixture::App& app,const char* id){
    auto* dock=app.dock(QString::fromLatin1(id)+"Dock");
    auto* panel=dock?qobject_cast<GraphPanel*>(dock->widget()):nullptr;
    gui_fixture::check(panel,"fixed graph panel missing");return panel;
}
static void check_fixed_audio_plots(gui_fixture::App& app){
    for(const auto* id:{"fft","mel"}){
        auto* panel=fixed_plot(app,id);
        gui_fixture::check(panel->audio_scales() && panel->modes().contains("line"),"fixed audio descriptor missing");
        const auto prefs=app.application->_ctx.file_configs.ui.graph_views.value(QString::fromLatin1(id)).toObject();
        gui_fixture::check(prefs.value("plot_specialization")=="audio" && prefs.value("plot_modes").isArray(),"fixed descriptor not saved");
    }
}
static void make_fixed_preset_malformed(gui_fixture::App& app,const QString& id){
    auto& presets=app.application->_ctx.file_configs.ui.pipeline_presets;
    for(int i=0;i<presets.size();++i)if(presets[i].toObject().value("id")==id){
        auto preset=presets[i].toObject();auto snapshot=preset["presentation"].toObject();
        auto views=snapshot["graph_views"].toObject();auto fft=views["fft"].toObject();
        fft["plot_modes"]=QJsonArray{"unsupported-mode"};fft["plot_specialization"]="generic";
        fft["unknown_extension"]=17;views["fft"]=fft;snapshot["graph_views"]=views;
        preset["presentation"]=snapshot;presets[i]=preset;
    }
}
static void remove_fixed_descriptor_from_preset(gui_fixture::App& app,const QString& id){
    auto& presets=app.application->_ctx.file_configs.ui.pipeline_presets;
    for(int i=0;i<presets.size();++i)if(presets[i].toObject().value("id")==id){
        auto preset=presets[i].toObject();auto snapshot=preset["presentation"].toObject();
        auto views=snapshot["graph_views"].toObject();
        for(const auto* key:{"fft","mel"}){
            auto prefs=views[key].toObject();
            prefs.remove("plot_modes");prefs.remove("plot_category_path");prefs.remove("plot_specialization");
            views[key]=prefs;
        }
        snapshot["graph_views"]=views;preset["presentation"]=snapshot;presets[i]=preset;
    }
}
static void check_malformed_fixed_restore(gui_fixture::App& app){
    auto& views=app.application->_ctx.file_configs.ui.graph_views;
    gui_fixture::check(fixed_plot(app,"fft")->modes().contains("line") && !fixed_plot(app,"fft")->audio_scales(),
        "malformed fixed descriptor did not fall back safely");
    gui_fixture::check(fixed_plot(app,"mel")->audio_scales() && views["fft"].toObject().value("unknown_extension").toInt()==17 &&
        views["fft"].toObject().value("plot_modes").toArray().at(0)=="line", "fixed descriptor restore lost Mel audio, mode fallback, or unknown field");
}
struct Topology {QStringList tabs;QString active;double leftShare;QRect floating;};
static Topology topology(MainWindow* window,WorkspaceDock* first,WorkspaceDock* neighbor,WorkspaceDock* external){
    using namespace gui_fixture;check(first->dockAreaWidget()!=neighbor->dockAreaWidget(),"split areas merged");
    const auto left=area_rect(window,first),right=area_rect(window,neighbor);
    check(left.right()<right.left() && left.height()>0 && right.height()>0,"A split order is not left/right");
    check(external->isFloating() && !external->sharesHost(first),"A external host is not independent");
    auto* active=first->dockAreaWidget()->currentDockWidget();check(active,"active tab missing");
    return {open_tabs(first),active->objectName(),double(left.width())/(left.width()+right.width()),external->floatingDockContainer()->frameGeometry()};
}
static bool close_rect(const QRect& a,const QRect& b,int tolerance=12){return std::abs(a.x()-b.x())<=tolerance && std::abs(a.y()-b.y())<=tolerance && std::abs(a.width()-b.width())<=tolerance && std::abs(a.height()-b.height())<=tolerance;}
static void workspace_persistence(gui_fixture::App& app,const QString& several,const QJsonObject& severalPresentation,quint64 revision,const QString& firstName,const QString& firstKey) {
using namespace gui_fixture;auto& ui=app.application->_ctx.file_configs.ui;
        const auto before=app.editor->document();const auto beforeDocks=app.window->workspaceDocks().size();
        auto malformed=ui.pipeline_presets.last().toObject();malformed["id"]="invalid-presentation";auto invalid=malformed["presentation"].toObject();invalid["version"]=99;malformed["presentation"]=invalid;ui.pipeline_presets.append(malformed);
        check(!app.editor->load_preset("invalid-presentation")&&app.editor->document()==before&&app.window->workspaceDocks().size()==beforeDocks,"future presentation did not reject atomically");
        check(app.editor->remove_preset("invalid-presentation"),"remove malformed fixture");
        app.window->activateWindow(); app.window->setFocus();
        auto* saveAction=app.window->findChild<QAction*>("saveSettings");
        check(wait([&]{return saveAction && saveAction->isEnabled();}),"Save command in active workspace");
        saveAction->trigger();
        desktop::NumericProjectDocument source;domain::NumericProcessingGraph loaded;check(bool(desktop::load_numeric_project(QString::fromLocal8Bit(app.path),source,loaded)),"saved canonical source");
        QJsonObject settings;check(bool(desktop::read_numeric_application_settings(source,settings)),"reload registered settings");
        const auto savedPresets=settings["ui"].toObject().value("pipeline_presets").toArray();
        check(savedPresets.size()==4,"disk reload lost preset list");
        QJsonObject savedA;for(const auto& preset:savedPresets)if(preset.toObject().value("id")==several)savedA=preset.toObject().value("presentation").toObject();
        check(savedA==severalPresentation,"disk reload changed preset display snapshot");
        emit app.window->save_configuration_as_requested(app.directory.filePath("saved-as.json"));QFile saved(app.directory.filePath("saved-as.json"));check(saved.exists(),"Save As canonical project");
        const auto failedPath=app.directory.filePath("invalid.json");QFile failed(failedPath);check(failed.open(QIODevice::WriteOnly),"invalid fixture");failed.write("{ broken");failed.close();emit app.window->open_configuration_requested(failedPath);
        check(app.window->isVisible()&&!app.window->statusBar()->currentMessage().isEmpty(),"malformed Open changed window or omitted error");
        check(app.presenter().client().snapshot().graph_revision==revision,"save/open changed processing");
        auto* liveDock=app.window->workspaceDock(firstName);auto* livePanel=liveDock?qobject_cast<GraphPanel*>(liveDock->widget()):nullptr;
        check(livePanel,"live descriptor panel missing");
        auto livePrefs=ui.graph_views.value(firstKey).toObject();
        livePrefs["plot_modes"]=QJsonArray{"points"};livePrefs["plot_specialization"]="generic";
        livePrefs["minimum_x"]=120;livePrefs["maximum_x"]=480;livePrefs["reverse_x"]=false;
        ui.graph_views[firstKey]=livePrefs;app.apply();
        check(wait([&]{auto* chart=livePanel->active_chart();
            return liveDock->widget()==livePanel && livePanel->modes()==QStringList{"line"} && livePanel->audio_scales() &&
                chart && chart->x_range().first==120 && chart->x_range().second==480 && !chart->reverse_x();}),
            "fixed view descriptor changed or applicable saved range did not refresh live panel");
}

static void preset_matrix_restore(gui_fixture::App& app,const QString& zero,const QString& one,const QString& several,const QJsonObject& severalPresentation,const QString& firstName,const QString& externalName,const QString& neighborName,const QString& inactiveName,quint64 revision,const QString& theme,const QString& language) {
using namespace gui_fixture;auto& ui=app.application->_ctx.file_configs.ui;
        ui.graph_rendering={{"version",1},{"defaults",QJsonObject{{"brightness",1.8}}}};
        app.application->_ctx.data_flow_graph->setNodeData(1,QtNodes::NodeRole::Position,QPointF(777,888));
        make_fixed_preset_malformed(app,zero);
        remove_fixed_descriptor_from_preset(app,one);
        check(app.editor->load_preset(zero),"load zero");
        check_malformed_fixed_restore(app);
        check(wait([&]{for(const auto docks=app.window->workspaceDocks();auto* dock:docks)if(dock!=app.dock("dataFlowDock")&&!dock->isClosed())return false;return true;}),"zero preset retained graphs");
        check(app.editor->load_preset(one),"load one");auto* first=app.window->workspaceDock(firstName);check(first&&!first->isClosed(),"one preset lost representation");
        check_fixed_audio_plots(app);
        int open=0;for(const auto docks=app.window->workspaceDocks();auto* dock:docks)if(dock!=app.dock("dataFlowDock")&&!dock->isClosed())++open;
        check(open==1,"one preset inherited other open tabs");
        check(app.application->_ctx.data_flow_graph->nodeData(1,QtNodes::NodeRole::Position).toPointF()==QPointF(42,73),"preset coordinates inherited previous positions");
        auto* restoredPanel=qobject_cast<GraphPanel*>(first->widget());
        auto* chart=restoredPanel?restoredPanel->active_chart():nullptr;check(chart,"restored active chart");
        check(chart->x_range().first==100 && chart->x_range().second==500 && chart->reverse_x(),"preset manual axis restoration");
        check(app.editor->load_preset(several),"load multiple");
        auto* external=app.window->workspaceDock(externalName);auto* neighbor=app.window->workspaceDock(neighborName);auto* inactive=app.window->workspaceDock(inactiveName);
        check(external&&external->isFloating()&&!external->isClosed()&&neighbor&&!neighbor->isClosed()&&inactive&&!inactive->isClosed(),"multiple preset lost host/open state");
        check(ui.graph_rendering==severalPresentation["graph_rendering"].toObject(),"preset rendering inherited prior display");
        check(ui.theme==theme&&ui.language==language,"preset rewrote global preferences");
        check(app.presenter().client().snapshot().graph_revision==revision&&app.presenter().client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,"preset changed continuous working graph");
}

static void restore_split_topologies(gui_fixture::App& app,const QString& several,const QJsonObject& severalPresentation,const QString& firstName,const QString& neighborName,const QString& inactiveName,const QString& externalName,const Topology& aTopology,const QString& otherLayout,const QRect& bGeometry) {
using namespace gui_fixture;auto& ui=app.application->_ctx.file_configs.ui;
        const bool loadedA=app.editor->load_preset(several);
        if(!loadedA){
            QJsonObject validated;const auto result=desktop::decode_numeric_preset_presentation(QJsonDocument(severalPresentation).toJson(QJsonDocument::Compact),validated);
            domain::NumericProcessingGraph restoredGraph;desktop::NumericDocumentResult graphResult;
            for(const auto presets=ui.pipeline_presets;const auto& preset:presets)if(preset.toObject().value("id").toString()==several)
                graphResult=desktop::decode_numeric_editor_graph(QJsonDocument(preset.toObject().value("graph").toObject()).toJson(QJsonDocument::Compact),restoredGraph);
            std::fprintf(stderr,"Workspace preset refusal: pending=%d presentationValid=%d presentationError=%s graphValid=%d graphError=%s status=%s\n",app.presenter().graph_request_pending(),bool(result),qPrintable(result.error),bool(graphResult),qPrintable(graphResult.error),qPrintable(app.window->statusBar()->currentMessage()));
            if(!result)std::fprintf(stderr,"Workspace rejected presentation: %s\n",QJsonDocument(severalPresentation).toJson(QJsonDocument::Compact).constData());
        }
        check(loadedA,"A to B to A topology restore");
        auto* first=app.window->workspaceDock(firstName);auto* neighbor=app.window->workspaceDock(neighborName);auto* inactive=app.window->workspaceDock(inactiveName);auto* external=app.window->workspaceDock(externalName);
        const bool restoredA=wait([&]{return first && neighbor && inactive && external && first->dockAreaWidget()==inactive->dockAreaWidget() && first->dockAreaWidget()->currentDockWidget()==first && area_rect(app.window,first).right()<area_rect(app.window,neighbor).left();});
        if(!restoredA && first && neighbor && inactive)qWarning()<<"A restore diagnostic"<<first->objectName()<<"tabs"<<open_tabs(first)<<"active"<<first->dockAreaWidget()->currentDockWidget()->objectName()<<"same group"<<(first->dockAreaWidget()==inactive->dockAreaWidget())<<"left"<<area_rect(app.window,first)<<"right"<<area_rect(app.window,neighbor);
        check(restoredA,"restored A tab membership/selection/split order");
        const auto restoredTopology=topology(app.window,first,neighbor,external);
        check(restoredTopology.tabs==aTopology.tabs && restoredTopology.active==aTopology.active && std::abs(restoredTopology.leftShare-aTopology.leftShare)<.08 && close_rect(restoredTopology.floating,aTopology.floating),"restored A tab set/split proportion/float geometry");
        check(app.editor->load_preset(otherLayout),"load distinct B saved topology");
        first=app.window->workspaceDock(firstName);neighbor=app.window->workspaceDock(neighborName);inactive=app.window->workspaceDock(inactiveName);external=app.window->workspaceDock(externalName);
        check(wait([&]{return first->dockAreaWidget()->currentDockWidget()==inactive && area_rect(app.window,first).bottom()<area_rect(app.window,neighbor).top();}) && close_rect(external->floatingDockContainer()->frameGeometry(),bGeometry),"B saved topology restoration");
}

static WorkspaceDock* configured_fft_view(gui_fixture::App& app) {
using namespace gui_fixture;auto& ui=app.application->_ctx.file_configs.ui;
        auto* first=app.open(10,2,0);const auto firstName=first->objectName();const auto firstKey=firstName.mid(QStringLiteral("nodeView_").size());
        ui.graph_views[firstKey]=QJsonObject{{"device",app.presenter().device_id()},{"graph",app.presenter().graph_id()},{"node",10},{"kind",first->property("graphKind").toString()},{"instance",first->property("viewInstance").toInt()},{"open",true},{"auto_range_x",false},{"minimum_x",100},{"maximum_x",500},{"reverse_x",true},{"side_ratio",.3}};
        auto* panel=qobject_cast<GraphPanel*>(first->widget());check(panel,"FFT panel");panel->restore_presentation(ui.graph_views[firstKey].toObject());
        auto* model=app.application->_ctx.data_flow_graph.get();model->setNodeData(1,QtNodes::NodeRole::Position,QPointF(42,73));
return first;
}
static QByteArray initial_session(){
        using namespace gui_fixture;
        App app;app.apply();auto& ui=app.application->_ctx.file_configs.ui;
        check_fixed_audio_plots(app);
        const auto revision=app.presenter().client().snapshot().graph_revision;
        const auto theme=ui.theme;const auto language=ui.language;
        for(const auto docks=app.window->workspaceDocks();auto* dock:docks)if(dock!=app.dock("dataFlowDock"))dock->toggleView(false);
        const auto zero=save_preset(app,"zero views");
        auto* first=configured_fft_view(app);const auto firstName=first->objectName();const auto firstKey=firstName.mid(QStringLiteral("nodeView_").size());
        const auto one=save_preset(app,"one view");
        auto* external=app.open(10,0,0);auto* neighbor=app.open(11,2,0);auto* inactive=app.open(10,1,0);
        const auto externalName=external->objectName();const auto neighborName=neighbor->objectName();const auto inactiveName=inactive->objectName();
        app.window->tabifyDockWidget(first,inactive);app.window->placeDock(neighbor,first,Qt::RightDockWidgetArea);
        first->toggleView(true);inactive->toggleView(true);neighbor->toggleView(true);first->raise();
        app.window->resizeDocks({first,neighbor},{550,300},Qt::Horizontal);
        external->floatingDockContainer()->resize(430,310);external->floatingDockContainer()->move(80,90);
        check(wait([&]{return first->dockAreaWidget()==inactive->dockAreaWidget() && first->dockAreaWidget()->currentDockWidget()==first && area_rect(app.window,first).right()<area_rect(app.window,neighbor).left();}),"A topology precondition");
        const auto aTopology=topology(app.window,first,neighbor,external);
        const auto several=save_preset(app,"multiple internal external");
        const auto severalPresentation=ui.pipeline_presets.last().toObject().value("presentation").toObject();
        check(severalPresentation["workspace"].toObject().value("visibility").toObject()[inactiveName].toBool(),"inactive tab saved as closed");
        app.window->placeDock(neighbor,first,Qt::BottomDockWidgetArea);inactive->raise();
        external->floatingDockContainer()->resize(360,250);external->floatingDockContainer()->move(160,170);
        check(wait([&]{return first->dockAreaWidget()->currentDockWidget()==inactive && area_rect(app.window,first).bottom()<area_rect(app.window,neighbor).top();}),"distinct B topology precondition");
        const auto bGeometry=external->floatingDockContainer()->frameGeometry();check(!close_rect(aTopology.floating,bGeometry),"B floating geometry is not distinct");
        const auto otherLayout=save_preset(app,"vertical split with other active tab");
        restore_split_topologies(app,several,severalPresentation,firstName,neighborName,inactiveName,externalName,aTopology,otherLayout,bGeometry);
        preset_matrix_restore(app,zero,one,several,severalPresentation,firstName,externalName,neighborName,inactiveName,revision,theme,language);
        workspace_persistence(app,several,severalPresentation,revision,firstName,firstKey);
        QFile retained(QString::fromLocal8Bit(app.path));check(retained.open(QIODevice::ReadOnly),"restart source read");return retained.readAll();
}
int main(int argc,char** argv){
    try {
        if(argc==3 && QString::fromLocal8Bit(argv[1])=="--initial"){
            const auto stored=initial_session();QFile file(QString::fromLocal8Bit(argv[2]));
            gui_fixture::check(file.open(QIODevice::WriteOnly) && file.write(stored)==stored.size(),"write restart handoff");return 0;
        }
        if(argc!=3 || QString::fromLocal8Bit(argv[1])!="--restart"){
            QCoreApplication coordinator(argc,argv);QTemporaryDir directory;
            gui_fixture::check(directory.isValid(),"restart test directory");const auto path=directory.filePath("session.json");
            for(const auto* phase:{"--initial","--restart"}){
                QProcess process;process.setProcessChannelMode(QProcess::ForwardedChannels);
                process.start(QCoreApplication::applicationFilePath(),{QString::fromLatin1(phase),path});
                gui_fixture::check(process.waitForStarted(),"start isolated workspace phase");
                if(!process.waitForFinished(45000)){process.kill();process.waitForFinished();throw std::runtime_error("workspace phase timeout");}
                gui_fixture::check(process.exitStatus()==QProcess::NormalExit && process.exitCode()==0,"isolated workspace phase failed");
            }
            return 0;
        }
        QFile file(QString::fromLocal8Bit(argv[2]));gui_fixture::check(file.open(QIODevice::ReadOnly),"read restart handoff");const auto stored=file.readAll();file.close();
        gui_fixture::App restarted(numeric_default_graph(),{},stored);restarted.apply();
        int external=0;for(const auto docks=dynamic_docks(restarted.window);auto* dock:docks)if(!dock->isClosed()&&dock->isFloating())++external;
        gui_fixture::check(external==1,"process restart lost floating representation");
        const auto& presets=restarted.application->_ctx.file_configs.ui.pipeline_presets;
        gui_fixture::check(presets.size()==4,"restart lost preset source");
        gui_fixture::check(restarted.editor->load_preset(presets[1].toObject().value("id").toString()),"restart preset switch");
        int open=0;for(const auto docks=restarted.window->workspaceDocks();auto* dock:docks)if(dock!=restarted.dock("dataFlowDock")&&!dock->isClosed())++open;
        gui_fixture::check(open==1,"restart one-view preset inherited external window");
        std::puts("Canonical preset 0/1/multiple views, ADS hosts, manual axes, positions, global isolation, disk reload and process restart PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"workspace: %s\n",error.what());return 1;}
}
