#include "core_app/main_window.h"
#include "core_app/graph_panel.h"
#include "core_app/settings_widget.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "visualization/ui_text.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMoveEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWindow>
#include <QResizeEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QStyle>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QToolBar>
#include <algorithm>
#include <FloatingDockContainer.h>
#include <QSet>

Q_DECLARE_LOGGING_CATEGORY(core_app_window)

SchemeEditorWidget *MainWindow::data_flow_editor() const {
    auto *dock=_panels.value("dataFlow");
    return dock ? qobject_cast<SchemeEditorWidget *>(dock->widget()) : nullptr;
}
WorkspaceDock *MainWindow::add_panel(const QString &id, const QString &title, QWidget *view,
                                   bool UIApplicationConfig::*flag) {
    auto *dock = new WorkspaceDock(title, this);
    dock->setObjectName(id + "Dock");
    dock->setFeatures(WorkspaceDock::DockWidgetClosable | WorkspaceDock::DockWidgetMovable |
                      WorkspaceDock::DockWidgetFloatable);
    ui::bind(dock, "windowTitle", title);
    ui::bind(dock->toggleViewAction(), "text", title);
    if (auto *chart = dynamic_cast<AbstractChartView *>(view)) {
        auto *panel =
            new GraphPanel(id, chart, _application.state().file_configs.ui.graph_views.value(id).toObject(), dock);
        connect(panel, &GraphPanel::presentation_changed, this, [this](const QString &id, const QJsonObject &prefs) {
            auto saved = _application.state().file_configs.ui.graph_views.value(id).toObject();
            for (auto it = prefs.constBegin(); it != prefs.constEnd(); ++it)
                saved[it.key()] = it.value();
            _application.state().file_configs.ui.graph_views[id] = saved;
            _workspace_save_timer.start();
        });
        dock->setWidget(panel);
    } else
        dock->setWidget(view);
    dock->installEventFilter(this);
    _panels[id] = dock;
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    if (id != "dataFlow") setup_graph_dock_controls(dock);
    connect(dock, &WorkspaceDock::viewToggled, this, [this, flag, id](bool open) {
        if (_restoring_layout)
            return;
        _application.state().file_configs.ui.*flag = open;
        const QMap<QString, QString> controls{{"rawSamples", "showRawSamples"},
                                              {"windowSignal", "showWindowSignal"},
                                              {"windowedSignal", "showWindowedSignal"},
                                              {"windowFunction", "showWindowFunction"},
                                              {"fft", "showFFT"},
                                              {"mel", "showMel"},
                                              {"spectrogram", "showSpectrogram"}};
        if (controls.contains(id)) {
            const auto name = controls.value(id);
            if (auto *checkbox = findChild<QCheckBox *>(name)) {
                const QSignalBlocker blocker(checkbox);
                checkbox->setChecked(open);
            }
            if (auto *action = findChild<QAction *>("toggle_" + name)) {
                const QSignalBlocker blocker(action);
                action->setChecked(open);
            }
        }
        _workspace_save_timer.start();
        refresh_subscriptions();
    });
    connect(dock, &WorkspaceDock::visibilityChanged, this, [this, id](bool visible) {
        set_graph_exposed(id, _panels[id], visible);
        refresh_subscriptions();
        if (!_restoring_layout)
            _workspace_save_timer.start();
    });
    connect(dock, &WorkspaceDock::topLevelChanged, this, [this] {
        if (!_restoring_layout)
            _workspace_save_timer.start();
    });
    connect(dock, &WorkspaceDock::dockLocationChanged, this, [this] {
        if (!_restoring_layout)
            _workspace_save_timer.start();
    });
    return dock;
}
void MainWindow::reset_workspace() {
    const bool restoring = _restoring_layout;
    _restoring_layout = true;
    if (!_default_workspace_state.isEmpty()) {
        // Let ADS restore its tab groups atomically instead of removing and reusing
        // docks while old floating/tab layouts still refer to them.
        restoreState(_default_workspace_state, 1);
    } else {
        for (auto *dock : _panels)
            removeDockWidget(dock);
        addDockWidget(Qt::LeftDockWidgetArea, _panels["spectrogram"]);
        splitDockWidget(_panels["spectrogram"], _panels["dataFlow"], Qt::Vertical);
        for (const auto &id : {"fft", "mel", "rawSamples", "windowSignal", "windowedSignal", "windowFunction"}) {
            addDockWidget(Qt::LeftDockWidgetArea, _panels[id]);
            tabifyDockWidget(_panels["spectrogram"], _panels[id]);
        }
    }
    auto &ui = _application.state().file_configs.ui;
    ui.show_spectrogram = ui.show_data_flow = true;
    ui.show_amplitudes = ui.show_test_amplitudes = ui.show_raw_samples = ui.show_window_signal =
        ui.show_windowed_signal = ui.show_window_function = false;
    update_chart_visibility(ui);
    _panels["spectrogram"]->raise();
    resizeDocks({_panels["spectrogram"], _panels["dataFlow"]}, {560, 320}, Qt::Vertical);
    if (_default_workspace_state.isEmpty())
        _default_workspace_state = saveState(1);
    _restoring_layout = restoring;
    refresh_subscriptions();
}
void MainWindow::save_workspace() {
    QJsonObject visibility;
    for (auto it = _panels.begin(); it != _panels.end(); ++it)
        visibility[it.key()] = !it.value()->isHidden();
    auto &ui = _application.state().file_configs.ui;
    for (auto it = _node_views.begin(); it != _node_views.end(); ++it) {
        const bool open = !it.value().dock->isHidden();
        visibility["nodeView_" + it.key()] = open;
        auto p = ui.graph_views[it.key()].toObject();
        p["open"] = open;
        ui.graph_views[it.key()] = p;
    }
    ui.show_spectrogram = visibility["spectrogram"].toBool();
    ui.show_data_flow = visibility["dataFlow"].toBool();
    ui.show_amplitudes = visibility["fft"].toBool();
    ui.show_test_amplitudes = visibility["mel"].toBool();
    ui.show_raw_samples = visibility["rawSamples"].toBool();
    ui.show_window_signal = visibility["windowSignal"].toBool();
    ui.show_windowed_signal = visibility["windowedSignal"].toBool();
    ui.show_window_function = visibility["windowFunction"].toBool();
    auto workspace = ui.workspace;
    workspace["version"] = 1;
    workspace["layout_version"] = 5;
    workspace["backend"] = "qt-ads";
    workspace["geometry"] = QString::fromLatin1(saveGeometry().toBase64());
    workspace["ads_state"] = QString::fromLatin1(saveState(1).toBase64());
    workspace["visibility"] = visibility;
    ui.workspace = workspace;
}
void MainWindow::refresh_subscriptions() {
    if (_restoring_layout)
        return;
    quint32 mask = 0;
    const auto visible = [this](const QString &id) {
        return (id == "dataFlow" || _active_node_views.values().contains(id)) && _render_visible.value(id) &&
               !_panels[id]->isHidden();
    };
    if (visible("dataFlow"))
        mask |= pipeline::EngineCommand::Previews;
    if (visible("rawSamples"))
        mask |= pipeline::EngineCommand::Views;
    if (visible("spectrogram"))
        mask |= pipeline::EngineCommand::History | pipeline::EngineCommand::Views;
    for (const auto &id : {"fft", "mel", "windowSignal", "windowedSignal", "windowFunction"})
        if (visible(id))
            mask |= pipeline::EngineCommand::Views;
    for (const auto &id : {"windowSignal", "windowedSignal", "windowFunction"})
        if (visible(id))
            mask |= pipeline::EngineCommand::Diagnostics;
    for (auto it = _node_views.cbegin(); it != _node_views.cend(); ++it) {
        const auto &view = it.value();
        if (view.device == _application.device_id() && view.graph == _application.graph_id() &&
            _active_node_views.contains(view.node) && _render_visible.value(it.key())) {
            mask |= pipeline::EngineCommand::Views;
            if (view.spectrogram)
                mask |= pipeline::EngineCommand::History;
        }
    }
    _application.set_visible_views(mask);
}
void MainWindow::recover_offscreen_panels() {
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return;
    QSet<QWidget *> visited;
    for (auto *panel : workspaceDocks())
        if (auto *dock = panel->floatingDockContainer(); dock && !visited.contains(dock)) {
            visited.insert(dock);
            QScreen *target = nullptr;
            const auto title = QRect(dock->frameGeometry().topLeft(), QSize(dock->width(), 30));
            for (auto *screen : screens)
                if (screen->availableGeometry().contains(title.center())) {
                    target = screen;
                    break;
                }
            if (!target)
                target = screens.front();
            const auto available = target->availableGeometry().adjusted(8, 8, -8, -8);
            dock->resize(dock->size().boundedTo(available.size()));
            auto position = dock->frameGeometry().topLeft();
            position.setX(
                std::clamp(position.x(), available.left(),
                           std::max(available.left(), available.right() - dock->frameGeometry().width() + 1)));
            position.setY(
                std::clamp(position.y(), available.top(),
                           std::max(available.top(), available.bottom() - dock->frameGeometry().height() + 1)));
            dock->move(position);
        }
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (!_restoring_layout && (event->type()==QEvent::MouseButtonPress || event->type()==QEvent::FocusIn)) {
        for(auto *widget=qobject_cast<QWidget *>(watched);widget;widget=widget->parentWidget()) {
            if(auto *dock=qobject_cast<WorkspaceDock *>(widget)) {
                if(is_graph_dock(dock) && in_editor_window(dock) && !dock->isHidden()) _last_graph_slot=dock;
                break;
            }
        }
    }
    if (!_restoring_layout && qobject_cast<WorkspaceDock *>(watched) &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize))
        _workspace_save_timer.start();
    return QMainWindow::eventFilter(watched, event);
}
void MainWindow::closeEvent(QCloseEvent *event) {
    _workspace_save_timer.stop();
    _camera_save_timer.stop();
    if (auto *editor = data_flow_editor())
        editor->flush_presentation();
    save_workspace();
    if (_application.state().file_configs.ui.autosave_enabled)
        emit _application.on_config_updated();
    _workspace_closed = true;
    _restoring_layout = true;
    QMainWindow::closeEvent(event);
}
void MainWindow::moveEvent(QMoveEvent *event) {
    QMainWindow::moveEvent(event);
    apply_render_rate();
    if (!_restoring_layout)
        _workspace_save_timer.start();
}
void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    if (!_restoring_layout)
        _workspace_save_timer.start();
}
WorkspaceDock *MainWindow::graph_dock(int node) const {
    const auto key=_active_node_views.value(node);
    return _panels.contains(key) ? _panels.value(key) : _node_views.value(key).dock;
}
void MainWindow::open_graph_node(int node) {
    auto *dock=graph_dock(node);
    if(!dock) return;
    auto *slot=_last_graph_slot.data();
    if(!slot || slot->isHidden() || !is_graph_dock(slot) || !in_editor_window(slot)) {
        slot=nullptr;
        for(auto it=_active_node_views.cbegin();it!=_active_node_views.cend();++it)
            if(_render_visible.value(it.value()) && in_editor_window(graph_dock(it.key()))) { slot=graph_dock(it.key()); break; }
        if(!slot) for(auto *candidate:graph_docks())
            if(in_editor_window(candidate) && candidate->property("renderExposed").toBool()) { slot=candidate; break; }
    }
    if(!slot) return; // Only an explicit context command creates a viewing area.
    for(auto *candidate:graph_docks())
        if(candidate->property("graphNode").toInt()==node && in_editor_window(candidate) && !candidate->isHidden()) {
            candidate->raise(); _last_graph_slot=candidate; return;
        }
    if(dock->isFloating()) dock=new_graph_view(node);
    if(!dock) return;
    if(slot!=dock) {
        if(!tabifiedDockWidgets(slot).contains(dock)) {
            dock->setFloating(false);
            tabifyDockWidget(slot,dock);
            slot->hide(); // Replace, rather than add, a visible tab in this area.
        }
    }
    if(auto *surface=qobject_cast<SpectrogramGraph *>(dock->widget()))
        surface->initialize({240,180},screen()->availableGeometry().size());
    dock->show();dock->raise();
    _last_graph_slot=dock;
    // Activation from the editor must not steal keyboard focus during dragging
    // or prevent subsequent copy/paste shortcuts from reaching the editor.
    _workspace_save_timer.start();refresh_subscriptions();
}
bool MainWindow::is_graph_dock(WorkspaceDock *dock) const {
    if(!dock) return false;
    for(auto it=_active_node_views.cbegin();it!=_active_node_views.cend();++it)
        if(graph_dock(it.key())==dock) return true;
    for(const auto &view:_node_views) if(view.dock==dock) return true;
    return false;
}
QList<WorkspaceDock *> MainWindow::graph_docks() const {
    QList<WorkspaceDock *> result;
    for(int node:_active_node_views.keys()) if(auto *dock=graph_dock(node)) result.append(dock);
    for(const auto &view:_node_views)
        if(view.device==_application.device_id() && view.graph==_application.graph_id() && _active_node_views.contains(view.node)
           && !result.contains(view.dock)) result.append(view.dock);
    return result;
}
bool MainWindow::in_editor_window(WorkspaceDock *dock) const {
    auto *flow=_panels.value("dataFlow");
    return dock && flow && dock->sharesHost(flow);
}
WorkspaceDock *MainWindow::new_graph_view(int node) {
    for(const auto &value:_application.state().file_configs.scene_graph.value("nodes").toArray()) {
        const auto entry=value.toObject();
        if(entry["id"].toInt()==node) {
            const auto kind=entry["internal-data"].toObject()["model-name"].toString();
            const auto key=ensure_node_view(node,kind,_application.device_id(),_application.graph_id(),false,_next_view_instance++);
            return _node_views.value(key).dock;
        }
    }
    return nullptr;
}
void MainWindow::populate_graph_menu(int node,QMenu *menu) {
    auto *focus=menu->addMenu(ui::text("Focus representation")); focus->setObjectName("graphRepresentations");
    int internal=0,external=0;
    for(auto *dock:graph_docks()) {
        if(dock->isHidden() || dock->property("graphNode").toInt()!=node) continue;
        const bool outside=dock->isFloating();
        auto *action=focus->addAction(ui::text(outside ? "External window %1" : "Internal area %1")
                                     .arg(outside ? ++external : ++internal));
        connect(action,&QAction::triggered,this,[this,dock] {
            dock->show(); dock->raise(); dock->activateWindow(); dock->widget()->setFocus();
            if(in_editor_window(dock)) _last_graph_slot=dock;
        });
    }
    focus->setEnabled(!focus->isEmpty());
    const char *labels[]{"Open in separate window","Open in new tab","Open in this window"};
    for(int placement=0;placement<3;++placement) {
        auto *action=menu->addAction(ui::text(labels[placement]));
        action->setObjectName(QStringLiteral("openGraphPlacement%1").arg(placement));
        if(placement<2) { action->setShortcut(QKeySequence(placement ? "Alt+G" : "Alt+Shift+G")); action->setShortcutVisibleInContextMenu(true); }
        connect(action,&QAction::triggered,this,[this,node,placement] { add_graph_node(node,placement); });
    }
    auto *beside=menu->addMenu(ui::text("Open beside")); beside->setObjectName("graphPlacementTargets");
    int index=0;
    for(auto *target:graph_docks()) if(in_editor_window(target) && !target->isHidden()) {
        auto *action=beside->addAction(ui::text("Internal area %1").arg(++index)+": "+target->windowTitle());
        connect(action,&QAction::triggered,this,[this,node,target] { _last_graph_slot=target; add_graph_node(node,2); });
    }
    beside->setEnabled(!beside->isEmpty());
}
void MainWindow::add_graph_node(int node,int placement) {
    if(auto *dock=graph_dock(node)) {
        if(dock->isFloating() && !dock->isHidden()) dock=new_graph_view(node);
        else if(placement==2 && dock->property("renderExposed").toBool()) dock=new_graph_view(node);
        if(!dock) return;
        place_graph(dock,placement);
        if(in_editor_window(dock)) _last_graph_slot=dock;
    }
}
void MainWindow::set_graph_exposed(const QString &key, WorkspaceDock *dock, bool exposed) {
    _render_visible[key] = exposed;
    dock->setProperty("renderExposed", exposed);
    if (auto *panel = qobject_cast<GraphPanel *>(dock->widget())) panel->set_render_visible(exposed);
    if (auto *surface = qobject_cast<SpectrogramGraph *>(dock->widget())) surface->set_render_visible(exposed);
    for (auto *view : dock->findChildren<QChartView *>())
        if (auto *chart = dynamic_cast<AbstractChartView *>(view)) chart->set_render_visible(exposed);
}
void MainWindow::rebuild_graph_areas(WorkspaceDock *dock,WorkspaceDock *anchor,int placement) {
    auto *flow=_panels.value("dataFlow");
    const auto position=_application.state().file_configs.ui.workspace.value("data_flow_position").toString("bottom");
    if(!dock) {
        moveDockToHostEdge(flow,position=="top" ? Qt::TopDockWidgetArea : position=="left" ? Qt::LeftDockWidgetArea :
            position=="right" ? Qt::RightDockWidgetArea : Qt::BottomDockWidgetArea);
        return;
    }
    if(anchor && placement==1) tabifyDockWidget(anchor,dock);
    else if(anchor) placeDock(dock,anchor,placement==3 ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea);
    else placeDock(dock,flow,position=="top" ? Qt::BottomDockWidgetArea : position=="left" ? Qt::RightDockWidgetArea :
        position=="right" ? Qt::LeftDockWidgetArea : Qt::TopDockWidgetArea);
}
void MainWindow::place_graph(WorkspaceDock *dock, int placement) {
    const bool restoring = _restoring_layout;
    _restoring_layout = true;
    // Reuse the dock and renderer: docking never recreates the camera or graph.
    if (placement == 0) {
        dock->setFloating(true);
    } else {
        auto *anchor=_last_graph_slot.data();
        if(anchor==dock || !in_editor_window(anchor) || anchor->isHidden()) anchor=nullptr;
        if(!anchor) for(auto *candidate:graph_docks())
            if(candidate!=dock && in_editor_window(candidate) && !candidate->isHidden()) { anchor=candidate; break; }
        dock->setFloating(false);
        rebuild_graph_areas(dock,anchor,placement);
    }
    dock->show();
    layout()->activate();
    dock->raise();
    if(in_editor_window(dock)) _last_graph_slot=dock;
    // Floating an inactive tab can retain its previous tab visibility signal.
    // A shown top-level dock is exposed independently of that former group.
    if (dock->isFloating()) {
        QString key;
        for (auto it = _active_node_views.cbegin(); it != _active_node_views.cend(); ++it)
            if (graph_dock(it.key()) == dock) { key = it.value(); break; }
        if (!key.isEmpty()) set_graph_exposed(key, dock, dock->isVisible());
    }
    _restoring_layout = restoring;
    _workspace_save_timer.start(); refresh_subscriptions();
}
void MainWindow::setup_graph_dock_controls(WorkspaceDock *dock) {
    // ADS owns the single caption, tab dragging, pin button and floating frame.
    // Product-specific commands are actions, not a second docking implementation.
    auto *placement=new QAction(ui::text("Placement"),dock);
    placement->setObjectName("graphPlacement");
    ui::bind(placement,"text","Placement");
    auto *menu=new QMenu(dock);
    placement->setMenu(menu);
    const char *labels[]{"Separate window", "Return to tabs", "Place beside", "Place below"};
    for(int index=0;index<4;++index) {
        auto *action=menu->addAction(ui::text(labels[index]));
        ui::bind(action,"text",labels[index]); action->setData(index);
        connect(action,&QAction::triggered,this,[this,dock,index] {place_graph(dock,index);});
    }
    dock->setTitleBarActions({placement});
    if (dock==_panels.value("dataFlow")) {
        if(auto *editor=qobject_cast<SchemeEditorWidget *>(dock->widget())) {
            dock->setHeaderControls(editor->toolbar());
        }
    }
    connect(dock,&WorkspaceDock::topLevelChanged,this,[this,dock](bool) {
        QTimer::singleShot(0,dock,[this,dock] {
            QString key=_panels.key(dock);
            if(key.isEmpty()) for(auto it=_node_views.cbegin();it!=_node_views.cend();++it)
                if(it->dock==dock) {key=it.key();break;}
            if(!key.isEmpty()) set_graph_exposed(key,dock,dock->isVisible());
            refresh_subscriptions();
        });
    });
}
void MainWindow::refresh_graph_navigation(const QMap<int,QString> &previous) {
    const bool first=!_graph_navigation_initialized;
    const bool migrate=_application.state().file_configs.ui.workspace.value("graph_navigation_version").toInt()!=1;
    _graph_navigation_initialized=true;
    QList<QAction *> actions;
    WorkspaceDock *anchor=nullptr,*selected=nullptr;
    for(auto it=_active_node_views.begin();it!=_active_node_views.end();++it) {
        auto *dock=graph_dock(it.key());if(!dock) continue;
        actions.append(dock->toggleViewAction());
        if(!dock->isFloating()) {
            if(!anchor || it.value()=="spectrogram") anchor=dock;
            if(dock->widget()->isVisible()) selected=dock;
        }
    }
    const bool restoring=_restoring_layout;_restoring_layout=true;
    for(auto it=_active_node_views.begin();it!=_active_node_views.end();++it) {
        auto *dock=graph_dock(it.key());if(!dock) continue;
        const bool newlyAdded=!first && !previous.contains(it.key());
        if((migrate || newlyAdded) && !dock->isFloating()) {
            if(anchor && dock!=anchor) tabifyDockWidget(anchor,dock);
            if(auto *surface=qobject_cast<SpectrogramGraph *>(dock->widget()))
                surface->initialize({240,180},screen()->availableGeometry().size());
            dock->show();
        }
    }
    if(migrate && anchor) selected=anchor;
    if(selected) selected->raise();
    _restoring_layout=restoring;
    if(auto *settings=findChild<SettingsWidget *>()) settings->set_graph_actions(actions);
    if(migrate || (!first && previous!=_active_node_views)) {
        _application.state().file_configs.ui.workspace["graph_navigation_version"]=1;
        save_workspace();_workspace_save_timer.start();
    }
}
