#include "core_app/main_window.h"
#include "core_app/graph_panel.h"
#include <core_app/plot_composition.h>
#include "core_app/settings_widget.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include <desktop/numeric_project_document.h>
#include <domain/graph_definition.h>
#include <optional>
#include <utility>
#include <DockAreaWidget.h>
#include "barelief/ui_text.h"
#include "barelief/charts/line.h"
#include <QApplication>
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

namespace {
presentation::PlotDescriptor fixed_plot_descriptor(const QString& id) {
    const QMap<QString,QString> kinds{{"rawSamples","AudioView"},{"windowSignal","WindowView"},
        {"windowedSignal","WindowedView"},{"windowFunction","WindowFunctionView"},
        {"fft","FFTView"},{"mel","MelView"},{"spectrogram","SpectrogramView"}};
    auto descriptor=numeric_editor::plot_descriptor_for_kind(kinds.value(id));
    if(id=="spectrogram"){
        descriptor.audio_scales=true;
        descriptor.category_path={"Graphs","Audio","Signals"};
    }
    return descriptor;
}
presentation::PlotDescriptor restore_fixed_plot_descriptor(const QString& id,QJsonObject& preferences) {
    auto descriptor=presentation::plot_descriptor_from_preferences(preferences,fixed_plot_descriptor(id));
    const auto fields=presentation::plot_descriptor_preferences(descriptor);
    for(auto it=fields.begin();it!=fields.end();++it)preferences[it.key()]=it.value();
    return descriptor;
}
Qt::DockWidgetArea flow_edge(const QString& position) {
    if(position=="top")return Qt::TopDockWidgetArea;
    if(position=="left")return Qt::LeftDockWidgetArea;
    if(position=="right")return Qt::RightDockWidgetArea;
    return Qt::BottomDockWidgetArea;
}
Qt::DockWidgetArea graph_edge(const QString& position) {
    if(position=="top")return Qt::BottomDockWidgetArea;
    if(position=="left")return Qt::RightDockWidgetArea;
    if(position=="right")return Qt::LeftDockWidgetArea;
    return Qt::TopDockWidgetArea;
}
}

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
    dock->setWidget(make_panel_content(id,view,dock));
    dock->installEventFilter(this);
    _panels[id] = dock;
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    if (id != "dataFlow") setup_graph_dock_controls(dock);
    bind_panel_visibility(dock,id,flag);
    return dock;
}
QWidget *MainWindow::make_panel_content(const QString& id,QWidget *view,WorkspaceDock *dock) {
    auto* supplied_surface=qobject_cast<SpectrogramGraph*>(view);
    auto* chart=dynamic_cast<AbstractChartView*>(view);
    if(supplied_surface)chart=new LineChartView;
    if(!chart)return view;
    auto preferences=_application.state().file_configs.ui.graph_views.value(id).toObject();
    const auto descriptor=restore_fixed_plot_descriptor(id,preferences);
    _application.state().file_configs.ui.graph_views[id]=preferences;
    if(supplied_surface && !preferences.contains("display_mode"))preferences["display_mode"]="surface3d";
    auto* panel=new GraphPanel(id,chart,preferences,dock,supplied_surface,presentation::plot_capabilities(descriptor));
    bind_panel_presentation(panel);
    return panel;
}
void MainWindow::bind_panel_presentation(GraphPanel *panel) {
    connect(panel,&GraphPanel::presentation_changed,this,[this](const QString& key,const QJsonObject& prefs){
        save_graph_panel_preferences(key,prefs);
    });
    connect(panel,&GraphPanel::renderer_changed,this,[this]{refresh_vector_delivery_queues();_numeric_render_dirty=true;render_numeric_views();});
}
void MainWindow::save_graph_panel_preferences(const QString& id,const QJsonObject& prefs) {
    auto saved=_application.state().file_configs.ui.graph_views.value(id).toObject();
    for(auto it=prefs.constBegin();it!=prefs.constEnd();++it)saved[it.key()]=it.value();
    _application.state().file_configs.ui.graph_views[id]=saved;
    if(prefs.size()!=1 || !prefs.contains("camera"))_numeric_render_dirty=true;
    _workspace_save_timer.start();
}
void MainWindow::sync_panel_toggle_controls(const QString& id,bool open) {
    const QMap<QString,QString> controls{{"rawSamples","showRawSamples"},{"windowSignal","showWindowSignal"},
        {"windowedSignal","showWindowedSignal"},{"windowFunction","showWindowFunction"},{"fft","showFFT"},
        {"mel","showMel"},{"spectrogram","showSpectrogram"}};
    if(!controls.contains(id))return;
    const auto name=controls.value(id);
    if(auto* action=findChild<QAction*>("toggle_"+name)){
        const QSignalBlocker blocker(action);action->setChecked(open);
    }
}
void MainWindow::bind_panel_visibility(WorkspaceDock *dock,const QString& id,bool UIApplicationConfig::*flag) {
    connect(dock, &WorkspaceDock::viewToggled, this, [this, flag, id](bool open) {
        panel_view_toggled(id,flag,open);
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
}
void MainWindow::panel_view_toggled(const QString& id,bool UIApplicationConfig::*flag,bool open) {
    if(_restoring_layout)return;
    _application.state().file_configs.ui.*flag=open;
    sync_panel_toggle_controls(id,open);
    _workspace_save_timer.start();
    refresh_subscriptions();
}
void MainWindow::restore_default_dock_groups() {
    for(auto* dock:std::as_const(_panels))removeDockWidget(dock);
    addDockWidget(Qt::LeftDockWidgetArea,_panels["spectrogram"]);
    splitDockWidget(_panels["spectrogram"],_panels["dataFlow"],Qt::Vertical);
    for(const auto& id:{"fft","mel","rawSamples","windowSignal","windowedSignal","windowFunction"}){
        addDockWidget(Qt::LeftDockWidgetArea,_panels[id]);
        tabifyDockWidget(_panels["spectrogram"],_panels[id]);
    }
}
void MainWindow::reset_workspace_flags() {
    auto& ui=_application.state().file_configs.ui;
    ui.show_spectrogram=ui.show_data_flow=true;
    ui.show_amplitudes=ui.show_test_amplitudes=ui.show_raw_samples=ui.show_window_signal=
        ui.show_windowed_signal=ui.show_window_function=false;
    update_chart_visibility(ui);
}
void MainWindow::reset_workspace() {
    const bool restoring = _restoring_layout;
    _restoring_layout = true;
    if (!_default_workspace_state.isEmpty()) {
        // Let ADS restore its tab groups atomically instead of removing and reusing
        // docks while old floating/tab layouts still refer to them.
        restoreState(_default_workspace_state, 1);
    } else restore_default_dock_groups();
    reset_workspace_flags();
    _panels["spectrogram"]->raise();
    resizeDocks({_panels["spectrogram"], _panels["dataFlow"]}, {560, 320}, Qt::Vertical);
    if (_default_workspace_state.isEmpty())
        _default_workspace_state = saveState(1);
    _restoring_layout = restoring;
    refresh_subscriptions();
}
void MainWindow::save_panel_visibility(QJsonObject& visibility) {
    for(auto it=_panels.begin();it!=_panels.end();++it)visibility[it.key()]=!it.value()->isClosed();
    auto& ui=_application.state().file_configs.ui;
    for(auto it=_node_views.begin();it!=_node_views.end();++it){
        const bool open=!it.value().dock->isClosed();
        visibility["nodeView_"+it.key()]=open;
        auto preferences=ui.graph_views[it.key()].toObject();
        preferences["open"]=open;ui.graph_views[it.key()]=preferences;
    }
}
void MainWindow::save_workspace_flags(const QJsonObject& visibility) {
    auto& ui=_application.state().file_configs.ui;
    ui.show_spectrogram=visibility["spectrogram"].toBool();
    ui.show_data_flow=visibility["dataFlow"].toBool();
    ui.show_amplitudes=visibility["fft"].toBool();
    ui.show_test_amplitudes=visibility["mel"].toBool();
    ui.show_raw_samples=visibility["rawSamples"].toBool();
    ui.show_window_signal=visibility["windowSignal"].toBool();
    ui.show_windowed_signal=visibility["windowedSignal"].toBool();
    ui.show_window_function=visibility["windowFunction"].toBool();
}
void MainWindow::save_workspace() {
    QJsonObject visibility;
    save_panel_visibility(visibility);
    auto &ui = _application.state().file_configs.ui;
    save_workspace_flags(visibility);
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
void MainWindow::clear_preset_node_views() {
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
}
void MainWindow::restore_preset_graph_preferences(const QJsonObject& presentation) {
    auto& ui=_application.state().file_configs.ui;
    ui.graph_views=presentation.value("graph_views").toObject();ui.graph_rendering=presentation.value("graph_rendering").toObject();
    for(auto it=_panels.begin();it!=_panels.end();++it)if(auto* panel=qobject_cast<GraphPanel*>(it.value()->widget())){
        auto preferences=ui.graph_views.value(it.key()).toObject();
        const auto descriptor=restore_fixed_plot_descriptor(it.key(),preferences);
        ui.graph_views[it.key()]=preferences;
        panel->set_plot_capabilities(presentation::plot_capabilities(descriptor));
    }
    const auto session_geometry=ui.workspace.value("geometry");
    ui.workspace=presentation.value("workspace").toObject();
    ui.workspace.remove("geometry");
    if (!session_geometry.isUndefined()) ui.workspace["geometry"]=session_geometry;
}
void MainWindow::restore_preset_node_visibility(bool restored) {
    auto& ui=_application.state().file_configs.ui;
    const auto choices=ui.workspace.value("visibility").toObject();
    for(auto it=_node_views.begin();it!=_node_views.end();++it) {
        const auto prefs=ui.graph_views.value(it.key()).toObject();
        const bool current=it->device==_application.device_id() && it->graph==QString::fromStdString(_view_draft->graph_id);
        const bool open=current && (restored && choices.value("nodeView_"+it.key()).isBool()
            ?choices.value("nodeView_"+it.key()).toBool():prefs.value("open").toBool(true));
        if(it->dock->isClosed()==open)it->dock->toggleView(open);
    }
}
void MainWindow::restore_preset_dock_layout(bool& restored) {
    auto& ui=_application.state().file_configs.ui;
    const auto state=QByteArray::fromBase64(ui.workspace.value("ads_state").toString().toLatin1());
    restored=ui.workspace.value("version").toInt()==1 && ui.workspace.value("layout_version").toInt()==5 && !state.isEmpty() && restoreState(state,1);
    if(restored)return;
    auto* flow=_panels.value("dataFlow");placeDock(flow,nullptr,Qt::BottomDockWidgetArea);
    WorkspaceDock* anchor=nullptr;
    const auto docks=graph_docks();
    for(auto* dock:docks) {
        if(!anchor){placeDock(dock,flow,Qt::TopDockWidgetArea);anchor=dock;}
        else tabifyDockWidget(anchor,dock);
    }
    if(anchor)anchor->raise();
}
void MainWindow::restore_preset_surface() {
    auto& ui=_application.state().file_configs.ui;
    for(auto it=_panels.begin();it!=_panels.end();++it)
        if(auto* panel=qobject_cast<GraphPanel*>(it.value()->widget()))
            panel->restore_presentation(ui.graph_views.value(it.key()).toObject());
    const auto surface=ui.graph_views.value("spectrogram").toObject();
    _spectrogram_view->set_presentation_state(surface.value("renderer").toObject());
    restore_preset_camera(surface);
    update_chart_visibility(ui);apply_surface_settings();
}
void MainWindow::restore_preset_camera(const QJsonObject& surface) {
    auto& ui=_application.state().file_configs.ui;
    auto camera=surface.value("camera").toObject();
    if(camera.isEmpty()){SpectrogramGraph defaults;camera=defaults.camera_state();}
    _spectrogram_view->set_camera_state(camera);
    _spectrogram_view->set_side_panel_ratio(float(surface.value("side_ratio").toDouble(.25)));
    ui.camera=_spectrogram_view->camera_state();
    ui.spectrogram_side_panel_ratio=float(surface.value("side_ratio").toDouble(.25));
}
void MainWindow::finish_preset_restore(bool previous_layout) {
    const auto docks=graph_docks();
    for(auto* dock:docks)refresh_graph_title(dock);
    if(auto* settings=findChild<SettingsWidget*>())settings->refresh_graph_preferences();
    _restoring_preset=false;_restoring_layout=previous_layout;
    auto& ui=_application.state().file_configs.ui;
    ui.workspace["graph_navigation_version"]=1;
    save_workspace();refresh_subscriptions();_workspace_save_timer.start();
}
void MainWindow::restore_preset_visibility_flags(const QJsonObject& presentation) {
    auto& ui=_application.state().file_configs.ui;
    const auto visible=[&](const char* field,const QString& key) {
        const bool exists=key=="dataFlow" || std::any_of(_active_node_views.cbegin(),_active_node_views.cend(),[&](const auto& value){return value==key;});
        return exists && presentation.value(field).toBool(true);
    };
    ui.show_raw_samples=visible("show_raw_samples","rawSamples");ui.show_window_signal=visible("show_window_signal","windowSignal");
    ui.show_windowed_signal=visible("show_windowed_signal","windowedSignal");ui.show_window_function=visible("show_window_function","windowFunction");
    ui.show_amplitudes=visible("show_amplitudes","fft");ui.show_test_amplitudes=visible("show_test_amplitudes","mel");
    ui.show_spectrogram=visible("show_spectrogram","spectrogram");ui.show_data_flow=visible("show_data_flow","dataFlow");
}
void MainWindow::begin_preset_restore(domain::NumericProcessingGraph&& draft,const QJsonObject& presentation) {
    _restoring_layout=_restoring_preset=true;
    _workspace_save_timer.stop();_camera_save_timer.stop();_last_graph_slot.clear();
    clear_preset_node_views();
    _view_draft=std::move(draft);_view_draft_device=_application.device_id();
    restore_preset_graph_preferences(presentation);
    restore_node_views();sync_node_views(_application.client().snapshot().graph);
    restore_preset_visibility_flags(presentation);
}
void MainWindow::restore_preset_presentation(const QJsonObject& supplied) {
    QJsonObject presentation;
    if(!supplied.isEmpty() && !desktop::decode_numeric_preset_presentation(QJsonDocument(supplied).toJson(QJsonDocument::Compact),presentation))return;
    domain::NumericProcessingGraph draft;
    auto* editor=data_flow_editor();if(!editor || !editor->numeric_draft(draft))return;
    const bool restoring=_restoring_layout;
    begin_preset_restore(std::move(draft),presentation);
    bool restored=false;restore_preset_dock_layout(restored);
    restore_preset_node_visibility(restored);
    restore_preset_surface();
    finish_preset_restore(restoring);
}
void MainWindow::collect_numeric_subscription_endpoints(std::vector<pipeline::EndpointIdentity>& endpoints) const {
    for(auto it=_numeric_bindings.cbegin();it!=_numeric_bindings.cend();++it) {
        if(!graph_subscription_exposed(it.key()))continue;
        const auto& binding=it.value();
        if(binding.device!=_application.device_id())continue;
        if(std::find(endpoints.begin(),endpoints.end(),binding.endpoint)==endpoints.end())
            endpoints.push_back(binding.endpoint);
    }
    collect_metric_subscription_endpoints(endpoints);
}
bool MainWindow::graph_subscription_exposed(const QString& key) const {
    auto* dock=_node_views.contains(key)?_node_views[key].dock:_panels.value(key);
    return dock && !dock->isHidden() && dock->property("renderExposed").toBool();
}
void MainWindow::collect_metric_subscription_endpoints(std::vector<pipeline::EndpointIdentity>& endpoints) const {
    for(auto it=_metric_bindings.cbegin();it!=_metric_bindings.cend();++it)if(graph_subscription_exposed(it.key()))for(const auto& binding:it.value()) {
        if(binding.device==_application.device_id() && std::find(endpoints.begin(),endpoints.end(),binding.endpoint)==endpoints.end())
            endpoints.push_back(binding.endpoint);
    }
}
void MainWindow::refresh_subscriptions() {
    // A restoration publishes one complete binding set after ADS has settled.
    if(_restoring_preset)return;
    refresh_vector_delivery_queues();
    std::vector<pipeline::EndpointIdentity> endpoints;
    collect_numeric_subscription_endpoints(endpoints);
    _application.set_numeric_endpoints(std::move(endpoints));
    if(!_restoring_layout)render_numeric_views();
}
void MainWindow::recover_floating_panel(QWidget *dock,const QList<QScreen *>& screens) {
    QScreen *target=nullptr;
    const auto title=QRect(dock->frameGeometry().topLeft(),QSize(dock->width(),30));
    for(auto *screen:screens)
        if(screen->availableGeometry().contains(title.center())){target=screen;break;}
    if(!target)target=screens.front();
    const auto available=target->availableGeometry().adjusted(8,8,-8,-8);
    dock->resize(dock->size().boundedTo(available.size()));
    auto position=dock->frameGeometry().topLeft();
    position.setX(std::clamp(position.x(),available.left(),
        std::max(available.left(),available.right()-dock->frameGeometry().width()+1)));
    position.setY(std::clamp(position.y(),available.top(),
        std::max(available.top(),available.bottom()-dock->frameGeometry().height()+1)));
    dock->move(position);
}
void MainWindow::recover_offscreen_panels() {
    const auto screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return;
    QSet<QWidget *> visited;
    const auto panels=workspaceDocks();
    for (auto *panel : panels)
        if (auto *dock = panel->floatingDockContainer(); dock && !visited.contains(dock)) {
            visited.insert(dock);
            recover_floating_panel(dock,screens);
        }
}
void MainWindow::remember_graph_slot(QObject *watched) {
    for(auto *widget=qobject_cast<QWidget *>(watched);widget;widget=widget->parentWidget()) {
        if(auto *dock=qobject_cast<WorkspaceDock *>(widget)) {
            if(is_graph_dock(dock) && in_editor_window(dock) && !dock->isHidden())_last_graph_slot=dock;
            break;
        }
    }
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (!_restoring_layout && (event->type()==QEvent::MouseButtonPress || event->type()==QEvent::FocusIn))
        remember_graph_slot(watched);
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
WorkspaceDock *MainWindow::graph_open_slot() const {
    auto *slot=_last_graph_slot.data();
    return slot && !slot->isHidden() && is_graph_dock(slot) && in_editor_window(slot)
        ? slot : find_editor_graph_slot();
}
WorkspaceDock *MainWindow::find_editor_graph_slot() const {
    for(auto it=_active_node_views.cbegin();it!=_active_node_views.cend();++it)
        if(_render_visible.value(it.value()) && in_editor_window(graph_dock(it.key())))return graph_dock(it.key());
    const auto candidates=graph_docks();
    for(auto *candidate:candidates)
        if(in_editor_window(candidate) && candidate->property("renderExposed").toBool())return candidate;
    return nullptr;
}
bool MainWindow::focus_existing_graph(int node) {
    const auto candidates=graph_docks();
    for(auto *candidate:candidates)
        if(candidate->property("graphNode").toInt()==node && in_editor_window(candidate) && !candidate->isHidden()) {
            candidate->raise();_last_graph_slot=candidate;return true;
        }
    return false;
}
void MainWindow::place_graph_in_slot(WorkspaceDock *dock,WorkspaceDock *slot) {
    if(slot==dock)return;
    if(!tabifiedDockWidgets(slot).contains(dock)) {
        dock->setFloating(false);
        tabifyDockWidget(slot,dock);
    }
    // A previously closed representation in this same tab group replaces
    // the selected slot too. Already-open representations were focused above.
    slot->hide();
}
void MainWindow::focus_or_place_graph(WorkspaceDock *dock,WorkspaceDock *slot,int node) {
    if(focus_existing_graph(node))return;
    if(dock->isFloating())dock=new_graph_view(node);
    if(!dock)return;
    place_graph_in_slot(dock,slot);
    if(auto *surface=qobject_cast<SpectrogramGraph *>(dock->widget()))
        surface->initialize({240,180},screen()->availableGeometry().size());
    install_graph_name_control(dock);
    dock->show();dock->raise();
    _last_graph_slot=dock;
    // Activation from the editor must not steal keyboard focus during dragging
    // or prevent subsequent copy/paste shortcuts from reaching the editor.
    _workspace_save_timer.start();refresh_subscriptions();
}
void MainWindow::open_graph_node(int node) {
    auto *dock=graph_dock(node);
    auto *slot=graph_open_slot();
    if(!slot)return; // Only an explicit context command creates a viewing area.
    if(!dock)dock=new_graph_view(node);
    if(!dock)return;
    focus_or_place_graph(dock,slot,node);
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
    for(auto it=_active_node_views.cbegin();it!=_active_node_views.cend();++it)
        if(auto *dock=graph_dock(it.key())) result.append(dock);
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
WorkspaceDock *MainWindow::create_graph_view_instance(int node,std::optional<domain::PortId> port,
    const domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& found,
    const QJsonObject& document) {
    const auto instance=_next_view_instance++;
    const auto identity=QString::fromLatin1(QJsonDocument(QJsonArray{_application.device_id(),QString::fromStdString(graph.graph_id),node,instance}).toJson(QJsonDocument::Compact).toHex());
    if(port){auto preferences=_application.state().file_configs.ui.graph_views[identity].toObject();preferences["port"]=int(*port);_application.state().file_configs.ui.graph_views[identity]=preferences;}
    const auto key=ensure_node_view(node,QString::fromStdString(found.kind),_application.device_id(),QString::fromStdString(graph.graph_id),false,instance,&found);
    // Only the authoritative snapshot binds data. Opening an unapplied draft
    // creates an unavailable presentation, never applies or starts processing.
    if(!document.isEmpty())sync_node_views(document);
    if(!_active_node_views.contains(node))_active_node_views[node]=key;
    return _node_views.value(key).dock;
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
    return create_graph_view_instance(node,port,graph,*found,document);
}
void MainWindow::populate_graph_ports(int node,const domain::NumericProcessingGraph& graph,QMenu *menu) {
    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==node;});
    domain::ResolvedProcessingNode resolved;
    if(found!=graph.nodes.end() && domain::resolve_processing_node(*found,numeric_editor::limits(),resolved)) {
        const bool input=resolved.role==domain::NodeRole::Sink;
        const auto ports=input?domain::data_inputs(*found):domain::data_outputs(*found);
        if(ports.size()<2)return;
        int data_ordinal=0;
        for(const auto& port:ports) {
            auto* action=menu->addAction(ui::text(input?"Input port %1":"Output port %1").arg(++data_ordinal));
            action->setObjectName(QStringLiteral("openGraphPort%1").arg(port.id));
            connect(action,&QAction::triggered,this,[this,node,port=port.id] {
                if(auto* dock=new_graph_view(node,port))place_graph(dock,1);
            });
        }
    }
}
void MainWindow::populate_graph_focus(int node,QMenu *menu) {
    auto *focus=menu->addMenu(ui::text("Focus representation")); focus->setObjectName("graphRepresentations");
    int internal=0,external=0;
    const auto docks=graph_docks();
    for(auto *dock:docks) {
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
}
void MainWindow::populate_graph_placements(int node,QMenu *menu) {
    const char *labels[]{"Open in separate window","Open in new tab","Open in this window"};
    for(int placement=0;placement<3;++placement) {
        auto *action=menu->addAction(ui::text(labels[placement]));
        action->setObjectName(QStringLiteral("openGraphPlacement%1").arg(placement));
        if(placement<2) { action->setShortcut(QKeySequence(placement ? "Alt+G" : "Alt+Shift+G")); action->setShortcutVisibleInContextMenu(true); }
        connect(action,&QAction::triggered,this,[this,node,placement] { add_graph_node(node,placement); });
    }
}
void MainWindow::populate_graph_targets(int node,QMenu *menu) {
    auto *beside=menu->addMenu(ui::text("Open beside")); beside->setObjectName("graphPlacementTargets");
    int index=0;
    const auto targets=graph_docks();
    for(auto *target:targets) if(in_editor_window(target) && !target->isHidden()) {
        auto *action=beside->addAction(ui::text("Internal area %1").arg(++index)+": "+target->windowTitle());
        connect(action,&QAction::triggered,this,[this,node,target] { _last_graph_slot=target; add_graph_node(node,2); });
    }
    beside->setEnabled(!beside->isEmpty());
}
void MainWindow::populate_graph_menu(int node,QMenu *menu) {
    domain::NumericProcessingGraph graph;
    if(!graph_for_view(node,graph))return;
    const auto shown=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==node;});
    if(shown==graph.nodes.end() || !numeric_editor::is_graph_view(QString::fromStdString(shown->kind)))return;
    if(graph_for_view(node,graph))populate_graph_ports(node,graph,menu);
    populate_graph_focus(node,menu);
    populate_graph_placements(node,menu);
    populate_graph_targets(node,menu);
}
void MainWindow::add_graph_node(int node,int placement) {
    auto* dock=new_graph_view(node);
    if(!dock)return;
    place_graph(dock,placement);
    if(in_editor_window(dock)) _last_graph_slot=dock;
}
void MainWindow::update_graph_exposure(const QString& key,WorkspaceDock *dock,bool exposed) {
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
}
void MainWindow::set_graph_exposed(const QString &key, WorkspaceDock *dock, bool exposed) {
    update_graph_exposure(key,dock,exposed);
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
        moveDockToHostEdge(flow,flow_edge(position));
        return;
    }
    // A new representation is initially tabbed with the fixed graph area.
    // Keep that group when all older graph tabs are closed: opening a new tab
    // must not turn it into a separate area or reopen a closed representation.
    if(placement==1 && !anchor && in_editor_window(dock) && !tabifiedDockWidgets(dock).isEmpty())return;
    if(anchor && placement==1)tabifyDockWidget(anchor,dock);
    else if(anchor)placeDock(dock,anchor,placement==3 ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea);
    else placeDock(dock,flow,graph_edge(position));
}
WorkspaceDock *MainWindow::graph_placement_anchor(WorkspaceDock *dock) const {
    auto *anchor=_last_graph_slot.data();
    if(anchor==dock || !in_editor_window(anchor) || anchor->isHidden())anchor=nullptr;
    if(!anchor) {
        const auto candidates=graph_docks();
        for(auto *candidate:candidates)
            if(candidate!=dock && in_editor_window(candidate) && !candidate->isHidden()){anchor=candidate;break;}
    }
    return anchor;
}
void MainWindow::expose_floating_graph(WorkspaceDock *dock) {
    QString key;
    for(auto it=_active_node_views.cbegin();it!=_active_node_views.cend();++it)
        if(graph_dock(it.key())==dock){key=it.value();break;}
    if(!key.isEmpty())set_graph_exposed(key,dock,dock->isVisible());
}
void MainWindow::place_graph(WorkspaceDock *dock, int placement) {
    const bool restoring = _restoring_layout;
    _restoring_layout = true;
    // Reuse the dock and renderer: docking never recreates the camera or graph.
    if (placement == 0) {
        dock->setFloating(true);
    } else {
        auto *anchor=graph_placement_anchor(dock);
        dock->setFloating(false);
        rebuild_graph_areas(dock,anchor,placement);
    }
    dock->show();
    layout()->activate();
    dock->raise();
    if(in_editor_window(dock)) _last_graph_slot=dock;
    // Floating an inactive tab can retain its previous tab visibility signal.
    // A shown top-level dock is exposed independently of that former group.
    if(dock->isFloating())expose_floating_graph(dock);
    _restoring_layout = restoring;
    _workspace_save_timer.start(); refresh_subscriptions();
}
void MainWindow::bind_graph_dock_placement(WorkspaceDock *dock) {
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
}
void MainWindow::bind_graph_dock_visibility(WorkspaceDock *dock) {
    connect(dock,&WorkspaceDock::topLevelChanged,this,[this,dock](bool) {
        QTimer::singleShot(0,dock,[this,dock] {
            QString key=_panels.key(dock);
            if(key.isEmpty())for(auto it=_node_views.cbegin();it!=_node_views.cend();++it)
                if(it->dock==dock){key=it.key();break;}
            if(!key.isEmpty())set_graph_exposed(key,dock,dock->isVisible());
            refresh_subscriptions();
        });
    });
}
void MainWindow::setup_graph_dock_controls(WorkspaceDock *dock) {
    bind_graph_dock_placement(dock);
    connect(&ui::Language::instance(),&ui::Language::changed,dock,[this,dock]{refresh_graph_title(dock);});
    refresh_graph_title(dock);
    if (dock==_panels.value("dataFlow")) {
        if(auto *editor=qobject_cast<SchemeEditorWidget *>(dock->widget())) {
            dock->setHeaderControls(editor->toolbar());
        }
    }
    bind_graph_dock_visibility(dock);
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
bool MainWindow::is_current_graph_dock(WorkspaceDock *dock,const QString& key) const {
    const auto identity=QJsonDocument::fromJson(QByteArray::fromHex(key.toLatin1())).array();
    const auto selected_graph=_view_draft?QString::fromStdString(_view_draft->graph_id):_application.graph_id();
    const auto endpoint=dock->property("numericEndpoint").toMap();
    return (identity.size()>=3 && identity[0].toString()==_application.device_id() && identity[1].toString()==selected_graph) ||
        (_panels.value(key)==dock && endpoint.value("device").toString()==_application.device_id() && endpoint.value("graph").toString()==selected_graph &&
         _active_node_views.value(dock->property("graphNode").toInt())==key);
}
void MainWindow::clear_scalar_input_binding(GraphPanel *panel) {
    const auto previousConnections=panel->property("scalarInputConnections").value<QList<QMetaObject::Connection>>();
    for(const auto& connection:previousConnections)
        QObject::disconnect(connection);
    panel->setProperty("scalarInputConnections",QVariant{});
    panel->setProperty("scalarInputsModel",QVariant{});
    panel->set_scalar_inputs({},false);
}
QList<QMetaObject::Connection> MainWindow::scalar_input_command_connections(GraphPanel *panel,AudioNodeModel *model) {
    const QPointer<AudioNodeModel> guard(model);
    QList<QMetaObject::Connection> connections;
    connections.append(connect(panel,&GraphPanel::add_scalar_input_requested,panel,[guard]{if(guard)guard->add_scalar_input();}));
    connections.append(connect(panel,&GraphPanel::remove_scalar_input_requested,panel,[guard](int port){
        if(guard && port>=0 && port<=65535)guard->remove_scalar_input(domain::PortId(port));
    }));
    connections.append(connect(panel,&GraphPanel::scalar_input_name_requested,panel,[guard](int port,const QString& name){
        if(guard && port>=0 && port<=65535)guard->edit_scalar_input_name(domain::PortId(port),name);
    }));
    connections.append(connect(panel,&GraphPanel::scalar_input_enabled_requested,panel,[guard](int port,bool enabled){
        if(guard && port>=0 && port<=65535)guard->set_scalar_input_enabled(domain::PortId(port),enabled);
    }));
    return connections;
}
void MainWindow::bind_scalar_input_lifecycle(GraphPanel *panel,AudioNodeModel *model,
    QList<QMetaObject::Connection>& connections) {
    connections.append(connect(model,&QObject::destroyed,panel,[panel,model]{
        if(panel->property("scalarInputsModel").value<QObject*>()==model){
            panel->setProperty("scalarInputsModel",QVariant{});
            panel->set_scalar_inputs({},false);
        }
    }));
    panel->setProperty("scalarInputsModel",QVariant::fromValue(static_cast<QObject*>(model)));
    panel->setProperty("scalarInputConnections",QVariant::fromValue(connections));
}
void MainWindow::bind_scalar_input_actions(GraphPanel *panel,AudioNodeModel *model) {
    const auto previousConnections=panel->property("scalarInputConnections").value<QList<QMetaObject::Connection>>();
    for(const auto& connection:previousConnections)
        QObject::disconnect(connection);
    auto connections=scalar_input_command_connections(panel,model);
    bind_scalar_input_lifecycle(panel,model,connections);
}
void MainWindow::refresh_scalar_input_binding(WorkspaceDock *dock,AudioNodeModel *model) {
    auto* panel=qobject_cast<GraphPanel*>(dock->widget());
    if(!panel)return;
    QVariantList inputs;
    const auto metadata=model->presentation().value("scalar_inputs").toObject();
    if(const auto* numeric=model->numeric_node())for(const auto& input:numeric->inputs) {
        const auto preferences=metadata.value(QString::number(input.id)).toObject();
        inputs.append(QVariantMap{{"port",int(input.id)},{"name",preferences.value("display_name").toString()},
            {"enabled",preferences.value("enabled").toBool(true)},{"removeEnabled",numeric->inputs.size()>1}});
    }
    panel->set_scalar_inputs(inputs,inputs.size()<qsizetype(numeric_editor::limits().max_ports_per_node));
    if(panel->property("scalarInputsModel").value<QObject*>()!=model)bind_scalar_input_actions(panel,model);
}
void MainWindow::bind_graph_caption(WorkspaceDock *dock,AudioNodeModel *model,const QString& title) {
    if(dock->property("graphCaptionModel").value<QObject*>()==model)return;
    dock->setProperty("graphCaptionModel",QVariant::fromValue(static_cast<QObject*>(model)));
    connect(model,&QObject::destroyed,dock,[dock,model]{
        if(dock->property("graphCaptionModel").value<QObject*>()==model)dock->setProperty("graphCaptionModel",QVariant{});
    });
    if(model->presentation().value("display_name").toString().trimmed().isEmpty() && !title.isEmpty()){
        auto presentation=model->presentation();presentation["display_name"]=title;model->set_presentation(presentation);
    }
    connect(model,&AudioNodeModel::presentation_changed,dock,[this,dock]{
        refresh_graph_title(dock);_workspace_save_timer.start();
        const auto kind=domain::node_kind(dock->property("graphKind").toString().toStdString());
        if(kind && domain::is_scalar_view(*kind))
            QTimer::singleShot(0,this,[this]{sync_node_views(_application.client().snapshot().graph);});
    });
}
void MainWindow::save_graph_display_name(const QString& key,const QString& title) {
    auto preferences=_application.state().file_configs.ui.graph_views.value(key).toObject();
    if(title.isEmpty())preferences.remove("display_name");else preferences["display_name"]=title;
    _application.state().file_configs.ui.graph_views[key]=preferences;
}
QString MainWindow::graph_title(WorkspaceDock *dock,const QString& key,AudioNodeModel *model,QString title) {
    if(model && numeric_editor::is_graph_view(model->name())){
        const auto kind=domain::node_kind(model->name().toStdString());
        if(kind && domain::is_scalar_view(*kind))refresh_scalar_input_binding(dock,model);
        bind_graph_caption(dock,model,title);
        title=model->presentation().value("display_name").toString().trimmed();
        save_graph_display_name(key,title);
    }
    if(title.isEmpty()) {
        const auto kind=dock->property("graphKind").toString();
        title=kind.isEmpty()?ui::text(dock->property("graphTitleSource").toString().toUtf8().constData()):
            AudioNodeModel(kind).caption()+" #"+QString::number(dock->property("graphNode").toInt());
    }
    return title;
}
void MainWindow::refresh_graph_title(WorkspaceDock *dock) {
    const auto key=dock->property("graphViewKey").toString();if(key.isEmpty())return;
    auto title=_application.state().file_configs.ui.graph_views.value(key).toObject().value("display_name").toString().trimmed();
    const bool current=is_current_graph_dock(dock,key);
    auto* model=current && dock->property("graphNode").isValid() && _application.state().data_flow_graph?
        _application.state().data_flow_graph->delegateModel<AudioNodeModel>(dock->property("graphNode").toUInt()):nullptr;
    const auto kind=model?domain::node_kind(model->name().toStdString()):std::nullopt;
    if(!kind || !domain::is_scalar_view(*kind))if(auto* panel=qobject_cast<GraphPanel*>(dock->widget())) {
        clear_scalar_input_binding(panel);
    }
    title=graph_title(dock,key,model,title);
    dock->setWindowTitle(title);dock->toggleViewAction()->setText(title);
}
void MainWindow::rename_graph(WorkspaceDock *dock,const QString &name) {
    const auto key=dock->property("graphViewKey").toString();if(key.isEmpty())return;
    const auto title=name.trimmed();
    if(title.size()>256 || std::any_of(title.begin(),title.end(),[](QChar c){return c.category()==QChar::Other_Control;}))return;
    if(rename_bound_graph(dock,key,title))return;
    save_graph_display_name(key,title);
    refresh_graph_title(dock);_workspace_save_timer.start();
}
bool MainWindow::rename_bound_graph(WorkspaceDock *dock,const QString& key,const QString& title) {
    if(!is_current_graph_dock(dock,key) || !dock->property("graphNode").isValid() ||
       !_application.state().data_flow_graph)return false;
    auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(dock->property("graphNode").toUInt());
    if(!model || !numeric_editor::is_graph_view(model->name()))return false;
    model->edit_display_name(title);refresh_graph_title(dock);_workspace_save_timer.start();return true;
}
void MainWindow::graph_navigation_actions(WorkspaceDock *&anchor,WorkspaceDock *&selected) {
    for(auto it=_active_node_views.begin();it!=_active_node_views.end();++it) {
        auto *dock=graph_dock(it.key());if(!dock) continue;
        refresh_graph_title(dock);
        if(!dock->isFloating()) {
            if(!anchor || it.value()=="spectrogram") anchor=dock;
            if(dock->widget()->isVisible()) selected=dock;
        }
    }
}
void MainWindow::migrate_graph_navigation(WorkspaceDock *anchor) {
    for(auto it=_active_node_views.begin();it!=_active_node_views.end();++it) {
        auto *dock=graph_dock(it.key());if(!dock || dock->isFloating())continue;
        // Receiving a runtime binding does not place a representation. Saved
        // ADS groups and explicit placement actions own the dock topology.
        const bool wasClosed=dock->isClosed();
        if(anchor && dock!=anchor)tabifyDockWidget(anchor,dock);
        if(wasClosed && !dock->isClosed())dock->toggleView(false);
        if(auto *surface=qobject_cast<SpectrogramGraph *>(dock->widget()))
            surface->initialize({240,180},screen()->availableGeometry().size());
        install_graph_name_control(dock);
    }
}
void MainWindow::refresh_graph_navigation(const QMap<int,QString> &previous) {
    // A startup Snapshot can be empty before the first Apply. It must not mark
    // the default navigation layout initialized without any graph bindings.
    if(_active_node_views.isEmpty())return;
    const bool first=!_graph_navigation_initialized;
    const bool migrate=_application.state().file_configs.ui.workspace.value("graph_navigation_version").toInt()!=1;
    _graph_navigation_initialized=true;
    WorkspaceDock *anchor=nullptr,*selected=nullptr;
    graph_navigation_actions(anchor,selected);
    if(_restoring_preset)return;
    const bool restoring=_restoring_layout;_restoring_layout=true;
    if(migrate)migrate_graph_navigation(anchor);
    if(migrate && anchor && !anchor->isClosed()) selected=anchor;
    finish_graph_navigation(selected,restoring,migrate,first,previous);
}
void MainWindow::finish_graph_navigation(WorkspaceDock *selected,bool restoring,bool migrate,bool first,
                                         const QMap<int,QString>& previous) {
    if(selected && !selected->isClosed())selected->raise();
    _restoring_layout=restoring;
    if(migrate || (!first && previous!=_active_node_views)) {
        _application.state().file_configs.ui.workspace["graph_navigation_version"]=1;
        save_workspace();_workspace_save_timer.start();
    }
}
