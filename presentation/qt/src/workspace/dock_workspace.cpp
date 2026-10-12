#include "core_app/main_window.h"
#include "core_app/graph_panel.h"
#include "core_app/settings_widget.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include <desktop/numeric_project_document.h>
#include <DockAreaWidget.h>
#include "visualization/ui_text.h"
#include "visualization/charts/line.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QFormLayout>
#include <QScrollArea>
#include <QMenu>
#include <QMenuBar>
#include <QMoveEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWindow>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QPushButton>
#include <QFormLayout>
#include <QVBoxLayout>
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
    if(id=="dataFlow") {
        ui::bind(dock, "windowTitle", title);
        ui::bind(dock->toggleViewAction(), "text", title);
    } else {
        dock->setProperty("graphViewKey",id);
        dock->setProperty("graphTitleSource",title);
    }
    auto* supplied_surface=qobject_cast<SpectrogramGraph*>(view);
    auto* chart=dynamic_cast<AbstractChartView*>(view);
    if(supplied_surface)chart=new LineChartView;
    if (chart) {
        auto preferences=_application.state().file_configs.ui.graph_views.value(id).toObject();
        if(supplied_surface && !preferences.contains("display_mode"))preferences["display_mode"]="surface3d";
        auto *panel =
            new GraphPanel(id, chart, preferences, dock,supplied_surface);
        connect(panel, &GraphPanel::presentation_changed, this, [this](const QString &id, const QJsonObject &prefs) {
            auto saved = _application.state().file_configs.ui.graph_views.value(id).toObject();
            for (auto it = prefs.constBegin(); it != prefs.constEnd(); ++it)
                saved[it.key()] = it.value();
            _application.state().file_configs.ui.graph_views[id] = saved;
            if(prefs.size()!=1 || !prefs.contains("camera"))_numeric_render_dirty=true;
            _workspace_save_timer.start();
        });
        connect(panel,&GraphPanel::renderer_changed,this,[this]{refresh_vector_delivery_queues();_numeric_render_dirty=true;render_numeric_views();});
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
        visibility[it.key()] = !it.value()->isClosed();
    auto &ui = _application.state().file_configs.ui;
    for (auto it = _node_views.begin(); it != _node_views.end(); ++it) {
        const bool open = !it.value().dock->isClosed();
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
QJsonObject MainWindow::capture_preset_presentation() {
    save_workspace();
    auto& ui=_application.state().file_configs.ui;
    auto surface=ui.graph_views.value("spectrogram").toObject();
    surface["camera"]=_spectrogram_view->camera_state();surface["renderer"]=_spectrogram_view->presentation_state();
    surface["side_ratio"]=ui.spectrogram_side_panel_ratio;ui.graph_views["spectrogram"]=surface;
    auto preset_workspace=ui.workspace;
    preset_workspace.remove("geometry");
    QJsonObject result{{"version",1},{"graph_views",ui.graph_views},{"graph_rendering",ui.graph_rendering},{"workspace",preset_workspace},
        {"show_raw_samples",ui.show_raw_samples},{"show_window_signal",ui.show_window_signal},
        {"show_windowed_signal",ui.show_windowed_signal},{"show_window_function",ui.show_window_function},
        {"show_amplitudes",ui.show_amplitudes},{"show_test_amplitudes",ui.show_test_amplitudes},
        {"show_spectrogram",ui.show_spectrogram},{"show_data_flow",ui.show_data_flow}};
    QJsonObject checked;
    const auto admitted=desktop::decode_numeric_preset_presentation(QJsonDocument(result).toJson(QJsonDocument::Compact),checked);
    if(!admitted)qCWarning(core_app_window) << "Preset display capture rejected:" << admitted.path << admitted.error;
    return result;
}
void MainWindow::restore_preset_presentation(const QJsonObject& supplied) {
    QJsonObject presentation;
    if(!supplied.isEmpty() && !desktop::decode_numeric_preset_presentation(QJsonDocument(supplied).toJson(QJsonDocument::Compact),presentation))return;
    domain::NumericProcessingGraph draft;
    auto* editor=data_flow_editor();if(!editor || !editor->numeric_draft(draft))return;
    const bool restoring=_restoring_layout;
    _restoring_layout=_restoring_preset=true;
    _workspace_save_timer.stop();_camera_save_timer.stop();_last_graph_slot.clear();
    // Dock identities and saved ADS groups belong to the selected preset.
    // Endpoint states survive until the new binding set removes incompatible ones.
    // ADS removal emits visibility notifications for neighbouring docks. Detach
    // the complete old registry before any removal so those callbacks cannot
    // discover a dock deleted earlier in the same restoration.
    const auto previousViews=std::move(_node_views);
    _node_views.clear();_active_node_views.clear();
    for(const auto& view:previousViews)disconnect(view.dock,nullptr,this,nullptr);
    for(auto it=previousViews.cbegin();it!=previousViews.cend();++it) {
        _numeric_bindings.remove(it.key());_metric_bindings.remove(it.key());
        _render_visible.remove(it.key());_surface_errors.remove(it.key());
    }
    for(const auto& view:previousViews) {
        removeDockWidget(view.dock);delete view.dock;
    }
    _view_draft=std::move(draft);_view_draft_device=_application.device_id();
    auto& ui=_application.state().file_configs.ui;
    ui.graph_views=presentation.value("graph_views").toObject();ui.graph_rendering=presentation.value("graph_rendering").toObject();
    const auto session_geometry=ui.workspace.value("geometry");
    ui.workspace=presentation.value("workspace").toObject();
    ui.workspace.remove("geometry");
    if (!session_geometry.isUndefined()) ui.workspace["geometry"]=session_geometry;
    restore_node_views();sync_node_views(_application.client().snapshot().graph);
    const auto visible=[&](const char* field,const QString& key) {
        const bool exists=key=="dataFlow" || std::any_of(_active_node_views.cbegin(),_active_node_views.cend(),[&](const auto& value){return value==key;});
        return exists && presentation.value(field).toBool(true);
    };
    ui.show_raw_samples=visible("show_raw_samples","rawSamples");ui.show_window_signal=visible("show_window_signal","windowSignal");
    ui.show_windowed_signal=visible("show_windowed_signal","windowedSignal");ui.show_window_function=visible("show_window_function","windowFunction");
    ui.show_amplitudes=visible("show_amplitudes","fft");ui.show_test_amplitudes=visible("show_test_amplitudes","mel");
    ui.show_spectrogram=visible("show_spectrogram","spectrogram");ui.show_data_flow=visible("show_data_flow","dataFlow");
    const auto state=QByteArray::fromBase64(ui.workspace.value("ads_state").toString().toLatin1());
    const bool restored=ui.workspace.value("version").toInt()==1 && ui.workspace.value("layout_version").toInt()==5 && !state.isEmpty() && restoreState(state,1);
    if(!restored) {
        auto* flow=_panels.value("dataFlow");placeDock(flow,nullptr,Qt::BottomDockWidgetArea);
        WorkspaceDock* anchor=nullptr;
        for(auto* dock:graph_docks()) {
            if(!anchor){placeDock(dock,flow,Qt::TopDockWidgetArea);anchor=dock;}else tabifyDockWidget(anchor,dock);
        }
        if(anchor)anchor->raise();
    }
    const auto choices=ui.workspace.value("visibility").toObject();
    for(auto it=_node_views.begin();it!=_node_views.end();++it) {
        const auto prefs=ui.graph_views.value(it.key()).toObject();
        const bool current=it->device==_application.device_id() && it->graph==QString::fromStdString(_view_draft->graph_id);
        const bool open=current && (restored && choices.value("nodeView_"+it.key()).isBool()
            ?choices.value("nodeView_"+it.key()).toBool():prefs.value("open").toBool(true));
        if(it->dock->isClosed()==open)it->dock->toggleView(open);
    }
    for(auto it=_panels.begin();it!=_panels.end();++it)if(auto* panel=qobject_cast<GraphPanel*>(it.value()->widget()))
        panel->restore_presentation(ui.graph_views.value(it.key()).toObject());
    const auto surface=ui.graph_views.value("spectrogram").toObject();
    _spectrogram_view->set_presentation_state(surface.value("renderer").toObject());
    auto camera=surface.value("camera").toObject();
    if(camera.isEmpty()){SpectrogramGraph defaults;camera=defaults.camera_state();}
    _spectrogram_view->set_camera_state(camera);
    _spectrogram_view->set_side_panel_ratio(float(surface.value("side_ratio").toDouble(.25)));
    ui.camera=_spectrogram_view->camera_state();ui.spectrogram_side_panel_ratio=float(surface.value("side_ratio").toDouble(.25));
    update_chart_visibility(ui);apply_surface_settings();
    for(auto* dock:graph_docks())refresh_graph_title(dock);
    if(auto* settings=findChild<SettingsWidget*>())settings->refresh_graph_preferences();
    _restoring_preset=false;_restoring_layout=restoring;
    ui.workspace["graph_navigation_version"]=1;
    save_workspace();refresh_subscriptions();_workspace_save_timer.start();
}
void MainWindow::refresh_subscriptions() {
    // A restoration publishes one complete binding set after ADS has settled.
    if(_restoring_preset)return;
    refresh_vector_delivery_queues();
    std::vector<pipeline::EndpointIdentity> endpoints;
    const auto exposed=[this](const QString& key){
        auto* dock=_node_views.contains(key)?_node_views[key].dock:_panels.value(key);
        return dock && !dock->isHidden() && dock->property("renderExposed").toBool();
    };
    for(auto it=_numeric_bindings.cbegin();it!=_numeric_bindings.cend();++it) {
        if(!exposed(it.key()))continue;
        const auto& binding=it.value();
        if(binding.device!=_application.device_id())continue;
        if(std::find(endpoints.begin(),endpoints.end(),binding.endpoint)==endpoints.end())
            endpoints.push_back(binding.endpoint);
    }
    for(auto it=_metric_bindings.cbegin();it!=_metric_bindings.cend();++it)if(exposed(it.key()))for(const auto& binding:it.value()) {
        if(binding.device==_application.device_id() && std::find(endpoints.begin(),endpoints.end(),binding.endpoint)==endpoints.end())
            endpoints.push_back(binding.endpoint);
    }
    _application.set_numeric_endpoints(std::move(endpoints));
    if(!_restoring_layout)render_numeric_views();
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
        emit settings_change_requested({ui::SettingsChangeScope::SessionWorkspace, false});
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
    auto *slot=_last_graph_slot.data();
    if(!slot || slot->isHidden() || !is_graph_dock(slot) || !in_editor_window(slot)) {
        slot=nullptr;
        for(auto it=_active_node_views.cbegin();it!=_active_node_views.cend();++it)
            if(_render_visible.value(it.value()) && in_editor_window(graph_dock(it.key()))) { slot=graph_dock(it.key()); break; }
        if(!slot) for(auto *candidate:graph_docks())
            if(in_editor_window(candidate) && candidate->property("renderExposed").toBool()) { slot=candidate; break; }
    }
    if(!slot) return; // Only an explicit context command creates a viewing area.
    if(!dock)dock=new_graph_view(node);
    if(!dock)return;
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
        }
        // A previously closed representation in this same tab group replaces
        // the selected slot too. Already-open representations were focused above.
        slot->hide();
    }
    if(auto *surface=qobject_cast<SpectrogramGraph *>(dock->widget()))
        surface->initialize({240,180},screen()->availableGeometry().size());
    install_graph_name_control(dock);
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
    const auto selected_graph=_view_draft?QString::fromStdString(_view_draft->graph_id):_application.graph_id();
    for(int node:_active_node_views.keys()) if(auto *dock=graph_dock(node)) result.append(dock);
    for(const auto &view:_node_views)
        if(view.device==_application.device_id() && view.graph==selected_graph && _active_node_views.contains(view.node)
           && !result.contains(view.dock)) result.append(view.dock);
    return result;
}
bool MainWindow::in_editor_window(WorkspaceDock *dock) const {
    auto *flow=_panels.value("dataFlow");
    return dock && flow && dock->sharesHost(flow);
}
bool MainWindow::graph_for_view(int node,domain::NumericProcessingGraph &graph) const {
    const auto contains=[node](const auto& candidate) {
        return std::any_of(candidate.nodes.begin(),candidate.nodes.end(),[node](const auto& entry){return entry.id==node;});
    };
    const auto document=_application.client().snapshot().graph;
    if(_view_draft) {
        auto* editor=data_flow_editor();
        if(editor && editor->numeric_draft(graph) && contains(graph))return true;
    }
    if(desktop::decode_numeric_graph(QJsonDocument(document).toJson(QJsonDocument::Compact),graph) && contains(graph))return true;
    auto* editor=data_flow_editor();
    return editor && editor->numeric_draft(graph) && contains(graph);
}
WorkspaceDock *MainWindow::new_graph_view(int node,std::optional<domain::PortId> port) {
    const auto document=_application.client().snapshot().graph;
    domain::NumericProcessingGraph graph;
    if(!graph_for_view(node,graph))return nullptr;
    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==node;});
    if(found==graph.nodes.end() || !numeric_editor::is_graph_view(QString::fromStdString(found->kind)))return nullptr;
    domain::ResolvedProcessingNode resolved;
    if(!domain::resolve_processing_node(*found,numeric_editor::limits(),resolved))return nullptr;
    const auto& ports=resolved.role==domain::NodeRole::Sink?found->inputs:found->outputs;
    if(ports.empty() || (port && std::none_of(ports.begin(),ports.end(),[&](const auto& p){return p.id==*port;})))return nullptr;
    const auto instance=_next_view_instance++;
    const auto identity=QString::fromLatin1(QJsonDocument(QJsonArray{_application.device_id(),QString::fromStdString(graph.graph_id),node,instance}).toJson(QJsonDocument::Compact).toHex());
    if(port){auto preferences=_application.state().file_configs.ui.graph_views[identity].toObject();preferences["port"]=int(*port);_application.state().file_configs.ui.graph_views[identity]=preferences;}
    const auto key=ensure_node_view(node,QString::fromStdString(found->kind),_application.device_id(),QString::fromStdString(graph.graph_id),false,instance,&*found);
    // Only the authoritative snapshot binds data. Opening an unapplied draft
    // creates an unavailable presentation, never applies or starts processing.
    if(!document.isEmpty())sync_node_views(document);
    if(!_active_node_views.contains(node))_active_node_views[node]=key;
    return _node_views.value(key).dock;
}
void MainWindow::populate_graph_menu(int node,QMenu *menu) {
    domain::NumericProcessingGraph graph;
    if(!graph_for_view(node,graph))return;
    const auto shown=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==node;});
    if(shown==graph.nodes.end() || !numeric_editor::is_graph_view(QString::fromStdString(shown->kind)))return;
    if(graph_for_view(node,graph)) {
        const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==node;});
        domain::ResolvedProcessingNode resolved;
        if(found!=graph.nodes.end() && domain::resolve_processing_node(*found,numeric_editor::limits(),resolved))
        for(const auto& port:resolved.role==domain::NodeRole::Sink?found->inputs:found->outputs) {
            auto* action=menu->addAction(ui::text(resolved.role==domain::NodeRole::Sink?"Input port %1":"Output port %1").arg(port.id));
            action->setObjectName(QStringLiteral("openGraphPort%1").arg(port.id));
            connect(action,&QAction::triggered,this,[this,node,port=port.id] {
                if(auto* dock=new_graph_view(node,port))place_graph(dock,1);
            });
        }
    }
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
    auto* dock=graph_dock(node);
    if(!dock || !dock->isClosed())
        dock=new_graph_view(node);
    if(!dock)return;
    place_graph(dock,placement);
    if(in_editor_window(dock)) _last_graph_slot=dock;
}
void MainWindow::set_graph_exposed(const QString &key, WorkspaceDock *dock, bool exposed) {
    const bool changed=dock->property("renderExposed").toBool()!=exposed;
    _render_visible[key] = exposed;
    dock->setProperty("renderExposed", exposed);
    if (auto *surface = qobject_cast<SpectrogramGraph *>(dock->widget())) surface->set_render_visible(exposed);
    if (auto *panel = qobject_cast<GraphPanel *>(dock->widget())) panel->set_render_visible(exposed);
    // Hidden tabs do not need transport previews or owning history snapshots.
    // DSP continues in the executor; subscription admission restores current
    // data when a tab returns, including without any new source ingress.
    if(changed)refresh_subscriptions();
    else if(exposed)render_numeric_views();
    // ADS emits Show while the returning tab can still have its offscreen
    // geometry. Resolve exposure after the tab selection/layout has settled.
    QTimer::singleShot(0,dock,[this,key,dock] {
        auto* area=dock->dockAreaWidget();
        const bool settled=!dock->isClosed() && dock->isVisible() && area && area->currentDockWidget()==dock;
        if(dock->property("renderExposed").toBool()!=settled)set_graph_exposed(key,dock,settled);
    });
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
    install_graph_name_control(dock);
    dock->setTitleBarActions({placement});
    connect(&ui::Language::instance(),&ui::Language::changed,dock,[this,dock]{refresh_graph_title(dock);});
    refresh_graph_title(dock);
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
void MainWindow::install_graph_name_control(WorkspaceDock *dock) {
    if(!dock || dock->property("graphViewKey").toString().isEmpty())return;
    auto* panel=qobject_cast<GraphPanel*>(dock->widget());
    if(!panel || panel->property("graphRenameConnected").toBool())return;
    panel->setProperty("graphRenameConnected",true);
    connect(panel,&GraphPanel::rename_requested,this,[this,dock]{request_graph_rename(dock);});
}
void MainWindow::request_graph_rename(WorkspaceDock *dock) {
    const auto key=dock->property("graphViewKey").toString();if(key.isEmpty())return;
    bool accepted=false;
    const auto name=QInputDialog::getText(dock,ui::text("Rename graph"),
        ui::text("Graph name (empty restores default)"),QLineEdit::Normal,
        _application.state().file_configs.ui.graph_views.value(key).toObject().value("display_name").toString(),&accepted);
    if(accepted)rename_graph(dock,name);
}
void MainWindow::refresh_graph_title(WorkspaceDock *dock) {
    const auto key=dock->property("graphViewKey").toString();if(key.isEmpty())return;
    auto title=_application.state().file_configs.ui.graph_views.value(key).toObject().value("display_name").toString().trimmed();
    const auto identity=QJsonDocument::fromJson(QByteArray::fromHex(key.toLatin1())).array();
    const auto selected_graph=_view_draft?QString::fromStdString(_view_draft->graph_id):_application.graph_id();
    const auto endpoint=dock->property("numericEndpoint").toMap();
    const bool current=(identity.size()>=3 && identity[0].toString()==_application.device_id() && identity[1].toString()==selected_graph) ||
        (_panels.value(key)==dock && endpoint.value("device").toString()==_application.device_id() && endpoint.value("graph").toString()==selected_graph &&
         _active_node_views.value(dock->property("graphNode").toInt())==key);
    auto* model=current && dock->property("graphNode").isValid() && _application.state().data_flow_graph?
        _application.state().data_flow_graph->delegateModel<AudioNodeModel>(dock->property("graphNode").toUInt()):nullptr;
    if(!model || model->name()!="ScalarView")if(auto* panel=qobject_cast<GraphPanel*>(dock->widget())) {
        for(const auto& connection:panel->property("scalarInputConnections").value<QList<QMetaObject::Connection>>())QObject::disconnect(connection);
        panel->setProperty("scalarInputConnections",QVariant{});
        panel->setProperty("scalarInputsModel",QVariant{});
        panel->set_scalar_inputs({},false);
    }
    if(model && numeric_editor::is_graph_view(model->name())){
        if(model->name()=="ScalarView")if(auto* panel=qobject_cast<GraphPanel*>(dock->widget())) {
            QVariantList inputs;
            const auto metadata=model->presentation().value("scalar_inputs").toObject();
            if(const auto* numeric=model->numeric_node())for(const auto& input:numeric->inputs) {
                const auto preferences=metadata.value(QString::number(input.id)).toObject();
                inputs.append(QVariantMap{{"port",int(input.id)},{"name",preferences.value("display_name").toString()},
                    {"enabled",preferences.value("enabled").toBool(true)},{"removeEnabled",numeric->inputs.size()>1}});
            }
            panel->set_scalar_inputs(inputs,inputs.size()<qsizetype(numeric_editor::limits().max_ports_per_node));
            if(panel->property("scalarInputsModel").value<QObject*>()!=model) {
                for(const auto& connection:panel->property("scalarInputConnections").value<QList<QMetaObject::Connection>>())QObject::disconnect(connection);
                const QPointer<AudioNodeModel> guard(model);
                QList<QMetaObject::Connection> connections;
                connections.append(connect(panel,&GraphPanel::add_scalar_input_requested,panel,[guard]{if(guard)guard->add_scalar_input();}));
                connections.append(connect(panel,&GraphPanel::remove_scalar_input_requested,panel,[guard](int port){if(guard && port>=0 && port<=65535)guard->remove_scalar_input(domain::PortId(port));}));
                connections.append(connect(panel,&GraphPanel::scalar_input_name_requested,panel,[guard](int port,const QString& name){if(guard && port>=0 && port<=65535)guard->edit_scalar_input_name(domain::PortId(port),name);}));
                connections.append(connect(panel,&GraphPanel::scalar_input_enabled_requested,panel,[guard](int port,bool enabled){if(guard && port>=0 && port<=65535)guard->set_scalar_input_enabled(domain::PortId(port),enabled);}));
                connections.append(connect(model,&QObject::destroyed,panel,[panel,model]{
                    if(panel->property("scalarInputsModel").value<QObject*>()==model){panel->setProperty("scalarInputsModel",QVariant{});panel->set_scalar_inputs({},false);}
                }));
                panel->setProperty("scalarInputsModel",QVariant::fromValue(static_cast<QObject*>(model)));
                panel->setProperty("scalarInputConnections",QVariant::fromValue(connections));
            }
        }

        if(dock->property("graphCaptionModel").value<QObject*>()!=model){
            dock->setProperty("graphCaptionModel",QVariant::fromValue(static_cast<QObject*>(model)));
            connect(model,&QObject::destroyed,dock,[dock,model]{
                if(dock->property("graphCaptionModel").value<QObject*>()==model)dock->setProperty("graphCaptionModel",QVariant{});
            });
            if(model->presentation().value("display_name").toString().trimmed().isEmpty() && !title.isEmpty()){
                auto presentation=model->presentation();presentation["display_name"]=title;model->set_presentation(presentation);
            }
            connect(model,&AudioNodeModel::presentation_changed,dock,[this,dock]{
                refresh_graph_title(dock);_workspace_save_timer.start();
                if(dock->property("graphKind").toString()=="ScalarView")QTimer::singleShot(0,this,[this]{sync_node_views(_application.client().snapshot().graph);});
            });
        }
        title=model->presentation().value("display_name").toString().trimmed();
        auto preferences=_application.state().file_configs.ui.graph_views.value(key).toObject();
        if(title.isEmpty())preferences.remove("display_name");else preferences["display_name"]=title;
        _application.state().file_configs.ui.graph_views[key]=preferences;
    }
    if(title.isEmpty()) {
        const auto kind=dock->property("graphKind").toString();
        title=kind.isEmpty()?ui::text(dock->property("graphTitleSource").toString().toUtf8().constData()):
            AudioNodeModel(kind).caption()+" #"+QString::number(dock->property("graphNode").toInt());
    }
    dock->setWindowTitle(title);dock->toggleViewAction()->setText(title);
}
void MainWindow::rename_graph(WorkspaceDock *dock,const QString &name) {
    const auto key=dock->property("graphViewKey").toString();if(key.isEmpty())return;
    const auto title=name.trimmed();
    if(title.size()>256 || std::any_of(title.begin(),title.end(),[](QChar c){return c.category()==QChar::Other_Control;}))return;
    const auto identity=QJsonDocument::fromJson(QByteArray::fromHex(key.toLatin1())).array();
    const auto selected_graph=_view_draft?QString::fromStdString(_view_draft->graph_id):_application.graph_id();
    const auto endpoint=dock->property("numericEndpoint").toMap();
    const bool current=(identity.size()>=3 && identity[0].toString()==_application.device_id() && identity[1].toString()==selected_graph) ||
        (_panels.value(key)==dock && endpoint.value("device").toString()==_application.device_id() && endpoint.value("graph").toString()==selected_graph &&
         _active_node_views.value(dock->property("graphNode").toInt())==key);
    if(current && dock->property("graphNode").isValid() && _application.state().data_flow_graph){
        if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(dock->property("graphNode").toUInt());model && numeric_editor::is_graph_view(model->name())){
            model->edit_display_name(title);refresh_graph_title(dock);_workspace_save_timer.start();return;
        }
    }
    auto preferences=_application.state().file_configs.ui.graph_views.value(key).toObject();
    if(title.isEmpty())preferences.remove("display_name");else preferences["display_name"]=title;
    _application.state().file_configs.ui.graph_views[key]=preferences;
    refresh_graph_title(dock);_workspace_save_timer.start();
}
void MainWindow::refresh_graph_navigation(const QMap<int,QString> &previous) {
    // A startup Snapshot can be empty before the first Apply. It must not mark
    // the default navigation layout initialized without any graph bindings.
    if(_active_node_views.isEmpty()) {
        if(auto* settings=findChild<SettingsWidget*>())settings->set_graph_actions({});
        return;
    }
    const bool first=!_graph_navigation_initialized;
    const bool migrate=_application.state().file_configs.ui.workspace.value("graph_navigation_version").toInt()!=1;
    _graph_navigation_initialized=true;
    QList<QAction *> actions;
    WorkspaceDock *anchor=nullptr,*selected=nullptr;
    for(auto it=_active_node_views.begin();it!=_active_node_views.end();++it) {
        auto *dock=graph_dock(it.key());if(!dock) continue;
        refresh_graph_title(dock);
        actions.append(dock->toggleViewAction());
        if(!dock->isFloating()) {
            if(!anchor || it.value()=="spectrogram") anchor=dock;
            if(dock->widget()->isVisible()) selected=dock;
        }
    }
    if(_restoring_preset) {
        if(auto* settings=findChild<SettingsWidget*>())settings->set_graph_actions(actions);
        return;
    }
    const bool restoring=_restoring_layout;_restoring_layout=true;
    for(auto it=_active_node_views.begin();it!=_active_node_views.end();++it) {
        auto *dock=graph_dock(it.key());if(!dock) continue;
        // Receiving a runtime binding does not place a representation. Saved
        // ADS groups and explicit placement actions own the dock topology.
        if(migrate && !dock->isFloating()) {
            if(anchor && dock!=anchor) tabifyDockWidget(anchor,dock);
            if(auto *surface=qobject_cast<SpectrogramGraph *>(dock->widget()))
                surface->initialize({240,180},screen()->availableGeometry().size());
            install_graph_name_control(dock);
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
