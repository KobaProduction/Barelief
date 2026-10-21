#pragma once
#include <core_app/workspace/workspace_window.h>
#include <DockAreaWidget.h>
#include <DockAreaTitleBar.h>
#include <DockAreaTabBar.h>
#include <DockWidgetTab.h>
#include <QApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QMenu>
#include <QDebug>
#include <QToolButton>
#include <QtTest/QTest>
#include <stdexcept>
namespace workspace_drag {
inline void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline void settle(){QCoreApplication::processEvents();QTest::qWait(30);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
inline void mouse(QWidget* source,QEvent::Type type,QPoint global,Qt::MouseButton button,Qt::MouseButtons buttons){
    QMouseEvent event(type,QPointF(source->mapFromGlobal(global)),QPointF(global),button,buttons,Qt::NoModifier);
    QApplication::sendEvent(source,&event);
}
inline QPoint insertion(WorkspaceDock* destination,int index){
    auto* bar=destination->dockAreaWidget()->titleBar()->tabBar();
    check(bar && bar->isVisible(),"Drop target tabbar is not visible");
    auto* tab=bar->tab(index);check(tab && tab->isVisible(),"Drop target tab is not visible");
    return tab->mapToGlobal(QPoint(3,tab->height()/2));
}
inline void check_tab_list_entry(const QList<QAction*>& actions,int open,int index,ads::CDockWidget* dock){
    const bool matches=open<actions.size() && actions[open]->data().toInt()==index && actions[open]->text()==dock->tabWidget()->text();
    if(!matches){
        qWarning()<<"Tab list expected"<<"entry"<<open<<"layoutIndex"<<index<<"dock"<<dock->objectName()<<"title"<<dock->tabWidget()->text()<<"closed"<<dock->isClosed()<<"tabHidden"<<dock->tabWidget()->isHidden();
        for(auto* action:actions)qWarning()<<"Tab list actual"<<"index"<<action->data()<<"title"<<action->text();
    }
    check(matches,"Tab list lost membership/order");
}
inline void tab_list(WorkspaceDock* dock){
    auto* area=dock->dockAreaWidget();
    auto* button=area->titleBar()->findChild<QToolButton*>("tabsMenuButton");
    check(button && button->isVisible() && button->menu(),"Tab list button missing");
    auto* menu=button->menu();menu->popup(button->mapToGlobal(QPoint(0,button->height())));settle();
    const auto actions=menu->actions();const auto docks=area->dockWidgets();int open=0;
    for(int i=0;i<docks.size();++i)if(!docks[i]->isClosed()){
        check_tab_list_entry(actions,open,i,docks[i]);++open;
    }
    check(actions.size()==open,"Tab list retained stale entries");menu->hide();settle();
}
// All events belong to this isolated app. Never move the system cursor or
// simulate a successful result with addDockWidget/dropWidget calls.
inline void enter_drag_preview(QWidget* source,QPoint global){
        mouse(source,QEvent::MouseMove,global+QPoint(0,-80),Qt::NoButton,Qt::LeftButton);settle();
        bool preview=false;for(const auto widgets=QApplication::topLevelWidgets();auto* widget:widgets)
            if(widget->inherits("ads::CFloatingDragPreview") && widget->isVisible())preview=true;
        check(preview,"Qt drag did not enter the ADS preview path");
}
template<class Target>
inline void drag_to(QWidget* source,QPoint start,Target target_position,bool reorder=false){
    check(source && source->isVisible(),"Drag source is not visible");
    const auto global=source->mapToGlobal(start);
    mouse(source,QEvent::MouseButtonPress,global,Qt::LeftButton,Qt::LeftButton);
    if(!reorder) {
        enter_drag_preview(source,global);
    }
    const auto target=target_position();
    mouse(source,QEvent::MouseMove,target,Qt::NoButton,Qt::LeftButton);
    // The first horizontal movement starts tab reordering; a second moves it.
    mouse(source,QEvent::MouseMove,target,Qt::NoButton,Qt::LeftButton);settle();
    mouse(source,QEvent::MouseButtonRelease,target,Qt::LeftButton,Qt::NoButton);settle();
}
inline void drag(QWidget* source,QPoint start,QPoint target,bool reorder=false){
    drag_to(source,start,[target]{return target;},reorder);
}
inline void tab_insert(WorkspaceDock* source,WorkspaceDock* destination,int index){
    auto* widget=source->tabWidget();
    drag_to(widget,widget->rect().center(),[destination,index]{return insertion(destination,index);});
}
inline void tab(WorkspaceDock* source,QPoint target,bool reorder=false){
    auto* widget=source->tabWidget();drag(widget,widget->rect().center(),target,reorder);
}
}
