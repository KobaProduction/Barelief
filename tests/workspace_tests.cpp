#include "numeric_gui_fixture.h"
#include <core_app/graph_panel.h>
#include <core_app/config_manager.h>
#include <visualization/charts/line.h>
#include <visualization/ui_text.h>
#include <QFile>
#include <QPointer>
#include <QStatusBar>
#include <QProcess>
#include <DockAreaWidget.h>
#include <FloatingDockContainer.h>
#include <cstdio>
static QString save_preset(gui_fixture::App& app,const QString& name){gui_fixture::check(app.editor->save_preset(name),"actual preset Save");return app.application->_ctx.file_configs.ui.pipeline_presets.last().toObject()["id"].toString();}
static QList<WorkspaceDock*> dynamic_docks(MainWindow* window){QList<WorkspaceDock*> result;for(auto* dock:window->workspaceDocks())if(dock->objectName().startsWith("nodeView_"))result.append(dock);return result;}
static QRect area_rect(MainWindow* window,WorkspaceDock* dock){auto* area=dock->dockAreaWidget();gui_fixture::check(area,"dock area missing");return {area->mapTo(window,QPoint()),area->size()};}
static QStringList open_tabs(WorkspaceDock* dock){QStringList names;for(auto* peer:dock->dockAreaWidget()->dockWidgets())if(!peer->isClosed())names.append(peer->objectName());names.sort();return names;}
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
static QByteArray initial_session(){
        using namespace gui_fixture;
        App app;app.apply();auto& ui=app.application->_ctx.file_configs.ui;
        const auto revision=app.presenter().client().snapshot().graph_revision;
        const auto theme=ui.theme;const auto language=ui.language;
        for(auto* dock:app.window->workspaceDocks())if(dock!=app.dock("dataFlowDock"))dock->toggleView(false);
        const auto zero=save_preset(app,"zero views");
        auto* first=app.open(10,2,0);const auto firstName=first->objectName();const auto firstKey=firstName.mid(QStringLiteral("nodeView_").size());
        ui.graph_views[firstKey]=QJsonObject{{"device",app.presenter().device_id()},{"graph",app.presenter().graph_id()},{"node",10},{"kind","FFTView"},{"instance",first->property("viewInstance").toInt()},{"open",true},{"auto_range_x",false},{"minimum_x",100},{"maximum_x",500},{"reverse_x",true},{"side_ratio",.3}};
        auto* panel=qobject_cast<GraphPanel*>(first->widget());check(panel,"FFT panel");panel->restore_presentation(ui.graph_views[firstKey].toObject());
        auto* model=app.application->_ctx.data_flow_graph.get();model->setNodeData(1,QtNodes::NodeRole::Position,QPointF(42,73));
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
        const auto severalPresentation=ui.pipeline_presets.last().toObject()["presentation"].toObject();
        check(severalPresentation["workspace"].toObject()["visibility"].toObject()[inactiveName].toBool(),"inactive tab saved as closed");
        app.window->placeDock(neighbor,first,Qt::BottomDockWidgetArea);inactive->raise();
        external->floatingDockContainer()->resize(360,250);external->floatingDockContainer()->move(160,170);
        check(wait([&]{return first->dockAreaWidget()->currentDockWidget()==inactive && area_rect(app.window,first).bottom()<area_rect(app.window,neighbor).top();}),"distinct B topology precondition");
        const auto bGeometry=external->floatingDockContainer()->frameGeometry();check(!close_rect(aTopology.floating,bGeometry),"B floating geometry is not distinct");
        const auto otherLayout=save_preset(app,"vertical split with other active tab");
        check(app.editor->load_preset(several),"A to B to A topology restore");
        first=app.window->workspaceDock(firstName);neighbor=app.window->workspaceDock(neighborName);inactive=app.window->workspaceDock(inactiveName);external=app.window->workspaceDock(externalName);
        const bool restoredA=wait([&]{return first && neighbor && inactive && external && first->dockAreaWidget()==inactive->dockAreaWidget() && first->dockAreaWidget()->currentDockWidget()==first && area_rect(app.window,first).right()<area_rect(app.window,neighbor).left();});
        if(!restoredA && first && neighbor && inactive)qWarning()<<"A restore diagnostic"<<first->objectName()<<"tabs"<<open_tabs(first)<<"active"<<first->dockAreaWidget()->currentDockWidget()->objectName()<<"same group"<<(first->dockAreaWidget()==inactive->dockAreaWidget())<<"left"<<area_rect(app.window,first)<<"right"<<area_rect(app.window,neighbor);
        check(restoredA,"restored A tab membership/selection/split order");
        const auto restoredTopology=topology(app.window,first,neighbor,external);
        check(restoredTopology.tabs==aTopology.tabs && restoredTopology.active==aTopology.active && std::abs(restoredTopology.leftShare-aTopology.leftShare)<.08 && close_rect(restoredTopology.floating,aTopology.floating),"restored A tab set/split proportion/float geometry");
        check(app.editor->load_preset(otherLayout),"load distinct B saved topology");
        first=app.window->workspaceDock(firstName);neighbor=app.window->workspaceDock(neighborName);inactive=app.window->workspaceDock(inactiveName);external=app.window->workspaceDock(externalName);
        check(wait([&]{return first->dockAreaWidget()->currentDockWidget()==inactive && area_rect(app.window,first).bottom()<area_rect(app.window,neighbor).top();}) && close_rect(external->floatingDockContainer()->frameGeometry(),bGeometry),"B saved topology restoration");
        ui.graph_rendering={{"version",1},{"defaults",QJsonObject{{"brightness",1.8}}}};
        model->setNodeData(1,QtNodes::NodeRole::Position,QPointF(777,888));
        check(app.editor->load_preset(zero),"load zero");
        check(wait([&]{for(auto* dock:app.window->workspaceDocks())if(dock!=app.dock("dataFlowDock")&&!dock->isClosed())return false;return true;}),"zero preset retained graphs");
        check(app.editor->load_preset(one),"load one");first=app.window->workspaceDock(firstName);check(first&&!first->isClosed(),"one preset lost representation");
        int open=0;for(auto* dock:app.window->workspaceDocks())if(dock!=app.dock("dataFlowDock")&&!dock->isClosed())++open;
        check(open==1,"one preset inherited other open tabs");
        check(model->nodeData(1,QtNodes::NodeRole::Position).toPointF()==QPointF(42,73),"preset coordinates inherited previous positions");
        auto* restoredPanel=qobject_cast<GraphPanel*>(first->widget());
        auto* chart=restoredPanel?restoredPanel->active_chart():nullptr;check(chart,"restored active chart");
        check(chart->x_range().first==100 && chart->x_range().second==500 && chart->reverse_x(),"preset manual axis restoration");
        check(app.editor->load_preset(several),"load multiple");
        external=app.window->workspaceDock(externalName);neighbor=app.window->workspaceDock(neighborName);inactive=app.window->workspaceDock(inactiveName);
        check(external&&external->isFloating()&&!external->isClosed()&&neighbor&&!neighbor->isClosed()&&inactive&&!inactive->isClosed(),"multiple preset lost host/open state");
        check(ui.graph_rendering==severalPresentation["graph_rendering"].toObject(),"preset rendering inherited prior display");
        check(ui.theme==theme&&ui.language==language,"preset rewrote global preferences");
        check(app.presenter().client().snapshot().graph_revision==revision&&app.presenter().client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,"preset changed continuous working graph");
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
        const auto savedPresets=settings["ui"].toObject()["pipeline_presets"].toArray();
        check(savedPresets.size()==4,"disk reload lost preset list");
        QJsonObject savedA;for(const auto& preset:savedPresets)if(preset.toObject()["id"]==several)savedA=preset.toObject()["presentation"].toObject();
        check(savedA==severalPresentation,"disk reload changed preset display snapshot");
        emit app.window->save_configuration_as_requested(app.directory.filePath("saved-as.json"));QFile saved(app.directory.filePath("saved-as.json"));check(saved.exists(),"Save As canonical project");
        const auto failedPath=app.directory.filePath("invalid.json");QFile failed(failedPath);check(failed.open(QIODevice::WriteOnly),"invalid fixture");failed.write("{ broken");failed.close();emit app.window->open_configuration_requested(failedPath);
        check(app.window->isVisible()&&!app.window->statusBar()->currentMessage().isEmpty(),"malformed Open changed window or omitted error");
        check(app.presenter().client().snapshot().graph_revision==revision,"save/open changed processing");
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
        gui_fixture::App restarted(numeric_test_graph(),{},stored);restarted.apply();
        int external=0;for(auto* dock:dynamic_docks(restarted.window))if(!dock->isClosed()&&dock->isFloating())++external;
        gui_fixture::check(external==1,"process restart lost floating representation");
        const auto& presets=restarted.application->_ctx.file_configs.ui.pipeline_presets;
        gui_fixture::check(presets.size()==4,"restart lost preset source");
        gui_fixture::check(restarted.editor->load_preset(presets[1].toObject()["id"].toString()),"restart preset switch");
        int open=0;for(auto* dock:restarted.window->workspaceDocks())if(dock!=restarted.dock("dataFlowDock")&&!dock->isClosed())++open;
        gui_fixture::check(open==1,"restart one-view preset inherited external window");
        std::puts("Canonical preset 0/1/multiple views, ADS hosts, manual axes, positions, global isolation, disk reload and process restart PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"workspace: %s\n",error.what());return 1;}
}
