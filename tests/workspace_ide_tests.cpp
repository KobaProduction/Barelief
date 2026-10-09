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

static void check(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
static void settle() { QCoreApplication::processEvents(); QTest::qWait(30); }
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    try {
        WorkspaceWindow host;
        host.resize(900,650);
        auto make=[&](const char *id) {
            auto *dock=new WorkspaceDock(QString::fromLatin1(id),&host);
            dock->setObjectName(QString::fromLatin1(id)); dock->setWidget(new QLabel(id));
            auto* placement=new QAction("Placement",dock);placement->setObjectName("graphPlacement");
            dock->setTitleBarActions({placement});
            host.addDockWidget(Qt::LeftDockWidgetArea,dock); return dock;
        };
        auto *a=make("graph-a"),*b=make("graph-b"),*flow=make("data-flow");
        host.tabifyDockWidget(a,b); host.splitDockWidget(a,flow,Qt::Vertical);
        a->raise(); host.show(); settle();
        check(host.workspaceDock("graph-b")==b && host.workspaceDocks().size()==3,"Inactive tabs disappear from workspace inventory");
        check(a->dockAreaWidget()==b->dockAreaWidget(),"Graphs did not form a tab group");
        auto *tabsMenu=a->dockAreaWidget()->titleBar()->findChild<QToolButton *>("tabsMenuButton");
        check(tabsMenu && tabsMenu->isVisible(),"Multiple tabs have no selector");
        ads::CDockOverlay dropOverlay(&host,ads::CDockOverlay::ModeDockAreaOverlay);
        auto *overlay=&dropOverlay;
        overlay->setAllowedAreas(ads::AllDockAreas);
        const auto areaSize=a->dockAreaWidget()->size();
        const auto nearLeft=a->dockAreaWidget()->mapToGlobal(QPoint(areaSize.width()/10,areaSize.height()/2));
        overlay->showOverlay(a->dockAreaWidget(),nearLeft);
        check(overlay->dropAreaUnderCursor(nearLeft)==ads::LeftDockWidgetArea,"Broad edge docking requires hitting the small indicator");
        const auto tabDrop=workspace_drag::insertion(a,0);
        overlay->showOverlay(a->dockAreaWidget(),tabDrop);
        check(overlay->dropAreaUnderCursor(tabDrop)==ads::CenterDockWidgetArea && overlay->tabIndexUnderCursor()==0,"Existing tabbar intercepted by broad edge drop zone");
        overlay->hideOverlay();
        a->setRenderFPS(0,true); settle();
        auto *fps=a->dockAreaWidget()->titleBar()->findChild<QLabel *>("graphRenderStatus");
        check(fps && fps->isVisible() && fps->text()=="0 FPS","Graph header lost idle FPS");
        auto *content=a->widget(); content->setProperty("camera",123);
        if(QGuiApplication::platformName()=="offscreen") {
            // Exercise ADS's actual tab drag delegate, not a product mouse handler.
            auto *tab=a->tabWidget(); auto *originalArea=a->dockAreaWidget();
            const auto local=tab->rect().center();
            QTest::mousePress(tab,Qt::LeftButton,Qt::NoModifier,local);
            const auto target=local+QPoint(0,-80);
            QMouseEvent move(QEvent::MouseMove,target,tab->mapToGlobal(target),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
            QApplication::sendEvent(tab,&move); settle();
            bool preview=false;
            for(auto *widget:QApplication::topLevelWidgets())
                if(widget->inherits("ads::CFloatingDragPreview") && widget->isVisible()) preview=true;
            check(preview,"Dragging a tab did not create the library drop preview");
            QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
            QApplication::sendEvent(&host,&cancel); settle();
            QTest::mouseRelease(tab,Qt::LeftButton,Qt::NoModifier,local); settle();
            check(!a->isFloating() && a->dockAreaWidget()==originalArea,"Escape did not cancel tab tear-off");
        }
        a->setFloating(true); settle();
        check(a->isFloating() && a->floatingDockContainer() && b->dockAreaWidget()!=a->dockAreaWidget(),"Tab tear-off moved the entire source group");
        check(b->dockAreaWidget()->titleBar()->findChild<QToolButton *>("tabsMenuButton")->isHidden(),"Single tab retains a useless selector");
        QPointer<ads::CFloatingDockContainer> oldHost=a->floatingDockContainer();
        oldHost->move(1100,40);settle();
        workspace_drag::tab(a,workspace_drag::insertion(b,0));
        check(!a->isFloating() && a->dockAreaWidget()==b->dockAreaWidget() && a->dockAreaWidget()->currentDockWidget()==a,"Floating tab gesture did not join/select existing tab group");
        check(a->dockAreaWidget()->dockWidgets().indexOf(a)==0 && oldHost.isNull(),"Drop lost insertion index or leaked old floating host");
        check(a->widget()==content && content->property("camera").toInt()==123,"Gesture recreated graph content");
        workspace_drag::tab(b,workspace_drag::insertion(a,0),true);
        check(b->dockAreaWidget()->dockWidgets().indexOf(b)==0 && b->dockAreaWidget()->currentDockWidget()==b,"Internal tab reorder lost order/selection");
        workspace_drag::tab_list(b);
        // Reverse tear-off is also a real tab gesture, not setFloating().
        workspace_drag::tab(a,QPoint(1200,900));
        check(a->isFloating() && !b->isFloating(),"Reverse tab tear-off moved its sibling");
        b->setFloating(true); settle();
        check(a->floatingDockContainer()!=b->floatingDockContainer(),"Separate requests reused the same external window");
        a->floatingDockContainer()->move(1100,40);b->floatingDockContainer()->move(1600,40);settle();
        QPointer<ads::CFloatingDockContainer> oldB=b->floatingDockContainer();
        workspace_drag::tab(b,workspace_drag::insertion(a,0));
        check(a->floatingDockContainer()==b->floatingDockContainer(),"External tabs did not join the requested group");
        check(oldB.isNull() && a->dockAreaWidget()->currentDockWidget()==b && a->dockAreaWidget()->dockWidgets().indexOf(b)==0,"Cross-host drop failed to release old host/select/insert tab");
        // Reorder in the existing external tab bar through the real moved signal.
        workspace_drag::tab(a,workspace_drag::insertion(b,0),true);
        check(a->dockAreaWidget()->dockWidgets().indexOf(a)==0 && a->dockAreaWidget()->currentDockWidget()==a,"Tab reorder lost exact order/selection");
        workspace_drag::tab_list(a);
        workspace_drag::tab(b,QPoint(1900,900));
        check(a->floatingDockContainer()!=b->floatingDockContainer(),"Detaching a tab from an external group was ignored");
        auto* header=a->dockAreaWidget()->titleBar();
        QPointer<ads::CFloatingDockContainer> headerHost=a->floatingDockContainer();
        workspace_drag::drag(header,QPoint(header->width()/2,header->height()/2),workspace_drag::insertion(flow,0));
        check(!a->isFloating() && b->isFloating(),"Returning one tab also moved another external window");
        check(headerHost.isNull() && a->dockAreaWidget()==flow->dockAreaWidget() && a->dockAreaWidget()->currentDockWidget()==a,"Floating header drop did not select/release its host");
        a->setAutoHide(true,ads::SideBarRight); settle();
        check(a->isAutoHide(),"Pin did not create an auto-hide sidebar");
        const auto saved=host.saveState(1);
        a->setAutoHide(false); b->hide(); settle();
        check(host.restoreState(saved,1),"Workspace state did not restore"); settle();
        check(a->isAutoHide() && !b->isClosed() && b->isFloating(),"Restore lost pin, visibility or external window");
        check(a->widget()==content && content->property("camera").toInt()==123,"Docking recreated graph content or camera state");
        check(!host.restoreState("invalid",1),"Damaged layout was accepted");
        host.close();
        qInfo("IDE workspace: tab drag/cancel, split, pin, external windows and restore passed");
        return 0;
    } catch(const std::exception &e) { qCritical("IDE workspace: %s",e.what());return 1; }
}
