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
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabBar>
#include <QToolButton>
#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(core_app_window)

QDockWidget *MainWindow::add_panel(const QString &id, const QString &title, QWidget *view,
                                   bool UIApplicationConfig::*flag) {
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(id + "Dock");
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
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
    connect(dock->toggleViewAction(), &QAction::toggled, this, [this, flag, id](bool open) {
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
    connect(dock, &QDockWidget::visibilityChanged, this, [this, id](bool visible) {
        set_graph_exposed(id, _panels[id], visible);
        refresh_subscriptions();
        if (!_restoring_layout)
            _workspace_save_timer.start();
    });
    connect(dock, &QDockWidget::topLevelChanged, this, [this] {
        if (!_restoring_layout)
            _workspace_save_timer.start();
    });
    connect(dock, &QDockWidget::dockLocationChanged, this, [this] {
        if (!_restoring_layout)
            _workspace_save_timer.start();
    });
    return dock;
}
void MainWindow::reset_workspace() {
    const bool restoring = _restoring_layout;
    _restoring_layout = true;
    if (!_default_workspace_state.isEmpty()) {
        // Let Qt restore its tab groups atomically instead of removing and reusing
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
    workspace["layout_version"] = 4;
    workspace["geometry"] = QString::fromLatin1(saveGeometry().toBase64());
    workspace["state"] = QString::fromLatin1(saveState(1).toBase64());
    workspace["visibility"] = visibility;
    ui.workspace = workspace;
}
void MainWindow::refresh_subscriptions() {
    if (_restoring_layout)
        return;
    int docked=0;
    for(auto *dock:findChildren<QDockWidget *>(QString{},Qt::FindDirectChildrenOnly))
        if(!dock->isHidden() && !dock->isFloating()) ++docked;
    const bool single=docked<=1;
    if(property("singleDock").toBool()!=single) {
        setProperty("singleDock",single);
        style()->unpolish(this);style()->polish(this);
    }
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
            _active_node_views.value(view.node) == it.key() && _render_visible.value(it.key())) {
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
    for (auto *dock : _panels)
        if (dock->isFloating()) {
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
            if(auto *dock=qobject_cast<QDockWidget *>(widget)) {
                if(is_graph_dock(dock) && !dock->isHidden()) _last_graph_slot=dock;
                break;
            }
        }
    }
    if (!_restoring_layout && qobject_cast<QDockWidget *>(watched) &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize))
        _workspace_save_timer.start();
    return QMainWindow::eventFilter(watched, event);
}
void MainWindow::closeEvent(QCloseEvent *event) {
    _workspace_save_timer.stop();
    _camera_save_timer.stop();
    if (auto *editor = findChild<SchemeEditorWidget *>())
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
QDockWidget *MainWindow::graph_dock(int node) const {
    const auto key=_active_node_views.value(node);
    return _panels.contains(key) ? _panels.value(key) : _node_views.value(key).dock;
}
void MainWindow::open_graph_node(int node) {
    auto *dock=graph_dock(node);
    if(!dock) return;
    auto *slot=_last_graph_slot.data();
    if(!slot || slot->isHidden() || !is_graph_dock(slot)) {
        slot=nullptr;
        for(auto it=_active_node_views.cbegin();it!=_active_node_views.cend();++it)
            if(_render_visible.value(it.value())) { slot=graph_dock(it.key()); break; }
    }
    if(!slot) return; // Only an explicit context command creates a viewing area.
    if(slot!=dock) {
        if(slot->isFloating()) {
            const auto geometry=slot->geometry();
            dock->setFloating(true);dock->setGeometry(geometry);
            slot->hide();
        } else if(!tabifiedDockWidgets(slot).contains(dock)) {
            dock->setFloating(false);
            tabifyDockWidget(slot,dock);
            slot->hide(); // Replace, rather than add, a visible tab in this area.
        }
    }
    if(auto *surface=qobject_cast<SpectrogramGraph *>(dock->widget()))
        surface->initialize({240,180},screen()->availableGeometry().size());
    dock->show();dock->raise();
    _last_graph_slot=dock;
    if(dock->isFloating()) set_graph_exposed(_active_node_views.value(node),dock,dock->isVisible());
    // Activation from the editor must not steal keyboard focus during dragging
    // or prevent subsequent copy/paste shortcuts from reaching the editor.
    _workspace_save_timer.start();refresh_subscriptions();
}
bool MainWindow::is_graph_dock(QDockWidget *dock) const {
    if(!dock) return false;
    for(auto it=_active_node_views.cbegin();it!=_active_node_views.cend();++it)
        if(graph_dock(it.key())==dock) return true;
    return false;
}
void MainWindow::add_graph_node(int node,int placement) {
    if(auto *dock=graph_dock(node)) {
        place_graph(dock,placement);
        _last_graph_slot=dock;
    }
}
void MainWindow::set_graph_exposed(const QString &key, QDockWidget *dock, bool exposed) {
    _render_visible[key] = exposed;
    dock->setProperty("renderExposed", exposed);
    if (auto *panel = qobject_cast<GraphPanel *>(dock->widget())) panel->set_render_visible(exposed);
    if (auto *surface = qobject_cast<SpectrogramGraph *>(dock->widget())) surface->set_render_visible(exposed);
    for (auto *view : dock->findChildren<QChartView *>())
        if (auto *chart = dynamic_cast<AbstractChartView *>(view)) chart->set_render_visible(exposed);
}
void MainWindow::place_graph(QDockWidget *dock, int placement) {
    // Reuse the dock and renderer: docking never recreates the camera or graph.
    if (placement == 0) {
        dock->setFloating(true);
    } else {
        QDockWidget *anchor = nullptr;
        for (const auto &key : _active_node_views.keys()) {
            auto *candidate = graph_dock(key);
            if (candidate && candidate != dock && !candidate->isFloating() && !candidate->isHidden()) {
                if (!anchor || _render_visible.value(_active_node_views.value(key))) anchor = candidate;
            }
        }
        dock->setFloating(false);
        if (anchor) {
            if (placement == 1) tabifyDockWidget(anchor, dock);
            else {
                // Qt adds a tab instead of splitting when `first` is tabified.
                // Split the anchor on its own, then restore its existing peers.
                const auto peers = tabifiedDockWidgets(anchor);
                const auto area = dockWidgetArea(anchor);
                removeDockWidget(dock);
                removeDockWidget(anchor);
                addDockWidget(area, anchor);
                splitDockWidget(anchor, dock, placement == 2 ? Qt::Horizontal : Qt::Vertical);
                for (auto *peer : peers) if (peer != dock) tabifyDockWidget(anchor, peer);
                anchor->show(); anchor->raise();
            }
        } else addDockWidget(Qt::LeftDockWidgetArea, dock);
    }
    dock->show(); dock->raise();
    // Floating an inactive tab can retain its previous tab visibility signal.
    // A shown top-level dock is exposed independently of that former group.
    if (dock->isFloating()) {
        QString key;
        for (auto it = _active_node_views.cbegin(); it != _active_node_views.cend(); ++it)
            if (graph_dock(it.key()) == dock) { key = it.value(); break; }
        if (!key.isEmpty()) set_graph_exposed(key, dock, dock->isVisible());
    }
    _workspace_save_timer.start(); refresh_subscriptions();
}
void MainWindow::setup_graph_dock_controls(QDockWidget *dock) {
    auto *strip=new QWidget(dock);strip->setProperty("dockControls",true);
    strip->setObjectName(dock->objectName()+"Controls");
    auto *row=new QHBoxLayout(strip);row->setContentsMargins(6,3,6,3);
    auto *floatingTitle=new QLabel(dock->windowTitle(),strip);floatingTitle->hide();row->addWidget(floatingTitle);
    connect(dock,&QDockWidget::windowTitleChanged,floatingTitle,&QLabel::setText);
    row->addStretch();
    auto *placement=new QToolButton(strip);placement->setObjectName("graphPlacement");
    placement->setPopupMode(QToolButton::InstantPopup);ui::bind(placement,"text","Placement");
    auto *placementMenu=new QMenu(placement);placement->setMenu(placementMenu);row->addWidget(placement);
    const char *labels[]{"Separate window", "Return to tabs", "Place beside", "Place below"};
    for(int index=0;index<4;++index) {
        auto *action=placementMenu->addAction(ui::text(labels[index]));
        ui::bind(action,"text",labels[index]);action->setData(index);
        connect(action,&QAction::triggered,this,[this,dock,index] {place_graph(dock,index);});
    }
    auto *close=new QToolButton(strip);close->setText(QStringLiteral("\u00d7"));ui::bind(close,"toolTip","Close");row->addWidget(close);
    connect(close,&QToolButton::clicked,dock,&QDockWidget::hide);
    dock->setTitleBarWidget(strip);
    connect(dock,&QDockWidget::topLevelChanged,dock,[floatingTitle](bool floating) {
        floatingTitle->setVisible(floating);
    });
    connect(dock, &QDockWidget::topLevelChanged, this, [this, dock](bool floating) {
        if (!floating) return;
        // Detaching an inactive tab can leave visibilityChanged at its old
        // value. Once Qt has completed reparenting, the separate window is
        // exposed independently of the previously selected dock tab.
        QTimer::singleShot(0, dock, [this, dock] {
            if (!dock->isFloating()) return;
            QString key = _panels.key(dock);
            if (key.isEmpty()) {
                for (auto it = _node_views.cbegin(); it != _node_views.cend(); ++it)
                    if (it->dock == dock) { key = it.key(); break; }
            }
            if (!key.isEmpty()) {
                set_graph_exposed(key, dock, dock->isVisible());
                refresh_subscriptions();
            }
        });
    });
}
void MainWindow::refresh_graph_navigation(const QMap<int,QString> &previous) {
    const bool first=!_graph_navigation_initialized;
    const bool migrate=_application.state().file_configs.ui.workspace.value("graph_navigation_version").toInt()!=1;
    _graph_navigation_initialized=true;
    QList<QAction *> actions;
    QDockWidget *anchor=nullptr,*selected=nullptr;
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
    for(auto *bar:findChildren<QTabBar *>()) {
        // Qt retains tab ordering, docking and active-page behavior.
        if(bar->parentWidget()==this) {bar->setUsesScrollButtons(true);bar->setElideMode(Qt::ElideRight);}
    }
    if(auto *settings=findChild<SettingsWidget *>()) settings->set_graph_actions(actions);
    if(migrate || (!first && previous!=_active_node_views)) {
        _application.state().file_configs.ui.workspace["graph_navigation_version"]=1;
        save_workspace();_workspace_save_timer.start();
    }
}
