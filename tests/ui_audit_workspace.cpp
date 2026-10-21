#include "isolated_gui_platform.h"
#include "ui_audit_recorder.h"
#include "workspace_drag_fixture.h"
#include <FloatingDockContainer.h>
#include <QAction>
#include <QContextMenuEvent>
#include <QDir>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenuBar>
#include <QTimer>
#include <QToolButton>
#include <QStringList>

namespace ui_audit {
namespace {
QJsonObject rectOf(const QRect& rect) {
    return {{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
}

void capturePopup(Recorder& recorder, const QString& id, QMenu* menu) {
    const QString relative = "captures/" + id + ".png";
    QDir directory(recorder.artifactDirectory());
    const bool created = directory.mkpath("captures");
    const bool saved = created && menu->grab().save(directory.filePath(relative), "PNG");
    recorder.evidence(id + ".popupImage", {{"path", relative}, {"saved", saved},
        {"source", "QWidget::grab of this offscreen fixture-owned live QMenu"},
        {"desktopCaptured", false}});
}

// Resolve inside the fixture's widget tree, never via the desktop or global
// cursor. Deliver to the actual deepest hit widget so an overlapping sibling
// cannot be bypassed by addressing the intended control directly.
void clickHit(Recorder& recorder, const QString& id, QWidget* target, QPoint position = {}) {
    workspace_drag::check(target && target->isVisible() && target->isEnabled(), "Input target is not visible/enabled");
    if (position.isNull()) position = target->rect().center();
    auto* window = target->window();
    const auto point = target->mapTo(window, position);
    auto* hit = window->childAt(point);
    if (!hit && window->rect().contains(point)) hit = window;
    const bool reachable = hit && (hit == target || target->isAncestorOf(hit));
    recorder.evidence(id + ".hit", {{"target", target->objectName()}, {"targetClass", target->metaObject()->className()},
        {"targetRect", rectOf(target->rect())}, {"visibleBounds", rectOf(target->visibleRegion().boundingRect())},
        {"positionX", position.x()}, {"positionY", position.y()},
        {"hit", hit ? hit->objectName() : QString()}, {"hitClass", hit ? hit->metaObject()->className() : "none"},
        {"hitBelongsToTarget", reachable}, {"visiblePoint", target->visibleRegion().contains(position)},
        {"dispatch", "QTest::mouseClick on fixture-tree hit widget"}});
    workspace_drag::check(reachable && target->visibleRegion().contains(position), "Control center is clipped or covered by another widget");
    QTest::mouseClick(hit, Qt::LeftButton, Qt::NoModifier, hit->mapFrom(window, point));
}

QStringList menuGeometry(QMenu* menu) {
    QStringList errors;
    QList<QRect> rectangles;
    for (const auto actions = menu->actions(); auto* action : actions) {
        if (!action->isVisible() || action->isSeparator()) continue;
        const auto rect = menu->actionGeometry(action);
        if (rect.isEmpty() || !menu->rect().contains(rect)) errors.append("Menu action is clipped: " + action->text());
        if (menu->actionAt(rect.center()) != action) errors.append("Menu action hit target differs: " + action->text());
        for (const auto& previous : rectangles)
            if (rect.intersects(previous)) errors.append("Menu action rectangles overlap: " + action->text());
        rectangles.append(rect);
    }
    return errors;
}

QStringList headerGeometry(Recorder& recorder, const QString& id, WorkspaceDock* dock) {
    QStringList errors;
    auto* header = dock->dockAreaWidget()->titleBar();
    QJsonArray items, tabs;
    QList<QRect> occupied;
    for (int index = 0; index < header->layout()->count(); ++index) {
        auto* widget = header->layout()->itemAt(index)->widget();
        if (!widget || !widget->isVisible()) continue;
        const QRect rect(widget->mapTo(header, QPoint{}), widget->size());
        const bool contained = header->rect().contains(rect);
        items.append(QJsonObject{{"id", widget->objectName()}, {"class", widget->metaObject()->className()},
            {"rect", rectOf(rect)}, {"containedInHeader", contained},
            {"visibleBounds", rectOf(widget->visibleRegion().boundingRect())}});
        if (!contained) errors.append("Header layout component is clipped: " + widget->objectName());
        for (const auto& previous : occupied)
            if (rect.intersects(previous)) errors.append("Header layout components overlap: " + widget->objectName());
        occupied.append(rect);
    }
    for (const auto peers = dock->dockAreaWidget()->dockWidgets(); auto* peer : peers) {
        auto* tab = peer->tabWidget();
        const auto center = tab->rect().center();
        tabs.append(QJsonObject{{"id", peer->objectName()}, {"text", tab->text()},
            {"tooltip", tab->toolTip()}, {"rectInHeader", rectOf(QRect(tab->mapTo(header, QPoint{}), tab->size()))},
            {"visibleBounds", rectOf(tab->visibleRegion().boundingRect())},
            {"centerVisible", tab->visibleRegion().contains(center)}, {"closed", peer->isClosed()}});
    }
    recorder.evidence(id + ".geometry", {{"headerRect", rectOf(header->rect())}, {"layoutComponents", items},
        {"tabs", tabs}, {"note", "Tab viewport clipping is recorded, not treated as a defect; selector must recover every open tab."}});
    return errors;
}

// QToolButton::InstantPopup enters a nested loop during mouse press. A fixture-
// owned timer observes that live popup and sends its input; a second timer is a
// bounded guard, not a substitute for the expected Escape/selection behavior.
void observe_tabs_popup(auto& observed,QMenu*& menu,auto& errors,Recorder& recorder,auto& id,auto& dock,auto& select,auto& keyboard) {

        observed = menu->isVisible();
        if (!observed) { errors.append("Mouse press did not open the tabs menu"); return; }
        recorder.evidence(id + ".livePopup", recorder.snapshot(menu));
        if (id == "workspace.tab-list") capturePopup(recorder, id, menu);
        errors.append(menuGeometry(menu));
        const auto peers = dock->dockAreaWidget()->dockWidgets();
        int open = 0;
        QAction* selected = nullptr;
        for (int index = 0; index < peers.size(); ++index) {
            if (peers[index]->isClosed()) continue;
            if (open >= menu->actions().size() || menu->actions().at(open)->data().toInt() != index ||
                menu->actions().at(open)->text() != peers[index]->tabWidget()->text())
                errors.append("Tab selector membership/order differs at open index " + QString::number(open));
            if (peers[index] == select && open < menu->actions().size()) selected = menu->actions().at(open);
            ++open;
        }
        if (menu->actions().size() != open) errors.append("Tab selector contains stale or missing entries");
        if (!select) QTest::keyClick(menu, Qt::Key_Escape);
        else if (!selected) { errors.append("Requested tab has no selector action"); QTest::keyClick(menu, Qt::Key_Escape); }
        else if (keyboard) {
            // Navigate from no active action through real key events.
            int steps = menu->actions().indexOf(selected) + 1;
            for (int index = 0; index < steps; ++index) QTest::keyClick(menu, Qt::Key_Down);
            if (menu->activeAction() != selected) errors.append("Down navigation reached the wrong selector action");
            QTest::keyClick(menu, Qt::Key_Return);
        } else {
            const auto rect = menu->actionGeometry(selected);
            recorder.evidence(id + ".actionHit", {{"rect", rectOf(rect)}, {"text", selected->text()},
                {"actionAtCenterMatches", menu->actionAt(rect.center()) == selected}, {"dispatch", "QTest::mouseClick(QMenu, action center)"}});
            QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, rect.center());
        }
    }

QStringList tabsInput(Recorder& recorder, const QString& id, WorkspaceDock* dock,
                      WorkspaceDock* select = nullptr, bool keyboard = false) {
    QStringList errors;
    auto* button = dock->dockAreaWidget()->titleBar()->findChild<QToolButton*>("tabsMenuButton");
    if (!button || !button->menu()) return {"Tabs selector is absent"};
    auto* menu = button->menu();
    QObject scope;
    bool observed = false, timedOut = false, triggered = false;
    QObject::connect(menu, &QMenu::triggered, &scope, [&](QAction*) { triggered = true; });
    QTimer::singleShot(500, &scope, [&] { if (menu->isVisible()) { timedOut = true; menu->close(); } });
    QTimer::singleShot(0, &scope, [&] {return observe_tabs_popup(observed,menu,errors,recorder,id,dock,select,keyboard);});
    clickHit(recorder, id, button);
    workspace_drag::settle();
    if (!observed) errors.append("Live popup was not observed");
    if (timedOut || menu->isVisible()) errors.append("Popup required guard dismissal or remained visible");
    if (select && (!triggered || dock->dockAreaWidget()->currentDockWidget() != select))
        errors.append("Selector input did not dispatch and select the requested tab");
    if (!select && triggered) errors.append("Escape dispatched an action");
    return errors;
}

void observe_context_popup(auto& tab,auto& observed,Recorder& recorder,auto& id,auto& errors,auto& close,auto& scope,auto& closeTriggered) {

        for(const auto children=tab->template findChildren<QMenu*>();auto* menu:children) if (menu->isVisible()) { observed = menu; break; }
        if (!observed) return;
        recorder.evidence(id + ".livePopup", recorder.snapshot(observed));
        if (id == "workspace.context-menu") capturePopup(recorder, id, observed);
        errors.append(menuGeometry(observed));
        if (!close) { QTest::keyClick(observed, Qt::Key_Escape); return; }
        QAction* command = nullptr;
        for(const auto children=observed->actions();auto* action:children) if (action->text() == tab->tr("Close")) command = action;
        if (!command || !command->isEnabled()) {
            errors.append("Context Close action is absent or disabled"); QTest::keyClick(observed, Qt::Key_Escape); return;
        }
        QObject::connect(command, &QAction::triggered, &scope, [&] { closeTriggered = true; });
        QTest::mouseClick(observed, Qt::LeftButton, Qt::NoModifier, observed->actionGeometry(command).center());
    }

QStringList contextInput(Recorder& recorder, const QString& id, WorkspaceDock* dock, bool close) {
    QStringList errors;
    auto* tab = dock->tabWidget();
    QObject scope;
    QPointer<QMenu> observed;
    bool timedOut = false, closeTriggered = false;
    QTimer::singleShot(500, &scope, [&] {
        for(const auto children=tab->template findChildren<QMenu*>();auto* menu:children)
            if (menu->isVisible()) { timedOut = true; menu->close(); }
    });
    QTimer::singleShot(0, &scope, [&] {return observe_context_popup(tab,observed,recorder,id,errors,close,scope,closeTriggered);});
    // QTest right-button events do not synthesize the platform context-menu
    // event on every QPA. Send that explicit Qt-local event, and label it so.
    QContextMenuEvent event(QContextMenuEvent::Mouse, tab->rect().center(), tab->mapToGlobal(tab->rect().center()));
    recorder.evidence(id + ".dispatch", {{"open", "QContextMenuEvent sent to fixture tab"},
        {"dismissOrSelect", close ? "QTest left click on actual Close action rectangle" : "QTest Escape on live menu"},
        {"nativeRightClickSynthesized", false}});
    QApplication::sendEvent(tab, &event);
    workspace_drag::settle();
    if (!observed) errors.append("Context event did not open a menu");
    if (timedOut || (observed && observed->isVisible())) errors.append("Context popup required guard dismissal or remained visible");
    if (close && (!closeTriggered || !dock->isClosed())) errors.append("Context Close click did not dispatch and close dock");
    return errors;
}

QJsonArray actionsOf(QMenu* menu) {
    QJsonArray result;
    if (!menu) return result;
    for (const auto actions = menu->actions(); auto* action : actions)
        result.append(QJsonObject{{"text", action->text()}, {"objectName", action->objectName()},
            {"enabled", action->isEnabled()}, {"checked", action->isChecked()},
            {"separator", action->isSeparator()}, {"tooltip", action->toolTip()},
            {"shortcut", action->shortcut().toString()}, {"data", QJsonValue::fromVariant(action->data())}});
    return result;
}

QJsonObject dockState(WorkspaceDock* dock) {
    QJsonArray order, buttons;
    auto* area = dock->dockAreaWidget();
    if (area) {
        for (const auto peers = area->dockWidgets(); auto* peer : peers)
            order.append(QJsonObject{{"id", peer->objectName()}, {"closed", peer->isClosed()},
                {"selected", area->currentDockWidget() == peer}});
        for(const auto children=area->titleBar()->findChildren<QToolButton*>();auto* button:children)
            buttons.append(QJsonObject{{"id", button->objectName()}, {"text", button->text()},
                {"accessibleName", button->accessibleName()}, {"tooltip", button->toolTip()},
                {"enabled", button->isEnabled()}, {"visible", button->isVisible()},
                {"menu", actionsOf(button->menu())}});
    }
    auto* tab = dock->tabWidget();
    return {{"id", dock->objectName()}, {"title", dock->windowTitle()},
        {"closed", dock->isClosed()}, {"floating", dock->isFloating()}, {"autoHide", dock->isAutoHide()},
        {"toggleChecked", dock->toggleViewAction()->isChecked()},
        {"tabText", tab ? tab->text() : QString()},
        {"tabAccessibleName", tab ? tab->accessibleName() : QString()},
        {"tabTooltip", tab ? tab->toolTip() : QString()},
        {"areaOrder", order}, {"headerButtons", buttons}};
}

QStringList menuViolations(WorkspaceDock* dock) {
    QStringList errors;
    auto* area = dock->dockAreaWidget();
    if (!area) return {QStringLiteral("Dock has no area")};
    auto* button = area->titleBar()->findChild<QToolButton*>("tabsMenuButton");
    if (!button || !button->menu()) return {QStringLiteral("Tab selector or its menu is missing")};
    auto* menu = button->menu();
    menu->setAttribute(Qt::WA_ShowWithoutActivating);
    menu->popup(button->mapToGlobal(QPoint(0, button->height())));
    workspace_drag::settle();
    const auto actions = menu->actions();
    const auto peers = area->dockWidgets();
    int open = 0;
    for (int index = 0; index < peers.size(); ++index) {
        if (peers[index]->isClosed()) continue;
        if (open >= actions.size() || actions[open]->data().toInt() != index ||
            actions[open]->text() != peers[index]->tabWidget()->text())
            errors.append(QStringLiteral("Tab selector membership/order differs at open index %1").arg(open));
        ++open;
    }
    if (actions.size() != open) errors.append(QStringLiteral("Tab selector contains missing or stale entries"));
    menu->hide();
    workspace_drag::settle();
    return errors;
}
}

void populate_workspace_overflow(WorkspaceWindow& overflow,auto& many,auto& content) {

            for (int index = 0; index < 12; ++index) {
                auto* dock = new WorkspaceDock(QString("Graph %1 — long title with distinct suffix %2").arg(index + 1).arg(index), &overflow);
                dock->setObjectName(QString("audit-overflow-%1").arg(index));
                auto* label = new QLabel(dock->windowTitle());
                dock->setWidget(label);
                overflow.addDockWidget(Qt::LeftDockWidgetArea, dock);
                if (!many.isEmpty()) overflow.tabifyDockWidget(many.front(), dock);
                many.append(dock); content.append(label);
            }
            many.front()->setAsCurrentTab(); overflow.show(); workspace_drag::settle();
        }

QStringList overflow_identity_errors(WorkspaceWindow& overflow,auto& many,auto& content) {

            QStringList errors;
            if (overflow.workspaceDocks().size() != many.size()) errors.append("Overflow registered dock count changed");
            for (int index = 0; index < many.size(); ++index)
                if (overflow.workspaceDock(many[index]->objectName()) != many[index] || many[index]->widget() != content[index] || many[index]->isClosed())
                    errors.append("Overflow tab identity/content/open state changed");
            return errors;
        }

void audit_workspace_overflow(Recorder& recorder,WorkspaceWindow& host,auto& popupErrors) {
    // A separate bounded fixture stresses overflow without changing the
    // identity oracle or saved-layout state of the three-dock fixture above.
    {
        WorkspaceWindow overflow;
        overflow.setObjectName("auditOverflowWorkspace");
        overflow.setAttribute(Qt::WA_ShowWithoutActivating);
        overflow.resize(460, 340);
        QList<WorkspaceDock*> many;
        QList<QWidget*> content;
        recorder.transition("workspace.overflow-setup", "Fixture setup creates twelve long-title tabs at compact width", &overflow, [&] {return populate_workspace_overflow(overflow,many,content);}, [&] { return many.size() == 12 ? QStringList{} : QStringList{"Overflow fixture setup is incomplete"}; });
        auto stable = [&] {return overflow_identity_errors(overflow,many,content);};
        recorder.transition("workspace.overflow-last-mouse", "Compact twelve-tab selector reaches the last long-title tab by mouse", &overflow, [&] {
            popupErrors = headerGeometry(recorder, "workspace.overflow-last-mouse", many.front());
            popupErrors.append(tabsInput(recorder, "workspace.overflow-last-mouse", many.front(), many.back()));
        }, [&] { auto errors = stable(); errors.append(popupErrors); return errors; }, "qt_input_mouse");
        recorder.transition("workspace.overflow-first-keyboard", "Reopened compact selector reaches first tab with Down/Return", &overflow, [&] {
            popupErrors = tabsInput(recorder, "workspace.overflow-first-keyboard", many.back(), many.front(), true);
        }, [&] { auto errors = stable(); errors.append(popupErrors); return errors; }, "qt_input_mouse_keyboard");
        recorder.transition("workspace.overflow-escape-reopen", "Repeated opening and Escape leaves twelve tabs and selection unchanged", &overflow, [&] {
            popupErrors = tabsInput(recorder, "workspace.overflow-escape-reopen.first", many.front());
            popupErrors.append(tabsInput(recorder, "workspace.overflow-escape-reopen.second", many.front()));
        }, [&] {
            auto errors = stable(); errors.append(popupErrors);
            if (many.front()->dockAreaWidget()->currentDockWidget() != many.front()) errors.append("Overflow Escape changed selected tab");
            return errors;
        }, "qt_input_mouse_keyboard");
        overflow.close(); workspace_drag::settle();
    }

    recorder.coverage("workspace.transitions", "covered", "Recorded fixture tab group/split, tab click, popup, context menu, selected close/reopen, reorder, Escape cancellation, floating/redock, pin and saved/malformed layout transitions. Per-transition violations determine success.");
    recorder.coverage("workspace.affordances", "partial", "Captured real ADS header buttons, text, tooltip, accessibleName, enabled/visible state, tab labels, selector entries and tab context commands. Fixture Placement action has no application behavior.");
    recorder.coverage("workspace.drag", "covered", "Qt-local mouse press/move/release reorder changes B from a non-first index to index zero with selection/identity preserved; tear-off preview Escape cancellation and floating-to-tab-group redock also have explicit state oracles. Fixture floating creation/positioning uses public APIs; native OS drag capture is not established. Per-transition violations determine success.");
    recorder.coverage("workspace.native-gestures", "not_covered", "Qt local events on offscreen fixtures do not establish native pointer capture, OS window frames, keyboard focus traversal, screen-reader behavior, multi-monitor/DPI behavior or hardware rendering.");
    recorder.coverage("workspace.overflow", "partial", "Two-tab narrow and twelve-tab compact long-title fixtures record header containment/overlap, tab viewport clipping, live popup action geometry and actual selector mouse/keyboard recovery. Native popup placement, extreme counts and visual elision readability remain unverified.");
    host.close();
    workspace_drag::settle();
}

void audit_workspace_layout(Recorder& recorder,WorkspaceWindow& host,WorkspaceDock*& a,WorkspaceDock*& b,auto& run,auto& popupErrors) {
    const auto saved = host.saveState(73);
    bool restored = false;
    run("workspace.layout-mutate", "Temporary layout mutation unpins A and closes B", [&] {
        a->setAutoHide(false); b->hide();
    }, [&] { return !a->isAutoHide() && b->isClosed() ? QStringList{} : QStringList{"Layout mutation did not take effect"}; });
    run("workspace.layout-restore", "Restoring saved layout restores pin/open states without recreating content", [&] {
        restored = host.restoreState(saved, 73);
    }, [&] { return restored && a->isAutoHide() && !b->isClosed() ? QStringList{} : QStringList{"Restore rejected valid layout or lost saved pin/open state"}; });
    bool acceptedInvalid = false;
    const auto beforeInvalid = host.saveState(73);
    run("workspace.invalid-layout", "Malformed layout is rejected without changing existing serialized layout", [&] {
        acceptedInvalid = host.restoreState(QByteArrayLiteral("invalid"), 73);
    }, [&] {
        QStringList errors;
        if (acceptedInvalid) errors.append("Malformed layout was accepted");
        if (beforeInvalid != host.saveState(73)) errors.append("Rejected malformed layout changed workspace state");
        return errors;
    });

audit_workspace_overflow(recorder,host,popupErrors);
}

void audit_workspace_redock(Recorder& recorder,WorkspaceWindow& host,WorkspaceDock*& a,WorkspaceDock*& b,auto& run,auto& popupErrors) {
    run("workspace.float", "Floating A detaches only A and preserves content", [&] { a->setFloating(true); }, [&] {
        return a->isFloating() && a->floatingDockContainer() && !b->isFloating() && a->dockAreaWidget() != b->dockAreaWidget()
            ? QStringList{} : QStringList{"Floating A moved its sibling or failed to create a separate host"};
    });
    QPointer<ads::CFloatingDockContainer> detachedHost = a->floatingDockContainer();
    run("workspace.redock-drag", "Dragging floating A into B at index zero selects A and releases old host", [&] {
        if (a->floatingDockContainer()) a->floatingDockContainer()->move(1100, 40);
        workspace_drag::settle();
        const auto start = a->tabWidget()->mapToGlobal(a->tabWidget()->rect().center());
        const auto target = workspace_drag::insertion(b, 0);
        recorder.evidence("workspace.redock-drag.inputRoute", {{"source", a->objectName()}, {"destination", b->objectName()},
            {"startX", start.x()}, {"startY", start.y()}, {"targetX", target.x()}, {"targetY", target.y()},
            {"dispatch", "QMouseEvent press, tear-off move with observed ADS preview, held-button moves to destination, release"},
            {"fixtureFloatingHostPositionedByApi", true}});
        workspace_drag::tab(a, target);
    }, [&] {
        QStringList errors;
        if (a->isFloating() || a->dockAreaWidget() != b->dockAreaWidget()) errors.append("Floating tab failed to join the destination group");
        if (a->dockAreaWidget()->dockWidgets().indexOf(a) != 0 || a->dockAreaWidget()->currentDockWidget() != a)
            errors.append("Redock lost insertion index or selection");
        if (!detachedHost.isNull()) errors.append("Old floating container remained alive after redock");
        errors.append(menuViolations(a));
        return errors;
    }, "qt_input_mouse");
    run("workspace.pin", "Pinning A creates an auto-hide sidebar without changing content", [&] {
        a->setAutoHide(true, ads::SideBarRight);
    }, [&] { return a->isAutoHide() ? QStringList{} : QStringList{"Pin did not create auto-hide state"}; });
audit_workspace_layout(recorder,host,a,b,run,popupErrors);
}

void audit_workspace_drag(Recorder& recorder,WorkspaceWindow& host,WorkspaceDock*& a,WorkspaceDock*& b,auto& run,auto& popupErrors) {
    auto* originalArea = a->dockAreaWidget();
    bool previewSeen = false;
    run("workspace.cancel-drag", "Escape cancels ADS tear-off preview and preserves the original tab group", [&] {
        auto* tab = a->tabWidget();
        const auto global = tab->mapToGlobal(tab->rect().center());
        workspace_drag::mouse(tab, QEvent::MouseButtonPress, global, Qt::LeftButton, Qt::LeftButton);
        workspace_drag::mouse(tab, QEvent::MouseMove, global + QPoint(0, -80), Qt::NoButton, Qt::LeftButton);
        workspace_drag::settle();
        for(const auto windows=QApplication::topLevelWidgets();auto* widget:windows)
            if (widget->inherits("ads::CFloatingDragPreview") && widget->isVisible()) previewSeen = true;
        QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&host, &cancel);
        workspace_drag::settle();
        workspace_drag::mouse(tab, QEvent::MouseButtonRelease, global, Qt::LeftButton, Qt::NoButton);
    }, [&] {
        QStringList errors;
        if (!previewSeen) errors.append("ADS tear-off preview was not observed before cancellation");
        if (a->isFloating() || a->dockAreaWidget() != originalArea) errors.append("Escape did not preserve original dock membership");
        for(const auto windows=QApplication::topLevelWidgets();auto* widget:windows)
            if (widget->inherits("ads::CFloatingDragPreview") && widget->isVisible()) errors.append("Drag preview remained visible after cancellation");
        return errors;
    }, "qt_input_mouse_keyboard");
audit_workspace_redock(recorder,host,a,b,run,popupErrors);
}

void audit_workspace_close_reorder(Recorder& recorder,WorkspaceWindow& host,WorkspaceDock*& a,WorkspaceDock*& b,auto& run,auto& popupErrors,auto& reopenByInput) {
    auto* closeButton = b->dockAreaWidget()->titleBar()->findChild<QToolButton*>("dockAreaCloseButton");
    if (closeButton && closeButton->isVisible() && closeButton->isEnabled()) {
        run("workspace.close-selected", "Area close button closes only selected B, preserving open A", [&] {
            QTest::mouseClick(closeButton, Qt::LeftButton);
        }, [&] {
            QStringList errors;
            if (!b->isClosed() || a->isClosed()) errors.append("Area close did not close only selected B");
            auto* selector = a->dockAreaWidget()->titleBar()->findChild<QToolButton*>("tabsMenuButton");
            if (!selector || !selector->isHidden()) errors.append("One-tab area retains the all-tabs selector");
            return errors;
        }, "qt_input_mouse");
    } else recorder.coverage("workspace.close-button", "not_covered", "Fixture area close control was not visible/enabled.");
    run("workspace.reopen", "Fixture View menu reopens B, keeping identity and tab group", [&] {
        popupErrors = reopenByInput("workspace.reopen");
    }, [&] {
        QStringList errors = popupErrors;
        if (b->isClosed() || b->dockAreaWidget() != a->dockAreaWidget()) errors.append("Reopened B lost its tab group");
        errors.append(menuViolations(b));
        return errors;
    }, "qt_input_mouse");
    int reorderBefore = -1;
    run("workspace.reorder", "Dragging B to first tab changes exact order and keeps B selected", [&] {
        reorderBefore = b->dockAreaWidget()->dockWidgets().indexOf(b);
        const auto start = b->tabWidget()->mapToGlobal(b->tabWidget()->rect().center());
        const auto target = workspace_drag::insertion(a, 0);
        recorder.evidence("workspace.reorder.inputRoute", {{"source", b->objectName()}, {"beforeIndex", reorderBefore},
            {"requestedIndex", 0}, {"startX", start.x()}, {"startY", start.y()},
            {"targetX", target.x()}, {"targetY", target.y()},
            {"dispatch", "QMouseEvent press, two held-button moves, release on fixture tab; no system cursor movement"}});
        workspace_drag::tab(b, target, true);
    }, [&] {
        QStringList errors;
        if (reorderBefore <= 0) errors.append("Reorder fixture did not start with B after the first tab");
        if (b->dockAreaWidget()->dockWidgets().indexOf(b) != 0 || b->dockAreaWidget()->currentDockWidget() != b)
            errors.append("Reorder lost requested insertion index or selected tab");
        errors.append(menuViolations(b));
        return errors;
    }, "qt_input_mouse");
audit_workspace_drag(recorder,host,a,b,run,popupErrors);
}

void audit_workspace_narrow_tabs(Recorder& recorder,WorkspaceWindow& host,WorkspaceDock*& a,WorkspaceDock*& b,auto& inventory,auto& run,auto& popupErrors,auto& reopenByInput) {
    run("workspace.narrow-tabs", "Long tab titles remain represented in the all-tabs list when the fixture is narrowed", [&] {
        a->setWindowTitle("Graph A — a deliberately long audit title that exceeds a compact tab");
        b->setWindowTitle("Graph B — another deliberately long audit title for compact tab overflow");
        host.resize(440, 700);
    }, [&] { return menuViolations(b); });
    recorder.evidence("workspace.narrowAffordances", inventory());
    run("workspace.narrow-selector-input", "Narrow long-title area keeps a reachable selector and selects A from the live popup", [&] {
        popupErrors = headerGeometry(recorder, "workspace.narrow-selector-input", b);
        popupErrors.append(tabsInput(recorder, "workspace.narrow-selector-input", b, a));
    }, [&] { return popupErrors; }, "qt_input_mouse");
    run("workspace.narrow-selector-keyboard", "Narrow popup reopens and selects B using keyboard navigation", [&] {
        popupErrors = tabsInput(recorder, "workspace.narrow-selector-keyboard", a, b, true);
    }, [&] { return popupErrors; }, "qt_input_mouse_keyboard");
    run("workspace.restore-tab-titles", "Restoring normal titles and window size keeps tab membership and selection", [&] {
        a->setWindowTitle("Graph A"); b->setWindowTitle("Graph B"); host.resize(1000, 700);
    }, [&] { return b->dockAreaWidget()->currentDockWidget() == b ? QStringList{} : QStringList{"Title/size change switched the selected tab"}; });

audit_workspace_close_reorder(recorder,host,a,b,run,popupErrors,reopenByInput);
}

void audit_workspace_view_menu(Recorder& recorder,WorkspaceWindow& host,WorkspaceDock*& a,WorkspaceDock*& b,auto& inventory,auto& run,auto& popupErrors) {
    // This isolated fixture supplies a View menu containing the production
    // toggle action. It establishes action routing/ownership, not application
    // menu discovery (covered by the application probe).
    auto* viewMenu = host.menuBar()->addMenu("Audit fixture View");
    viewMenu->setObjectName("auditFixtureViewMenu");
    viewMenu->addAction(a->toggleViewAction()); viewMenu->addAction(b->toggleViewAction());
    auto reopenByInput = [&](const QString& id) {
        QStringList errors;
        QObject scope;
        bool observed = false, triggered = false, timedOut = false;
        QObject::connect(b->toggleViewAction(), &QAction::triggered, &scope, [&] { triggered = true; });
        QTimer::singleShot(500, &scope, [&] { if (viewMenu->isVisible()) { timedOut = true; viewMenu->close(); } });
        QTimer::singleShot(0, &scope, [&] {
            observed = viewMenu->isVisible();
            if (!observed) return;
            recorder.evidence(id + ".livePopup", recorder.snapshot(viewMenu));
            errors.append(menuGeometry(viewMenu));
            QTest::mouseClick(viewMenu, Qt::LeftButton, Qt::NoModifier, viewMenu->actionGeometry(b->toggleViewAction()).center());
        });
        clickHit(recorder, id, host.menuBar(), host.menuBar()->actionGeometry(viewMenu->menuAction()).center());
        workspace_drag::settle();
        if (!observed || !triggered || b->isClosed()) errors.append("Fixture View menu click did not dispatch and reopen B");
        if (timedOut || viewMenu->isVisible()) errors.append("Fixture View popup required guard dismissal or remained visible");
        return errors;
    };
    run("workspace.context-close", "Reopened context menu Close click closes selected B while preserving A", [&] {
        popupErrors = contextInput(recorder, "workspace.context-close", b, true);
    }, [&] {
        auto errors = popupErrors;
        if (a->isClosed()) errors.append("Context Close also closed sibling A");
        return errors;
    }, "qt_context_event_mouse");
    run("workspace.context-reopen", "Fixture View menu reopens B through the production toggle action", [&] {
        popupErrors = reopenByInput("workspace.context-reopen");
    }, [&] { return popupErrors; }, "qt_input_mouse");

audit_workspace_narrow_tabs(recorder,host,a,b,inventory,run,popupErrors,reopenByInput);
}

void audit_workspace_popups(Recorder& recorder,WorkspaceWindow& host,WorkspaceDock*& a,WorkspaceDock*& b,auto& inventory,auto& run) {
    QStringList popupErrors;
    run("workspace.tab-list", "Mouse opens all-tabs popup; Escape dismisses without selection", [&] {
        popupErrors = tabsInput(recorder, "workspace.tab-list", b);
    }, [&] { return popupErrors; }, "qt_input_mouse_keyboard");
    run("workspace.tab-list-mouse-select", "Reopened tabs popup selects A through its action hit rectangle", [&] {
        popupErrors = tabsInput(recorder, "workspace.tab-list-mouse-select", b, a);
    }, [&] { return popupErrors; }, "qt_input_mouse");
    run("workspace.tab-list-keyboard-select", "Reopened tabs popup selects B through Down and Return", [&] {
        popupErrors = tabsInput(recorder, "workspace.tab-list-keyboard-select", a, b, true);
    }, [&] { return popupErrors; }, "qt_input_mouse_keyboard");
    recorder.evidence("workspace.initialAffordances", inventory());
    QJsonArray explicitNameReview;
    for(const auto children=b->dockAreaWidget()->titleBar()->findChildren<QToolButton*>();auto* button:children)
        if (button->isVisible() && button->accessibleName().isEmpty())
            explicitNameReview.append(QJsonObject{{"id", button->objectName()}, {"text", button->text()}, {"tooltip", button->toolTip()}});
    if (!explicitNameReview.isEmpty())
        recorder.finding("workspace.explicit-accessible-names", "review", "ux_heuristic",
            "Some visible tab/header buttons have empty explicit accessibleName properties.",
            "Review effective QAccessible names with a screen reader; tooltip/text fallback may already provide names.",
            {{"controls", explicitNameReview}, {"screenReaderTested", false}});

    run("workspace.context-menu", "Qt-local context event opens tab commands; Escape dismisses without changing selection", [&] {
        popupErrors = contextInput(recorder, "workspace.context-menu", b, false);
    }, [&] {
        QStringList errors = popupErrors;
        if (b->dockAreaWidget()->currentDockWidget() != b) errors.append("Opening/dismissing context menu changed the selected tab");
        return errors;
    }, "qt_context_event_keyboard");

audit_workspace_view_menu(recorder,host,a,b,inventory,run,popupErrors);
}

void audit_workspace_headers(Recorder& recorder,WorkspaceWindow& host,WorkspaceDock*& a,WorkspaceDock*& b,auto& inventory,auto& run) {
    auto* controlsA = new QLabel("A header controls", a);
    controlsA->setObjectName("auditHeaderControlsA");
    auto* controlsB = new QLabel("B header controls", b);
    controlsB->setObjectName("auditHeaderControlsB");
    a->setHeaderControls(controlsA);
    b->setHeaderControls(controlsB);
    a->setRenderFPS(17, true);
    b->setRenderFPS(23, true);
    workspace_drag::settle();
    run("workspace.select-header-owner", "Selecting A transfers header controls and FPS to A and parks B controls", [&] {
        clickHit(recorder, "workspace.select-header-owner", a->tabWidget());
    }, [&] {
        QStringList errors;
        auto* header = a->dockAreaWidget()->titleBar();
        auto* fps = header->findChild<QLabel*>("graphRenderStatus");
        if (controlsA->parentWidget() != header || !controlsA->isVisible() || !controlsB->isHidden())
            errors.append("Selected header controls did not transfer or inactive controls stayed visible");
        if (!fps || !fps->isVisible() || fps->text() != "17 FPS") errors.append("Header FPS does not belong to selected A");
        return errors;
    }, "qt_input_mouse");
    run("workspace.select-header-owner-back", "Selecting B restores its header controls and FPS", [&] {
        clickHit(recorder, "workspace.select-header-owner-back", b->tabWidget());
    }, [&] {
        QStringList errors;
        auto* header = b->dockAreaWidget()->titleBar();
        auto* fps = header->findChild<QLabel*>("graphRenderStatus");
        if (controlsB->parentWidget() != header || !controlsB->isVisible() || !controlsA->isHidden())
            errors.append("B header controls were not restored or A controls stayed visible");
        if (!fps || !fps->isVisible() || fps->text() != "23 FPS") errors.append("Header FPS does not belong to selected B");
        return errors;
    }, "qt_input_mouse");
audit_workspace_popups(recorder,host,a,b,inventory,run);
}

QStringList workspace_identity_errors(WorkspaceWindow& host,auto& docks,auto& contents) {

        QStringList result;
        if (host.workspaceDocks().size() != docks.size()) result.append("Registered dock count changed");
        for (int i = 0; i < docks.size(); ++i) {
            auto* dock = docks[i];
            if (host.workspaceDock(dock->objectName()) != dock) result.append("Dock identity disappeared: " + dock->objectName());
            if (dock->widget() != contents[i] || contents[i]->property("auditPersistentState").toString() != dock->objectName())
                result.append("Content identity/state changed: " + dock->objectName());
            if (dock->toggleViewAction()->isChecked() == dock->isClosed())
                result.append("Toggle action checked state disagrees with open state: " + dock->objectName());
        }
        return result;
    }

void drive_workspace_fixture(Recorder& recorder,WorkspaceWindow& host,WorkspaceDock*& a,WorkspaceDock*& b,WorkspaceDock*& flow,auto& docks,auto& inventory,auto& identityErrors) {
    auto run = [&](const QString& id, const QString& intent, const std::function<void()>& action,
                   const std::function<QStringList()>& oracle, const QString& dispatch = "public_api") {
        QString actionError;
        QStringList violations;
        recorder.evidence(id + ".beforeDockState", inventory());
        recorder.transition(id, intent, &host, [&] {
            try { action(); workspace_drag::settle(); }
            catch (const std::exception& error) { actionError = QString::fromUtf8(error.what()); throw; }
        }, [&] {
            violations = identityErrors();
            violations.append(oracle());
            return violations;
        }, dispatch);
        auto after = inventory();
        after["actionException"] = actionError;
        recorder.evidence(id + ".afterDockState", after);
        for (auto* dock : docks)
            if (dock->isFloating() && dock->floatingDockContainer())
                recorder.evidence(id + ".floating." + dock->objectName(), recorder.snapshot(dock->floatingDockContainer()));
        if (!actionError.isEmpty())
            recorder.coverage(id, "partial", "Probe action could not complete: " + actionError);
    };
    host.show();
    workspace_drag::settle();
    run("workspace.group", "Tabifying B with A preserves all registered docks and content", [&] {
        host.tabifyDockWidget(a, b); host.splitDockWidget(a, flow, Qt::Vertical); a->setAsCurrentTab();
    }, [&] { return a->dockAreaWidget() == b->dockAreaWidget() && a->dockAreaWidget() != flow->dockAreaWidget()
        ? QStringList{} : QStringList{"Tab group/split membership is incorrect"}; });
    run("workspace.select", "Click B tab selects B and preserves A", [&] {
        clickHit(recorder, "workspace.select", b->tabWidget());
    }, [&] { return b->dockAreaWidget()->currentDockWidget() == b ? QStringList{} : QStringList{"Clicked tab was not selected"}; }, "qt_input_mouse");
audit_workspace_headers(recorder,host,a,b,inventory,run);
}

WorkspaceDock* make_workspace_dock(WorkspaceWindow& host,auto& id,auto& title) {

        auto* dock = new WorkspaceDock(title, &host);
        dock->setObjectName(id);
        auto* content = new QLabel(title);
        content->setObjectName(id + "Content");
        content->setProperty("auditPersistentState", id);
        dock->setWidget(content);
        // Fixture action tests forwarding/ownership only. Application command semantics
        // belong to the application probe, not this isolated docking adapter fixture.
        auto* placement = new QAction("Placement", dock);
        placement->setObjectName("graphPlacement");
        dock->setTitleBarActions({placement});
        host.addDockWidget(Qt::LeftDockWidgetArea, dock);
        return dock;
    }

void audit_workspace(Recorder& recorder) {
    if (!test_fixture::isolated_gui_platform()) {
        recorder.coverage("workspace", "not_covered", "Workspace probe requires the offscreen platform; no desktop input is permitted.");
        return;
    }
    WorkspaceWindow host;
    host.setObjectName("auditWorkspaceHost");
    host.setAttribute(Qt::WA_ShowWithoutActivating);
    host.resize(1000, 700);
    auto make = [&](const QString& id, const QString& title) {return make_workspace_dock(host,id,title);};
    auto* a = make("audit-graph-a", "Graph A");
    auto* b = make("audit-graph-b", "Graph B");
    auto* flow = make("audit-data-flow", "Data flow");
    const QList<WorkspaceDock*> docks{a, b, flow};
    const QList<QWidget*> contents{a->widget(), b->widget(), flow->widget()};
    auto inventory = [&] {
        QJsonArray state;
        for (auto* dock : docks) state.append(dockState(dock));
        return QJsonObject{{"docks", state}, {"registeredCount", host.workspaceDocks().size()}};
    };
    auto identityErrors = [&] {return workspace_identity_errors(host,docks,contents);};
drive_workspace_fixture(recorder,host,a,b,flow,docks,inventory,identityErrors);
}
}
