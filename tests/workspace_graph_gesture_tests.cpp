#include "numeric_gui_fixture.h"
#include "workspace_drag_fixture.h"
#include <FloatingDockContainer.h>
#include <QGraphicsView>
#include <QPointer>
#include <QtTest/QTest>
#include <cstdio>
using namespace gui_fixture;
static int opened(MainWindow* window,WorkspaceDock* flow=nullptr){
    int count=0;for(auto* dock:window->workspaceDocks())
        if(dock->property("graphNode").isValid() && !dock->isClosed() && (!flow || dock->sharesHost(flow)))++count;
    return count;
}
static void click_node(App& app,int node){
    auto* view=app.editor->findChild<QGraphicsView*>();
    auto* scene=app.editor->findChild<QtNodes::DataFlowGraphicsScene*>();
    auto* item=scene->nodeGraphicsObject(node);check(view && item,"Node click fixture");
    const auto point=item->sceneBoundingRect().topLeft()+QPointF(30,15);
    view->centerOn(point);workspace_drag::settle();
    const auto viewport=view->mapFromScene(point);
    check(view->viewport()->rect().contains(viewport),"Node click outside viewport");
    QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,viewport);workspace_drag::settle();
}
static void select_tab(WorkspaceDock* dock){
    auto* tab=dock->tabWidget();check(tab->isVisible(),"Selected graph tab hidden");
    QTest::mouseClick(tab,Qt::LeftButton,Qt::NoModifier,tab->rect().center());workspace_drag::settle();
}
static void placement(WorkspaceDock* dock,int value){
    auto* action=dock->findChild<QAction*>("graphPlacement");check(action && action->menu(),"Graph placement menu absent");
    action->menu()->actions()[value]->trigger();workspace_drag::settle();
}
int main(){try {
    App app;app.apply();auto* flow=app.dock("dataFlowDock");
    const auto baseline=app.presenter().client().snapshot();
    for(auto* dock:app.window->workspaceDocks())if(dock!=flow)dock->toggleView(false);
    const auto inventory=app.window->workspaceDocks().size();
    click_node(app,10);check(opened(app.window)==0 && app.window->workspaceDocks().size()==inventory,"Pipeline-only click created a graph");
    auto* first=app.open(10,2,0);auto* inactive=app.open(11,1,0);auto* adjacent=app.open(10,2,0);
    check(first!=adjacent && first->dockAreaWidget()==inactive->dockAreaWidget() && adjacent->dockAreaWidget()!=first->dockAreaWidget(),"Explicit tab/adjacent actions lost distinct instances");
    const QRect flowBounds(flow->mapTo(app.window,QPoint()),flow->size());
    const QRect graphBounds(first->mapTo(app.window,QPoint()),first->size());
    check(flowBounds.top()>graphBounds.bottom(),"Pipeline default is not below internal graphs");
    auto* external=app.open(12,0,0);auto* externalAgain=app.open(12,0,0);
    check(external!=externalAgain && external->isFloating() && externalAgain->isFloating() && !external->sharesHost(externalAgain),"Repeated external actions reused an instance/host");
    external->floatingDockContainer()->move(1300,40);externalAgain->floatingDockContainer()->move(2300,40);workspace_drag::settle();
    const auto identity=external->objectName();const auto instance=external->property("viewInstance");
    const auto binding=external->property("numericEndpoint").toMap();check(binding.value("port").toInt()==0 && binding.value("direction")=="input","Explicit count-port binding missing");
    const auto key=identity.mid(QStringLiteral("nodeView_").size());
    auto& views=app.application->_ctx.file_configs.ui.graph_views;
    auto prefs=views.value(key).toObject();prefs["extension"]=QJsonObject{{"keep",42}};views[key]=prefs;
    QPointer<QWidget> renderer=external->widget();QPointer<ads::CFloatingDockContainer> oldHost=external->floatingDockContainer();
    workspace_drag::tab(external,workspace_drag::insertion(first,1));
    check(!external->isFloating() && external->sharesHost(flow) && external->dockAreaWidget()==first->dockAreaWidget(),"Floating graph did not join existing pipeline tab group");
    check(oldHost.isNull() && external->dockAreaWidget()->currentDockWidget()==external && external->dockAreaWidget()->dockWidgets().indexOf(external)==1,"Drop lost exact insertion/active tab or leaked source host");
    check(external->objectName()==identity && external->property("viewInstance")==instance && external->widget()==renderer && external->property("numericEndpoint").toMap()==binding && views.value(key).toObject().value("extension")==prefs.value("extension"),"Gesture changed instance/renderer/settings/port binding");
    check(external->dockAreaWidget()->titleBar()->findChild<QToolButton*>("tabsMenuButton")->isVisible(),"Joined group missing tab list");
    workspace_drag::tab_list(external);
    const auto firstIdentity=first->objectName(),inactiveIdentity=inactive->objectName();
    app.window->findChild<QAction*>("saveSettings")->trigger();
    QFile persisted(QString::fromLocal8Bit(app.path));check(persisted.open(QIODevice::ReadOnly),"Read saved gesture workspace");
    const auto joinedProject=persisted.readAll();persisted.close();
    select_tab(first);app.window->findChild<QAction*>("saveSettings")->trigger();
    check(persisted.open(QIODevice::ReadOnly),"Read first-active gesture workspace");
    const auto firstActiveProject=persisted.readAll();persisted.close();select_tab(external);
    workspace_drag::tab(external,QPoint(1700,1000));check(external->isFloating() && !first->isFloating() && !inactive->isFloating(),"Reverse detach moved other tabs");
    check(external->widget()==renderer && external->property("numericEndpoint").toMap()==binding,"Reverse detach changed renderer/binding");
    select_tab(first);const auto beforeFocus=app.window->workspaceDocks().size();
    click_node(app,11);check(inactive->dockAreaWidget()->currentDockWidget()==inactive && !first->isClosed() && app.window->workspaceDocks().size()==beforeFocus,"Click did not focus an existing inactive tab");
    auto* raw=app.dock("rawSamplesDock");auto* surface=app.dock("spectrogramDock");
    click_node(app,9);check(!raw->isClosed() && inactive->isClosed() && raw->dockAreaWidget()==first->dockAreaWidget() && !adjacent->isClosed(),"New endpoint did not replace last selected slot only");
    click_node(app,8);check(!surface->isClosed() && raw->isClosed(),"Second endpoint did not replace slot");
    const auto beforeReturn=opened(app.window);
    click_node(app,9);check(!raw->isClosed() && surface->isClosed() && opened(app.window)==beforeReturn && raw->dockAreaWidget()->currentDockWidget()==raw,"Returning closed same-group endpoint implicitly added a tab");
    check(external->isFloating() && externalAgain->isFloating() && external->property("numericEndpoint").toMap()==binding,"Pipeline clicks consumed an external graph-only host");
    placement(flow,0);check(flow->isFloating(),"Explicit pipeline detach failed");
    const auto beforePipelineOnly=app.window->workspaceDocks().size();click_node(app,10);
    check(opened(app.window,flow)==0 && app.window->workspaceDocks().size()==beforePipelineOnly,"Floating pipeline-only click consumed graphs from another host");
    auto* floatingSlot=app.open(10,1,0);check(floatingSlot->sharesHost(flow),"Explicit tab in floating pipeline targeted another host");
    click_node(app,11);check(floatingSlot->isClosed() && opened(app.window,flow)==1,"Floating pipeline click replaced a graph outside its own host");
    WorkspaceDock* selected=nullptr;for(auto* dock:app.window->workspaceDocks())
        if(dock->sharesHost(flow) && !dock->isClosed() && dock->property("graphNode").isValid())selected=dock;
    check(selected && selected->property("graphNode").toInt()==11 && selected->dockAreaWidget()->currentDockWidget()==selected,"Floating pipeline selected the wrong endpoint");
    check(external->isFloating() && externalAgain->isFloating() && !external->sharesHost(flow),"Floating pipeline replacement consumed graph-only host");
    const auto final=app.presenter().client().snapshot();
    check(final.graph_revision==baseline.graph_revision && final.epoch==baseline.epoch && final.statuses.value("analyzer").state==pipeline::State::Running,"Workspace gesture changed continuous processing");
    app.application.reset();
    App restarted(numeric_test_graph(),{},joinedProject);restarted.apply();
    auto* restored=restarted.dock(identity);auto* restoredFirst=restarted.dock(firstIdentity);auto* restoredInactive=restarted.dock(inactiveIdentity);
    const bool restoredJoin=wait([&]{return !restored->isClosed() && !restored->isFloating() && restored->dockAreaWidget()==restoredFirst->dockAreaWidget() && restored->dockAreaWidget()==restoredInactive->dockAreaWidget() && restored->dockAreaWidget()->currentDockWidget()==restored;});
    check(restoredJoin,"Restart lost joined topology/active tab");
    check(restored->dockAreaWidget()->dockWidgets().indexOf(restored)==1 && restored->property("viewInstance")==instance && restored->property("numericEndpoint").toMap()==binding,"Restart lost insertion order/instance/port binding");
    check(restarted.application->_ctx.file_configs.ui.graph_views.value(key).toObject().value("extension")==prefs.value("extension"),"Restart lost gestured instance settings");
    restarted.application.reset();
    App firstActive(numeric_test_graph(),{},firstActiveProject);firstActive.apply();
    auto* firstAfterRestart=firstActive.dock(firstIdentity);auto* gestureAfterRestart=firstActive.dock(identity);
    check(wait([&]{return firstAfterRestart->dockAreaWidget()==gestureAfterRestart->dockAreaWidget() && firstAfterRestart->dockAreaWidget()->currentDockWidget()==firstAfterRestart;}),"Restart overwrote the saved first active tab");
    check(gestureAfterRestart->dockAreaWidget()->dockWidgets().indexOf(gestureAfterRestart)==1 && gestureAfterRestart->property("numericEndpoint").toMap()==binding,"First-active restart changed gestured insertion/binding");
    std::puts("Actual Qt docking gestures and pipeline-slot click scope, stable identity/settings/port bindings PASS");return 0;
}catch(const std::exception& error){std::fprintf(stderr,"workspace graph gestures: %s\n",error.what());return 1;}}
