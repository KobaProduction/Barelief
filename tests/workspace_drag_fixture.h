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
inline void tab_list(WorkspaceDock* dock){
    auto* area=dock->dockAreaWidget();
    auto* button=area->titleBar()->findChild<QToolButton*>("tabsMenuButton");
    check(button && button->isVisible() && button->menu(),"Tab list button missing");
    auto* menu=button->menu();menu->popup(button->mapToGlobal(QPoint(0,button->height())));settle();
    const auto actions=menu->actions();const auto docks=area->dockWidgets();int open=0;
    for(int i=0;i<docks.size();++i)if(!docks[i]->isClosed()){
        check(open<actions.size() && actions[open]->data().toInt()==i && actions[open]->text()==docks[i]->tabWidget()->text(),"Tab list lost membership/order");++open;
    }
    check(actions.size()==open,"Tab list retained stale entries");menu->hide();settle();
}
// All events belong to this isolated app. Never move the system cursor or
// simulate a successful result with addDockWidget/dropWidget calls.
inline void drag(QWidget* source,QPoint start,QPoint target,bool reorder=false){
    check(source && source->isVisible(),"Drag source is not visible");
    const auto global=source->mapToGlobal(start);
    mouse(source,QEvent::MouseButtonPress,global,Qt::LeftButton,Qt::LeftButton);
    if(!reorder) {
        mouse(source,QEvent::MouseMove,global+QPoint(0,-80),Qt::NoButton,Qt::LeftButton);settle();
        bool preview=false;for(auto* widget:QApplication::topLevelWidgets())
            if(widget->inherits("ads::CFloatingDragPreview") && widget->isVisible())preview=true;
        check(preview,"Qt drag did not enter the ADS preview path");
    }
    mouse(source,QEvent::MouseMove,target,Qt::NoButton,Qt::LeftButton);
    // The first horizontal movement starts tab reordering; a second moves it.
    mouse(source,QEvent::MouseMove,target,Qt::NoButton,Qt::LeftButton);settle();
    mouse(source,QEvent::MouseButtonRelease,target,Qt::LeftButton,Qt::NoButton);settle();
}
inline void tab(WorkspaceDock* source,QPoint target,bool reorder=false){
    auto* widget=source->tabWidget();drag(widget,widget->rect().center(),target,reorder);
}
}
