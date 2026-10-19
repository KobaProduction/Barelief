#include "core_app/workspace/workspace_window.h"
#include <DockAreaWidget.h>
#include <DockAreaTitleBar.h>
#include <DockWidgetTab.h>
#include <DockOverlay.h>
#include <FloatingDockContainer.h>
#include <FloatingDragPreview.h>
#include <QApplication>
#include <QLabel>
#include <QToolButton>
#include <QMouseEvent>
#include <QtTest/QTest>
#include <stdexcept>
#include "workspace_drag_fixture.h"
#include <QAction>
#include <AutoHideTab.h>
#include <AutoHideDockContainer.h>
#include <QMetaProperty>
#include <QSignalSpy>
#include <QPixmap>
#include <memory>

static void check(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
static void settle() { QCoreApplication::processEvents(); QTest::qWait(30); }
static void check_auto_hide_property_metadata(const ads::CAutoHideTab& tab) {
    const auto* meta=tab.metaObject();
    for(const char* name:{"sideBarLocation","orientation","activeTab","iconOnly"}) {
        const int index=meta->indexOfProperty(name);
        check(index>=0,"Auto-hide property missing from meta-object");
        const auto property=meta->property(index);
        check(property.hasNotifySignal(),"Auto-hide property has no notifier");
        check(property.notifySignal().methodSignature()=="tabStateChanged()","Auto-hide property uses incorrect notifier");
    }
}
static void check_auto_hide_stable_notifier(ads::CAutoHideTab& tab) {
    tab.updateStyle();
    QSignalSpy changes(&tab,&ads::CAutoHideTab::tabStateChanged);
    check(changes.isValid(),"Auto-hide state notifier is not observable");
    tab.updateStyle();
    tab.updateStyle();
    check(changes.isEmpty(),"Unchanged auto-hide style emits a state change");
    const auto orientation=tab.orientation();
    tab.setOrientation(orientation==Qt::Horizontal?Qt::Vertical:Qt::Horizontal);
    check(changes.count()==1,"Changed auto-hide orientation did not notify exactly once");
    tab.setOrientation(orientation);
}
static void check_auto_hide_active_notifications(WorkspaceDock& dock) {
    auto* tab=dock.sideTabWidget();
    auto* container=dock.autoHideDockContainer();
    container->hide();
    settle();
    tab->updateStyle();
    QSignalSpy changes(tab,&ads::CAutoHideTab::tabStateChanged);
    container->show();
    settle();
    check(tab->isActiveTab() && changes.count()==1,"Showing auto-hide panel did not notify active state exactly once");
    changes.clear();
    container->hide();
    settle();
    check(!tab->isActiveTab() && changes.count()==1,"Hiding auto-hide panel did not notify active state exactly once");
}
static void check_auto_hide_icon_notifications(WorkspaceDock& dock) {
    const auto flags=ads::CDockManager::autoHideConfigFlags();
    ads::CDockManager::setAutoHideConfigFlag(ads::CDockManager::AutoHideSideBarsIconOnly,true);
    auto* tab=dock.sideTabWidget();
    tab->updateStyle();
    QSignalSpy changes(tab,&ads::CAutoHideTab::tabStateChanged);
    QPixmap pixels(8,8);
    pixels.fill(Qt::red);
    dock.setIcon(QIcon(pixels));
    settle();
    check(tab->iconOnly() && changes.count()==1,"Nonempty dock icon did not notify icon-only state exactly once");
    changes.clear();
    dock.setIcon(QIcon{});
    settle();
    check(!tab->iconOnly() && changes.count()==1,"Empty dock icon did not notify icon-only state exactly once");
    ads::CDockManager::setAutoHideConfigFlags(flags);
    tab->updateStyle();
}
static void check_auto_hide_properties(WorkspaceDock& dock) {
    auto* tab=dock.sideTabWidget();
    check(tab,"Auto-hide tab missing");
    check_auto_hide_property_metadata(*tab);
    check_auto_hide_stable_notifier(*tab);
    check_auto_hide_active_notifications(dock);
    check_auto_hide_icon_notifications(dock);
}
static WorkspaceDock* make_dock(WorkspaceWindow& host,const char* id) {
    auto* dock=new WorkspaceDock(QString::fromLatin1(id),&host);
    dock->setObjectName(QString::fromLatin1(id));
    dock->setWidget(new QLabel(id));
    auto* placement=new QAction("Placement",dock);
    placement->setObjectName("graphPlacement");
    dock->setTitleBarActions({placement});
    host.addDockWidget(Qt::LeftDockWidgetArea,dock);
    return dock;
}
struct WorkspaceScenario {
    WorkspaceWindow host;
    WorkspaceDock* a;
    WorkspaceDock* b;
    WorkspaceDock* flow;
    QWidget* content=nullptr;
    std::unique_ptr<ads::CDockOverlay> dropOverlay;
    WorkspaceScenario() {
        host.resize(900,650);
        a=make_dock(host,"graph-a");
        b=make_dock(host,"graph-b");
        flow=make_dock(host,"data-flow");
        host.tabifyDockWidget(a,b);
        host.splitDockWidget(a,flow,Qt::Vertical);
        a->raise();
        host.show();
        settle();
    }
};
static void check_initial_layout(WorkspaceScenario& s) {
    check(s.host.workspaceDock("graph-b")==s.b && s.host.workspaceDocks().size()==3,"Inactive tabs disappear from workspace inventory");
    check(s.a->dockAreaWidget()==s.b->dockAreaWidget(),"Graphs did not form a tab group");
    auto* tabsMenu=s.a->dockAreaWidget()->titleBar()->findChild<QToolButton *>("tabsMenuButton");
    check(tabsMenu && tabsMenu->isVisible(),"Multiple tabs have no selector");
}
static void check_drop_zones(WorkspaceScenario& s) {
    s.dropOverlay=std::make_unique<ads::CDockOverlay>(&s.host,ads::CDockOverlay::ModeDockAreaOverlay);
    auto& overlay=*s.dropOverlay;
    overlay.setAllowedAreas(ads::AllDockAreas);
    const auto areaSize=s.a->dockAreaWidget()->size();
    const auto nearLeft=s.a->dockAreaWidget()->mapToGlobal(QPoint(areaSize.width()/10,areaSize.height()/2));
    overlay.showOverlay(s.a->dockAreaWidget(),nearLeft);
    check(overlay.dropAreaUnderCursor(nearLeft)==ads::LeftDockWidgetArea,"Broad edge docking requires hitting the small indicator");
    const auto tabDrop=workspace_drag::insertion(s.a,0);
    overlay.showOverlay(s.a->dockAreaWidget(),tabDrop);
    check(overlay.dropAreaUnderCursor(tabDrop)==ads::CenterDockWidgetArea && overlay.tabIndexUnderCursor()==0,"Existing tabbar intercepted by broad edge drop zone");
    overlay.hideOverlay();
}
static void check_idle_fps(WorkspaceScenario& s) {
    s.a->setRenderFPS(0,true);
    settle();
    auto* fps=s.a->dockAreaWidget()->titleBar()->findChild<QLabel *>("graphRenderStatus");
    check(fps && fps->isVisible() && fps->text()=="0 FPS","Graph header lost idle FPS");
    s.content=s.a->widget();
    s.content->setProperty("camera",123);
}
static bool visible_drag_preview() {
    const auto widgets=QApplication::topLevelWidgets();
    for(auto* widget:widgets)
        if(widget->inherits("ads::CFloatingDragPreview") && widget->isVisible()) return true;
    return false;
}
static void start_tab_drag(ads::CDockWidgetTab& tab,QPoint local) {
    QTest::mousePress(&tab,Qt::LeftButton,Qt::NoModifier,local);
    const auto target=local+QPoint(0,-80);
    QMouseEvent move(QEvent::MouseMove,target,tab.mapToGlobal(target),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&tab,&move);
    settle();
}
static void check_drag_cancellation(WorkspaceScenario& s) {
    if(QGuiApplication::platformName()!="offscreen") return;
    // Exercise ADS's actual tab drag delegate, not a product mouse handler.
    auto* tab=s.a->tabWidget();
    auto* originalArea=s.a->dockAreaWidget();
    const auto local=tab->rect().center();
    start_tab_drag(*tab,local);
    check(visible_drag_preview(),"Dragging a tab did not create the library drop preview");
    QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(&s.host,&cancel);
    settle();
    QTest::mouseRelease(tab,Qt::LeftButton,Qt::NoModifier,local);
    settle();
    check(!s.a->isFloating() && s.a->dockAreaWidget()==originalArea,"Escape did not cancel tab tear-off");
}
static void check_floating_tab_return(WorkspaceScenario& s) {
    s.a->setFloating(true);
    settle();
    check(s.a->isFloating() && s.a->floatingDockContainer() && s.b->dockAreaWidget()!=s.a->dockAreaWidget(),"Tab tear-off moved the entire source group");
    check(s.b->dockAreaWidget()->titleBar()->findChild<QToolButton *>("tabsMenuButton")->isHidden(),"Single tab retains a useless selector");
    QPointer<ads::CFloatingDockContainer> oldHost=s.a->floatingDockContainer();
    oldHost->move(1100,40);
    settle();
    workspace_drag::tab(s.a,workspace_drag::insertion(s.b,0));
    check(!s.a->isFloating() && s.a->dockAreaWidget()==s.b->dockAreaWidget() && s.a->dockAreaWidget()->currentDockWidget()==s.a,"Floating tab gesture did not join/select existing tab group");
    check(s.a->dockAreaWidget()->dockWidgets().indexOf(s.a)==0 && oldHost.isNull(),"Drop lost insertion index or leaked old floating host");
    check(s.a->widget()==s.content && s.content->property("camera").toInt()==123,"Gesture recreated graph content");
}
static void check_internal_reorder(WorkspaceScenario& s) {
    workspace_drag::tab(s.b,workspace_drag::insertion(s.a,0),true);
    check(s.b->dockAreaWidget()->dockWidgets().indexOf(s.b)==0 && s.b->dockAreaWidget()->currentDockWidget()==s.b,"Internal tab reorder lost order/selection");
    workspace_drag::tab_list(s.b);
}
static void check_cross_host_join(WorkspaceScenario& s) {
    // Reverse tear-off is also a real tab gesture, not setFloating().
    workspace_drag::tab(s.a,QPoint(1200,900));
    check(s.a->isFloating() && !s.b->isFloating(),"Reverse tab tear-off moved its sibling");
    s.b->setFloating(true);
    settle();
    check(s.a->floatingDockContainer()!=s.b->floatingDockContainer(),"Separate requests reused the same external window");
    s.a->floatingDockContainer()->move(1100,40);
    s.b->floatingDockContainer()->move(1600,40);
    settle();
    QPointer<ads::CFloatingDockContainer> oldB=s.b->floatingDockContainer();
    workspace_drag::tab(s.b,workspace_drag::insertion(s.a,0));
    check(s.a->floatingDockContainer()==s.b->floatingDockContainer(),"External tabs did not join the requested group");
    check(oldB.isNull() && s.a->dockAreaWidget()->currentDockWidget()==s.b && s.a->dockAreaWidget()->dockWidgets().indexOf(s.b)==0,"Cross-host drop failed to release old host/select/insert tab");
}
static void check_external_reorder(WorkspaceScenario& s) {
    // Reorder in the existing external tab bar through the real moved signal.
    workspace_drag::tab(s.a,workspace_drag::insertion(s.b,0),true);
    check(s.a->dockAreaWidget()->dockWidgets().indexOf(s.a)==0 && s.a->dockAreaWidget()->currentDockWidget()==s.a,"Tab reorder lost exact order/selection");
    workspace_drag::tab_list(s.a);
    workspace_drag::tab(s.b,QPoint(1900,900));
    check(s.a->floatingDockContainer()!=s.b->floatingDockContainer(),"Detaching a tab from an external group was ignored");
}
static void check_floating_header_return(WorkspaceScenario& s) {
    auto* header=s.a->dockAreaWidget()->titleBar();
    QPointer<ads::CFloatingDockContainer> headerHost=s.a->floatingDockContainer();
    workspace_drag::drag(header,QPoint(header->width()/2,header->height()/2),workspace_drag::insertion(s.flow,0));
    check(!s.a->isFloating() && s.b->isFloating(),"Returning one tab also moved another external window");
    check(headerHost.isNull() && s.a->dockAreaWidget()==s.flow->dockAreaWidget() && s.a->dockAreaWidget()->currentDockWidget()==s.a,"Floating header drop did not select/release its host");
}
static void check_pinned_state_restore(WorkspaceScenario& s) {
    s.a->setAutoHide(true,ads::SideBarRight);
    settle();
    check(s.a->isAutoHide(),"Pin did not create an auto-hide sidebar");
    check_auto_hide_properties(*s.a);
    const auto saved=s.host.saveState(1);
    s.a->setAutoHide(false);
    s.b->hide();
    settle();
    check(s.host.restoreState(saved,1),"Workspace state did not restore");
    settle();
    check(s.a->isAutoHide() && !s.b->isClosed() && s.b->isFloating(),"Restore lost pin, visibility or external window");
    check(s.a->widget()==s.content && s.content->property("camera").toInt()==123,"Docking recreated graph content or camera state");
    check(!s.host.restoreState("invalid",1),"Damaged layout was accepted");
}
static void run_workspace_scenarios() {
    WorkspaceScenario scenario;
    check_initial_layout(scenario);
    check_drop_zones(scenario);
    check_idle_fps(scenario);
    check_drag_cancellation(scenario);
    check_floating_tab_return(scenario);
    check_internal_reorder(scenario);
    check_cross_host_join(scenario);
    check_external_reorder(scenario);
    check_floating_header_return(scenario);
    check_pinned_state_restore(scenario);
    scenario.host.close();
}
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    try {
        run_workspace_scenarios();
        qInfo("IDE workspace: tab drag/cancel, split, pin, external windows and restore passed");
        return 0;
    } catch(const std::exception &e) {
        qCritical("IDE workspace: %s",e.what());
        return 1;
    }
}
