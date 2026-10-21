#include "core_app/main_window.h"
#include <core/performance.h>
#include "core_app/graph_panel.h"
#include <core_app/plot_composition.h>
#include "core_app/settings_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "barelief/surface_settings.h"
#include "barelief/ui_text.h"
#include "barelief/charts/line.h"
#include "barelief/charts/freq.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QScreen>
#include <QHash>
#include <QStatusBar>
#include <algorithm>
#include <utility>
#include <cmath>
#include <desktop/numeric_project_document.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <QLabel>
#include <QLayout>
#include <barelief/numeric_metrics_view.h>

Q_DECLARE_LOGGING_CATEGORY(core_app_window)

namespace {
bool scalar_view_kind(std::string_view kind) {
    const auto id=domain::node_kind(kind);
    return id && domain::is_scalar_view(*id);
}
void refresh_plot_descriptor(GraphPanel* panel,const presentation::PlotDescriptor& descriptor,const QJsonObject& preferences){
    if(!panel)return;
    const auto capabilities=presentation::plot_capabilities(descriptor);
    const bool sameCapabilities=panel->modes()==capabilities.modes && panel->audio_scales()==capabilities.audio_scales;
    const bool samePreferences=panel->property("restoredViewPreferences").toJsonObject()==preferences;
    if(sameCapabilities && samePreferences)return;
    panel->setProperty("restoredViewPreferences",QVariant::fromValue(preferences));
    panel->set_plot_capabilities(capabilities);
    panel->restore_presentation(preferences);
}
QString joined_surface_errors(const QMap<QString,QString>& errors) {
    QString text;
    bool first = true;
    for (const auto& error : errors) {
        if (!first) text += QStringLiteral(" \u00b7 ");
        text += error;
        first = false;
    }
    return text;
}
bool decode_runtime_view_graph(const QJsonObject& document,domain::NumericProcessingGraph& graph) {
    return !document.isEmpty() && bool(desktop::decode_numeric_graph(
        QJsonDocument(document).toJson(QJsonDocument::Compact),graph));
}
}

QString MainWindow::ensure_node_view(int node, const QString &kind, const QString &device, const QString &graph,
                                     bool open, int instance, const domain::ConfiguredProcessingNode *configured) {
    if (!numeric_editor::is_graph_view(kind)) return {};
    QJsonArray identity{device, graph, node};
    if (instance) identity.append(instance);
    const auto key = QString::fromLatin1(QJsonDocument(identity).toJson(QJsonDocument::Compact).toHex());
    const bool existing = _node_views.contains(key);
    NodeView view{node, device, graph, kind};
    auto *dock = prepare_node_view_dock(key, kind, node, instance, existing);
    view.dock = dock;
    auto preferences = prepare_node_view_preferences(key, kind, instance);
    return install_node_view_renderer(view,key,kind,open,configured,existing,std::move(preferences));
}

QString MainWindow::install_node_view_renderer(NodeView view,const QString& key,const QString& kind,
    bool open,const domain::ConfiguredProcessingNode *configured,bool existing,QJsonObject preferences) {
    auto *dock = view.dock;
    if (existing && _node_views[key].kind == kind) {
        const auto descriptor = presentation::plot_descriptor_from_preferences(
            preferences, numeric_editor::plot_descriptor_for_kind(kind));
        refresh_plot_descriptor(qobject_cast<GraphPanel *>(dock->widget()), descriptor, preferences);
        return key;
    }
    return install_new_node_view(std::move(view),key,kind,open,configured,existing,std::move(preferences));
}
QString MainWindow::install_new_node_view(NodeView view,const QString& key,const QString& kind,
    bool open,const domain::ConfiguredProcessingNode *configured,bool existing,QJsonObject preferences) {
    auto* dock=view.dock;
    if (existing) {
        delete dock->takeWidget();
        _surface_errors.remove(key);
    }
    auto *panel = create_node_view_panel(view, key, kind, preferences, configured);
    bind_node_view_panel(panel, key);
    dock->setWidget(panel);
    install_graph_name_control(dock);
    _node_views[key] = view;
    initialize_node_view_surface(dock, key);
    if (existing) {
        set_graph_exposed(key, dock, dock->property("renderExposed").toBool());
        return key;
    }
    place_new_node_view(dock, key, view.device, view.graph, view.node, kind, open, preferences);
    return key;
}
QJsonObject MainWindow::prepare_node_view_preferences(const QString& key,const QString& kind,int instance) {
    auto preferences = _application.state().file_configs.ui.graph_views.value(key).toObject();
    preferences["kind"] = kind;
    preferences["instance"] = instance;
    const auto catalogue = numeric_editor::plot_descriptor_for_kind(kind);
    auto descriptor = presentation::plot_descriptor_from_preferences(preferences, catalogue);
    if (!preferences.contains("plot_category_path") && descriptor.audio_scales && !catalogue.audio_scales)
        descriptor.category_path = {"Graphs", "Audio", "Signals"};
    const auto explicit_descriptor = presentation::plot_descriptor_preferences(descriptor);
    for (auto it = explicit_descriptor.begin(); it != explicit_descriptor.end(); ++it)
        preferences[it.key()] = it.value();
    _application.state().file_configs.ui.graph_views[key] = preferences;
    // Renderer choice is a view preference, never a numeric semantic type.
    if (kind == "SpectrogramView" && !preferences.contains("display_mode"))
        preferences["display_mode"] = "surface3d";
    return preferences;
}

WorkspaceDock *MainWindow::prepare_node_view_dock(const QString& key,const QString& kind,
                                                    int node,int instance,bool existing) {
    auto *dock = existing ? _node_views[key].dock :
        new WorkspaceDock(AudioNodeModel(kind).caption() + " #" + QString::number(node), this);
    dock->setProperty("graphKind", kind);
    dock->setProperty("graphViewKey", key);
    dock->setObjectName("nodeView_" + key);
    dock->setProperty("graphNode", node);
    dock->setProperty("viewInstance", instance);
    if (!existing) setup_graph_dock_controls(dock);
    refresh_graph_title(dock);
    return dock;
}

namespace {
void configure_piano_first_midi(GraphPanel *panel,const domain::ConfiguredProcessingNode *configured) {
    if (!configured) return;
    domain::ResolvedProcessingNode resolved;
    domain::ProcessingParameterView first;
    if (!domain::resolve_processing_node(*configured, numeric_editor::limits(), resolved)) return;
    if (domain::resolve_processing_parameter(resolved, "first_midi", first) != signal_contracts::Status::ok) return;
    if (const auto *literal = std::get_if<signal_contracts::NumericLiteral>(&first))
        panel->set_first_midi(std::visit([](auto value) { return unsigned(value); }, *literal));
}
}

GraphPanel *MainWindow::create_node_view_panel(NodeView& view,const QString& key,const QString& kind,
    const QJsonObject& preferences,const domain::ConfiguredProcessingNode *configured) {
    view.chart = new LineChartView;
    view.chart->setTitle({});
    view.chart->set_range(-1, 1);
    view.chart->set_auto_resizing(true);
    view.chart->set_auto_gain(true, 1, -1);
    const auto descriptor = presentation::plot_descriptor_from_preferences(
        preferences, numeric_editor::plot_descriptor_for_kind(kind));
    auto *panel = new GraphPanel(key, view.chart, preferences, view.dock, nullptr,
                                 presentation::plot_capabilities(descriptor));
    if (kind == "PianoView") configure_piano_first_midi(panel, configured);
    panel->set_render_fps(_render_fps);
    view.chart->set_render_fps(_render_fps);
    return panel;
}

void MainWindow::bind_node_view_panel(GraphPanel *panel,const QString& key) {
    connect(panel, &GraphPanel::renderer_changed, this, [this, panel, key] {
        on_node_renderer_changed(panel,key);
    });
    connect(panel, &GraphPanel::channel_requested, this,
            [this, key](int plane) { select_numeric_plane(key, std::size_t(std::max(0, plane))); });
    connect(panel, &GraphPanel::presentation_changed, this, [this](const QString& id,const QJsonObject& updates) {
        update_node_view_preferences(id,updates);
    });
}

void MainWindow::on_node_renderer_changed(GraphPanel *panel,const QString& key) {
    if (auto *surface = panel->active_spectrogram()) {
        if (!surface->property("numericCompositionConnected").toBool()) {
            surface->setProperty("numericCompositionConnected", true);
            connect_surface_status(surface,key);
        }
        surface->set_camera_controls(_application.state().file_configs.ui.camera_controls);
        surface->set_surface_settings(SurfaceSettings::effective(
            _application.state().file_configs.ui.graph_rendering,
            _application.state().file_configs.ui.graph_views.value(key).toObject()));
    }
    refresh_vector_delivery_queues();
    _numeric_render_dirty = true;
    render_numeric_views();
}

void MainWindow::update_node_view_preferences(const QString& key,const QJsonObject& updates) {
    auto preferences = _application.state().file_configs.ui.graph_views[key].toObject();
    for (auto it = updates.begin(); it != updates.end(); ++it)
        preferences[it.key()] = it.value();
    _application.state().file_configs.ui.graph_views[key] = preferences;
    if (updates.size() != 1 || !updates.contains("camera")) _numeric_render_dirty = true;
    _workspace_save_timer.start();
}

void MainWindow::initialize_node_view_surface(WorkspaceDock *dock,const QString& key) {
    auto *panel = qobject_cast<GraphPanel *>(dock->widget());
    auto *surface = panel ? panel->active_spectrogram() : nullptr;
    if (!surface) return;
    surface->setProperty("numericCompositionConnected", true);
    connect_surface_status(surface, key);
    surface->set_camera_controls(_application.state().file_configs.ui.camera_controls);
}

void MainWindow::place_new_node_view(WorkspaceDock *dock,const QString& key,const QString& device,
    const QString& graph,int node,const QString& kind,bool open,QJsonObject preferences) {
    set_graph_exposed(key, dock, false);
    connect(dock, &WorkspaceDock::visibilityChanged, this, [this, key, dock](bool visible) {
        set_graph_exposed(key, dock, visible);
        refresh_subscriptions();
    });
    if (auto *anchor = _panels.value("spectrogram"); anchor && in_editor_window(anchor))
        tabifyDockWidget(anchor, dock);
    else
        placeDock(dock, _panels.value("dataFlow"), Qt::TopDockWidgetArea);
    const auto selected_graph = _view_draft ? QString::fromStdString(_view_draft->graph_id) : _application.graph_id();
    dock->toggleView(device == _application.device_id() && graph == selected_graph &&
                     preferences.value("open").toBool(open));
    preferences["device"] = device;
    preferences["graph"] = graph;
    preferences["node"] = node;
    preferences["kind"] = kind;
    preferences["open"] = !dock->isClosed();
    _application.state().file_configs.ui.graph_views[key] = preferences;
    connect(dock, &WorkspaceDock::viewToggled, this,
            [this, key](bool) { record_node_view_visibility(key); });
    dock->installEventFilter(this);
}

void MainWindow::record_node_view_visibility(const QString& key) {
    if (_restoring_layout) return;
    auto preferences = _application.state().file_configs.ui.graph_views[key].toObject();
    preferences["open"] = !_node_views[key].dock->isClosed();
    _application.state().file_configs.ui.graph_views[key] = preferences;
    refresh_subscriptions();
    _workspace_save_timer.start();
}

void MainWindow::restore_node_views(const domain::NumericProcessingGraph* supplied) {
    domain::NumericProcessingGraph draft;
    if (!supplied) {
        auto *model = dynamic_cast<NumericEditorGraphModel *>(_application.state().data_flow_graph.get());
        if (!model) return;
        draft = model->numeric_graph();
        supplied = &draft;
    }
    const auto preferences = _application.state().file_configs.ui.graph_views;
    for (auto it = preferences.begin(); it != preferences.end(); ++it)
        restore_saved_node_view(it.key(), it.value().toObject(), *supplied);
}
void MainWindow::restore_saved_node_view(const QString& key,const QJsonObject& preferences,
    const domain::NumericProcessingGraph& graph) {
    if (preferences.contains("legacy_panel") || !preferences.contains("node") || !preferences.contains("kind"))
        return;
    if (preferences.value("device").toString() != _application.device_id() ||
        preferences.value("graph").toString() != QString::fromStdString(graph.graph_id)) return;
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) {
        return node.id == preferences.value("node").toInt() &&
               numeric_editor::is_graph_view(QString::fromStdString(node.kind));
    });
    if (found == graph.nodes.end() || preferences.value("kind").toString() != QString::fromStdString(found->kind)) {
        auto stale = preferences;
        stale["open"] = false;
        _application.state().file_configs.ui.graph_views[key] = stale;
        return;
    }
    const int instance = preferences.value("instance").toInt();
    _next_view_instance = std::max(_next_view_instance, instance + 1);
    ensure_node_view(preferences.value("node").toInt(), QString::fromStdString(found->kind),
                     preferences.value("device").toString(), preferences.value("graph").toString(),
                     false, instance, &*found);
}

void MainWindow::reset_numeric_view_endpoints() {
    for (const auto docks=graph_docks(); auto *dock:docks) dock->setProperty("numericEndpoint",QVariant{});
}
void MainWindow::sync_node_views(const QJsonObject& document) {
    if (_view_draft && _view_draft_device != _application.device_id()) _view_draft.reset();
    if (document.isEmpty() && !_view_draft) {
        retire_node_view_session();
        return;
    }
    domain::NumericProcessingGraph runtime_graph;
    const bool runtime_available = decode_runtime_view_graph(document,runtime_graph);
    if (!runtime_available && !_view_draft) return;
    update_view_draft();
    const auto& graph = _view_draft ? *_view_draft : runtime_graph;
    restore_node_views(&graph);
    reset_numeric_view_endpoints();
    const auto previous = _active_node_views;
    _active_node_views.clear();
    rebuild_node_view_bindings(graph,runtime_graph,runtime_available);
    complete_node_view_sync(previous);
}

void MainWindow::rebuild_node_view_bindings(const domain::NumericProcessingGraph& graph,
    const domain::NumericProcessingGraph& runtime_graph,bool runtime_available) {
    QSet<QString> used;
    QMap<QString, NumericBinding> bindings;
    bind_primary_node_views(graph, runtime_graph, runtime_available, used, bindings);
    refresh_additional_node_views(graph);
    bind_additional_node_views(graph, runtime_graph, runtime_available, bindings);
    hide_unused_fixed_views(used);
    const auto targets = numeric_view_targets();
    sync_metric_bindings(graph, runtime_graph, runtime_available, targets, bindings);
    clear_unbound_view_targets(targets, bindings);
    sync_numeric_bindings(graph, runtime_graph, runtime_available, std::move(bindings));
}

void MainWindow::complete_node_view_sync(const QMap<int,QString>& previous) {
    refresh_graph_navigation(previous);
    apply_surface_settings();
    refresh_subscriptions();
    render_numeric_views();
}
void MainWindow::retire_node_view_session() {
    reset_numeric_view_endpoints();
    const auto previous = _active_node_views;
    clear_node_views();
    release_node_view_bindings();
    _metric_bindings.clear();
    _numeric_bindings.clear();
    _active_node_views.clear();
    refresh_graph_navigation(previous);
    refresh_subscriptions();
}
void MainWindow::release_node_view_bindings() {
    for (const auto& binding : std::as_const(_numeric_bindings)) _numeric_views.unbind(binding.endpoint, binding.plane);
    for (const auto& ports : std::as_const(_metric_bindings))
        for (const auto& binding : ports) _numeric_views.unbind(binding.endpoint, binding.plane);
}

void MainWindow::update_view_draft() {
    if (!_view_draft) return;
    auto *editor = data_flow_editor();
    if (!editor) return;
    domain::NumericProcessingGraph current;
    if (editor->numeric_draft(current)) _view_draft = std::move(current);
}

bool MainWindow::admits_view_binding(const NumericBinding& binding,const domain::ProcessingPort& shown,
    const domain::NumericProcessingGraph& runtime_graph,bool runtime_available) const {
    if (!runtime_available || runtime_graph.graph_id != binding.endpoint.graph_id) return false;
    const auto node = std::find_if(runtime_graph.nodes.begin(), runtime_graph.nodes.end(), [&](const auto& candidate) {
        return candidate.id == binding.endpoint.node;
    });
    if (node == runtime_graph.nodes.end()) return false;
    const auto& ports = binding.endpoint.direction == domain::PortDirection::Input ? node->inputs : node->outputs;
    const auto port = std::find_if(ports.begin(), ports.end(), [&](const auto& candidate) {
        return candidate.id == binding.endpoint.port;
    });
    return port != ports.end() && signal_contracts::same_contract(port->contract.view(), shown.contract.view());
}

void MainWindow::bind_primary_node_views(const domain::NumericProcessingGraph& graph,
    const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
    QSet<QString>& used,QMap<QString,NumericBinding>& bindings) {
    const QMap<QString,QString> fixed_views{{"AudioView","rawSamples"},{"WindowView","windowSignal"},
        {"WindowedView","windowedSignal"},{"WindowFunctionView","windowFunction"},
        {"FFTView","fft"},{"MelView","mel"},{"SpectrogramView","spectrogram"}};
    for (const auto& node : graph.nodes)
        bind_primary_node_view(node,graph,runtime_graph,runtime_available,fixed_views,used,bindings);
}

void MainWindow::bind_primary_node_view(const domain::ConfiguredProcessingNode& node,
    const domain::NumericProcessingGraph& graph,const domain::NumericProcessingGraph& runtime_graph,
    bool runtime_available,const QMap<QString,QString>& fixed_views,
    QSet<QString>& used,QMap<QString,NumericBinding>& bindings) {
    const QString kind = QString::fromStdString(node.kind);
    domain::ResolvedProcessingNode resolved;
    if (!domain::resolve_processing_node(node,numeric_editor::limits(),resolved)) return;
    const auto node_kind = domain::node_kind(node.kind);
    if (resolved.role != domain::NodeRole::Sink || !node_kind ||
        !domain::is_view(*node_kind) || node.inputs.empty()) return;
    QString key = fixed_views.value(kind);
    if (key.isEmpty() || used.contains(key))
        key = ensure_node_view(node.id,kind,_application.device_id(),
                               QString::fromStdString(graph.graph_id),false,0,&node);
    used.insert(key);
    _active_node_views[node.id] = key;
    auto *dock = _node_views.contains(key) ? _node_views[key].dock : _panels.value(key);
    if (dock) dock->setProperty("graphNode",int(node.id));
    add_primary_binding(node,graph,runtime_graph,runtime_available,key,kind,bindings);
}
void MainWindow::add_primary_binding(const domain::ConfiguredProcessingNode& node,
    const domain::NumericProcessingGraph& graph,const domain::NumericProcessingGraph& runtime_graph,
    bool runtime_available,const QString& key,const QString& kind,QMap<QString,NumericBinding>& bindings) {
    const auto prefs = _application.state().file_configs.ui.graph_views.value(key).toObject();
    NumericBinding binding{{graph.graph_id,node.id,domain::PortDirection::Input,node.inputs.front().id},
        std::size_t(std::max(0,prefs["plane"].toInt())),_application.device_id()};
    if (!scalar_view_kind(node.kind) && admits_view_binding(binding,node.inputs.front(),runtime_graph,runtime_available))
        bindings[key] = binding;
}

void MainWindow::refresh_additional_node_views(const domain::NumericProcessingGraph& graph) {
    // Renderer installation can update the live map; retain the original key order.
    const auto views = _node_views;
    for (auto it = views.cbegin(); it != views.cend(); ++it) {
        const auto view = _node_views[it.key()];
        if (view.device != _application.device_id() ||
            view.graph != QString::fromStdString(graph.graph_id)) continue;
        const auto node = std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& candidate) {
            return candidate.id == view.node;
        });
        if (node == graph.nodes.end()) continue;
        ensure_node_view(view.node,QString::fromStdString(node->kind),view.device,view.graph,false,
                         view.dock->property("viewInstance").toInt(),&*node);
    }
}

void MainWindow::bind_additional_node_views(const domain::NumericProcessingGraph& graph,
    const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
    QMap<QString,NumericBinding>& bindings) {
    for (auto it = _node_views.begin(); it != _node_views.end(); ++it)
        bind_additional_node_view(it.key(),it.value(),graph,runtime_graph,runtime_available,bindings);
}

void MainWindow::bind_additional_node_view(const QString& key,const NodeView& view,
    const domain::NumericProcessingGraph& graph,const domain::NumericProcessingGraph& runtime_graph,
    bool runtime_available,QMap<QString,NumericBinding>& bindings) {
    if (view.device != _application.device_id() ||
        view.graph != QString::fromStdString(graph.graph_id)) return;
    const auto node = std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& candidate) {
        return candidate.id == view.node;
    });
    if (node == graph.nodes.end() || !numeric_editor::is_graph_view(QString::fromStdString(node->kind))) {
        view.dock->toggleView(false);
        return;
    }
    if (bindings.contains(key) || scalar_view_kind(node->kind)) return;
    add_additional_binding(key,view,*node,graph,runtime_graph,runtime_available,bindings);
}
void MainWindow::add_additional_binding(const QString& key,const NodeView& view,
    const domain::ConfiguredProcessingNode& node,const domain::NumericProcessingGraph& graph,
    const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
    QMap<QString,NumericBinding>& bindings) {
    const auto binding = additional_view_binding(key,view,node,graph);
    if (!binding) return;
    const auto& ports = binding->endpoint.direction == domain::PortDirection::Input ?
        node.inputs : node.outputs;
    const auto selected = std::find_if(ports.begin(),ports.end(),[&](const auto& port) {
        return port.id == binding->endpoint.port;
    });
    if (admits_view_binding(*binding,*selected,runtime_graph,runtime_available))
        bindings[key] = *binding;
    if (!_active_node_views.contains(node.id)) _active_node_views[node.id] = key;
}

std::optional<MainWindow::NumericBinding> MainWindow::additional_view_binding(const QString& key,
    const NodeView& view,const domain::ConfiguredProcessingNode& node,
    const domain::NumericProcessingGraph& graph) const {
    domain::ResolvedProcessingNode resolved;
    if (!domain::resolve_processing_node(node,numeric_editor::limits(),resolved)) return std::nullopt;
    const auto direction = resolved.role == domain::NodeRole::Sink ?
        domain::PortDirection::Input : domain::PortDirection::Output;
    const auto& ports = direction == domain::PortDirection::Input ? node.inputs : node.outputs;
    if (ports.empty()) return std::nullopt;
    const auto prefs = _application.state().file_configs.ui.graph_views.value(key).toObject();
    const auto port_id = std::uint16_t(prefs.value("port").toInt(ports.front().id));
    const auto selected = std::find_if(ports.begin(),ports.end(),[&](const auto& port) {
        return port.id == port_id;
    });
    if (selected == ports.end()) return std::nullopt;
    NumericBinding binding{{graph.graph_id,node.id,direction,port_id},
        std::size_t(std::max(0,prefs["plane"].toInt())),view.device};
    return binding;
}

void MainWindow::hide_unused_fixed_views(const QSet<QString>& used) {
    for (auto it = _panels.begin(); it != _panels.end(); ++it) {
        if (it.key() == "dataFlow" || used.contains(it.key())) continue;
        it.value()->toggleView(false);
        it.value()->setProperty("graphNode",QVariant{});
    }
}

QSet<QString> MainWindow::numeric_view_targets() const {
    QSet<QString> targets;
    for (auto it = _node_views.cbegin(); it != _node_views.cend(); ++it) targets.insert(it.key());
    for (auto it = _panels.cbegin(); it != _panels.cend(); ++it)
        if (it.key() != "dataFlow") targets.insert(it.key());
    return targets;
}

void MainWindow::configure_metric_view(const QString& key,const domain::NumericProcessingGraph& graph,
    const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
    QMap<QString,QList<NumericBinding>>& metric_bindings) {
    auto *dock = _node_views.contains(key) ? _node_views[key].dock : _panels.value(key);
    auto *panel = dock ? qobject_cast<GraphPanel *>(dock->widget()) : nullptr;
    if (!panel || !panel->metrics_view()) return;
    const auto node = std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& candidate) {
        return candidate.id == dock->property("graphNode").toUInt();
    });
    if (node != graph.nodes.end() && scalar_view_kind(node->kind)) {
        configure_scalar_metric_view(panel,key,*node,graph,runtime_graph,runtime_available,metric_bindings);
        return;
    }
    panel->metrics_view()->set_inputs({});
}

void MainWindow::configure_scalar_metric_view(GraphPanel *panel,const QString& key,
    const domain::ConfiguredProcessingNode& node,const domain::NumericProcessingGraph& graph,
    const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
    QMap<QString,QList<NumericBinding>>& metric_bindings) {
    QList<NumericMetricsView::Input> inputs;
    const auto presentation = _application.state().data_flow_graph ?
        _application.state().data_flow_graph->delegateModel<AudioNodeModel>(node.id) : nullptr;
    const auto options = presentation ? presentation->presentation().value("scalar_inputs").toObject() : QJsonObject{};
    for (const auto& port : node.inputs) {
        const auto configured = options.value(QString::number(port.id)).toObject();
        if (!configured.value("enabled").toBool(true)) continue;
        const auto name = configured.value("display_name").toString().trimmed();
        inputs.push_back({port.id,name});
        NumericBinding binding{{graph.graph_id,node.id,domain::PortDirection::Input,port.id},
            0,_application.device_id()};
        if (admits_view_binding(binding,port,runtime_graph,runtime_available))
            metric_bindings[key].push_back(binding);
    }
    panel->metrics_view()->set_inputs(inputs);
    mark_unavailable_metric_inputs(panel,key,inputs,metric_bindings);
}
void MainWindow::mark_unavailable_metric_inputs(GraphPanel *panel,const QString& key,
    const QList<NumericMetricsView::Input>& inputs,QMap<QString,QList<NumericBinding>>& metric_bindings) {
    for (const auto& input : inputs) {
        const auto& ports = metric_bindings[key];
        if (std::none_of(ports.cbegin(),ports.cend(),[&](const auto& binding) {
            return binding.endpoint.port == input.port;
        })) panel->metrics_view()->set_unavailable(input.port);
    }
}

bool MainWindow::has_metric_binding(const NumericBinding& candidate,
    const QMap<QString,QList<NumericBinding>>& bindings) const {
    for (const auto& ports : bindings)
        for (const auto& binding : ports)
            if (binding.endpoint == candidate.endpoint && binding.plane == candidate.plane) return true;
    return false;
}

void MainWindow::release_old_metric_bindings(const QMap<QString,QList<NumericBinding>>& metric_bindings,
    const QMap<QString,NumericBinding>& bindings) {
    for (const auto& ports : std::as_const(_metric_bindings)) {
        for (const auto& old : ports) {
            const bool in_views = std::any_of(bindings.cbegin(),bindings.cend(),[&](const auto& binding) {
                return binding.endpoint == old.endpoint && binding.plane == old.plane;
            });
            if (!has_metric_binding(old,metric_bindings) && !in_views)
                _numeric_views.unbind(old.endpoint,old.plane);
        }
    }
}

void MainWindow::publish_metric_bindings() {
    for (const auto& ports : std::as_const(_metric_bindings))
        for (const auto& binding : ports) _numeric_views.bind(binding.endpoint,binding.plane);
    for (auto it = _metric_bindings.cbegin(); it != _metric_bindings.cend(); ++it) {
        if (it.value().isEmpty()) continue;
        auto *dock = _node_views.contains(it.key()) ? _node_views[it.key()].dock : _panels.value(it.key());
        const auto& binding = it.value().front();
        if (dock) dock->setProperty("numericEndpoint",QVariantMap{
            {"graph",QString::fromStdString(binding.endpoint.graph_id)},{"device",binding.device},
            {"node",int(binding.endpoint.node)},{"direction","input"},
            {"port",int(binding.endpoint.port)},{"plane",qulonglong(binding.plane)}});
    }
}

void MainWindow::sync_metric_bindings(const domain::NumericProcessingGraph& graph,
    const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
    const QSet<QString>& targets,const QMap<QString,NumericBinding>& bindings) {
    QMap<QString,QList<NumericBinding>> metric_bindings;
    for (const auto& key : targets)
        configure_metric_view(key,graph,runtime_graph,runtime_available,metric_bindings);
    release_old_metric_bindings(metric_bindings,bindings);
    _metric_bindings = std::move(metric_bindings);
    publish_metric_bindings();
}

void MainWindow::clear_unbound_view_targets(const QSet<QString>& targets,
    const QMap<QString,NumericBinding>& bindings) {
    for (const auto& key : targets) {
        if (bindings.contains(key) || _metric_bindings.contains(key)) continue;
        clear_unbound_view_target(key);
    }
}
void MainWindow::clear_unbound_view_target(const QString& key) {
    auto *dock = _node_views.contains(key) ? _node_views[key].dock : _panels.value(key);
    auto *panel = dock ? qobject_cast<GraphPanel *>(dock->widget()) : nullptr;
    if (!panel) return;
    auto target = panel->all_render_targets();
    presentation::NumericViewAdapter::clear_render_target(target);
    if (!panel->active_spectrogram() || !panel->active_spectrogram()->paused())
        target.exact_label->caption("Renderer projection unavailable",{},false,false);
    if (target.spectrogram) target.spectrogram->caption("Renderer projection unavailable",{},false,false);
    if (target.image) target.image->caption("Renderer projection unavailable",{},false,false);
    if (target.pie) target.pie->caption("Renderer projection unavailable",{},false,false);
}

namespace {
void apply_frequency_parameter(presentation::NumericRenderTarget::FrequencyGrid& grid,
    const domain::ResolvedProcessingNode& resolved,const char *key) {
    domain::ProcessingParameterView value;
    if (domain::resolve_processing_parameter(resolved,key,value) != signal_contracts::Status::ok) return;
    const auto *number = std::get_if<signal_contracts::NumericLiteral>(&value);
    if (!number) return;
    const auto numeric = std::visit([](auto item) { return double(item); },*number);
    if (std::string_view(key) == "bin_hz") grid.step = numeric;
    else grid.first_bin = std::uint64_t(numeric);
}
void resolve_frequency_grid(presentation::NumericRenderTarget::FrequencyGrid& grid,
    const domain::ConfiguredProcessingNode& node,const domain::NumericProcessingGraph& runtime_graph,
    bool applied,bool audio) {
    const auto active = std::find_if(runtime_graph.nodes.begin(),runtime_graph.nodes.end(),
        [&](const auto& candidate) { return candidate.id == node.id; });
    const bool connected = std::any_of(runtime_graph.edges.begin(),runtime_graph.edges.end(),
        [&](const auto& edge) { return edge.to == node.id && edge.input_port == domain::frequency_step_input; });
    if (applied && active != runtime_graph.nodes.end() && (connected || audio)) {
        domain::ResolvedProcessingNode resolved;
        if (domain::resolve_processing_node(*active,numeric_editor::limits(),resolved)) {
            apply_frequency_parameter(grid,resolved,"bin_hz");
            apply_frequency_parameter(grid,resolved,"first_bin");
        }
    }
}

}

void MainWindow::configure_frequency_grid(GraphPanel *panel,const domain::ConfiguredProcessingNode& node,
    const domain::NumericProcessingGraph& runtime_graph,bool applied) {
    presentation::NumericRenderTarget::FrequencyGrid grid;
    const bool legacy=node.kind=="FFTView";
    const bool audio=node.kind=="AudioLineView" || node.kind=="AudioPointView" ||
        node.kind=="AudioBarView" || node.kind=="AudioPieView";
    if (!legacy && !audio) {
        panel->set_frequency_grid(grid);
        return;
    }
    grid.explicit_input=legacy;
    resolve_frequency_grid(grid,node,runtime_graph,applied,audio);
    if (grid.step>0)grid.explicit_input=true;
    if (grid.explicit_input){grid.index_title="Frequency bin (X)";grid.coordinate_title="Frequency, Hz (X)";}
    panel->set_frequency_grid(grid);
}

void MainWindow::configure_numeric_panel(GraphPanel *panel,const QString& key,const NumericBinding& binding,
    const domain::ConfiguredProcessingNode& node) {
    if (node.kind == "PianoView") configure_piano_first_midi(panel,&node);
    const auto& ports = binding.endpoint.direction == domain::PortDirection::Input ? node.inputs : node.outputs;
    const auto port = std::find_if(ports.begin(),ports.end(),[&](const auto& candidate) {
        return candidate.id == binding.endpoint.port;
    });
    if (port == ports.end()) return;
    const auto data = port->contract.view();
    const auto *numeric = std::get_if<signal_contracts::NumericContract>(&data);
    panel->set_numeric_plane_count(numeric ? numeric->channel_order.size() : 0,binding.plane);
    panel->set_value_layout("");
    const auto preferences = _application.state().file_configs.ui.graph_views.value(key).toObject();
    const auto fallback = numeric_editor::plot_descriptor_for_kind(QString::fromStdString(node.kind));
    const auto descriptor = presentation::plot_descriptor_from_preferences(preferences,fallback);
    const auto capabilities = presentation::plot_capabilities(descriptor);
    panel->set_plot_presentation(numeric && capabilities.audio_scales ?
        presentation::audio_plot_presentation(*numeric) : presentation::PlotPresentation{});
    const auto section_text = presentation::explicit_plot_sections(preferences.value("plot_sections").toArray());
    panel->set_value_layout("%1",{section_text});
}

void MainWindow::configure_numeric_binding(const QString& key,const NumericBinding& binding,
    const domain::NumericProcessingGraph& graph,const domain::NumericProcessingGraph& runtime_graph,
    bool runtime_available) {
    _numeric_views.bind(binding.endpoint,binding.plane);
    auto *dock = _node_views.contains(key) ? _node_views[key].dock : _panels.value(key);
    if (dock) dock->setProperty("numericEndpoint",QVariantMap{
        {"graph",QString::fromStdString(binding.endpoint.graph_id)},{"device",binding.device},
        {"node",int(binding.endpoint.node)},
        {"direction",binding.endpoint.direction == domain::PortDirection::Input ? "input" : "output"},
        {"port",int(binding.endpoint.port)},{"plane",qulonglong(binding.plane)}});
    auto *panel = dock ? qobject_cast<GraphPanel *>(dock->widget()) : nullptr;
    if (!panel) return;
    const bool applied = runtime_available && runtime_graph.graph_id == binding.endpoint.graph_id;
    panel->set_axis_permissions({applied,applied});
    const auto node = std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& candidate) {
        return candidate.id == binding.endpoint.node;
    });
    if (node == graph.nodes.end()) return;
    configure_frequency_grid(panel,*node,runtime_graph,applied);
    configure_numeric_panel(panel,key,binding,*node);
}

void MainWindow::sync_numeric_bindings(const domain::NumericProcessingGraph& graph,
    const domain::NumericProcessingGraph& runtime_graph,bool runtime_available,
    QMap<QString,NumericBinding> bindings) {
    for (const auto& old : std::as_const(_numeric_bindings)) {
        const bool retained = std::any_of(bindings.cbegin(),bindings.cend(),[&](const auto& binding) {
            return binding.endpoint == old.endpoint && binding.plane == old.plane;
        });
        if (!has_metric_binding(old,_metric_bindings) && !retained)
            _numeric_views.unbind(old.endpoint,old.plane);
    }
    _numeric_bindings = std::move(bindings);
    for (auto it = _numeric_bindings.cbegin(); it != _numeric_bindings.cend(); ++it)
        configure_numeric_binding(it.key(),it.value(),graph,runtime_graph,runtime_available);
}

void MainWindow::clear_node_views() {
    _numeric_changed_endpoints.clear();
    _vector_delivery_queues.clear();
    _numeric_views.clear();
    for(const auto docks=graph_docks(); auto* dock:docks)if(auto* panel=qobject_cast<GraphPanel*>(dock->widget())){
        panel->set_axis_permissions({});
        panel->set_value_layout("");
        presentation::NumericViewAdapter::clear_render_target(panel->all_render_targets());
    }
}
void MainWindow::refresh_vector_delivery_queues() {
    const auto selected=selected_vector_endpoints();
    // The preview arena reserves three leases per endpoint and eight scratch slots.
    const auto capacity = selected.empty() || selected.size() > 40 ? std::size_t{0} :
        std::min(std::size_t{3},(120 - 3 * selected.size()) / selected.size());
    std::vector<VectorDeliveryQueue> prepared;
    if (capacity >= 2 && !_restoring_preset)
        for (auto it = _numeric_bindings.cbegin(); it != _numeric_bindings.cend(); ++it)
            append_vector_delivery_queue(prepared,it.key(),it.value(),capacity);
    _vector_delivery_queues = std::move(prepared);
    _vector_delivery_device = _application.device_id();
    for (const auto& queue : _vector_delivery_queues)
        if (queue.count) _numeric_render_dirty = true;
}
std::vector<pipeline::EndpointIdentity> MainWindow::selected_vector_endpoints() const {
    std::vector<pipeline::EndpointIdentity> selected;
    const auto collect = [&](const NumericBinding& binding) {
        if (binding.device == _application.device_id() &&
            std::find(selected.begin(),selected.end(),binding.endpoint) == selected.end())
            selected.push_back(binding.endpoint);
    };
    for (const auto& binding : _numeric_bindings) collect(binding);
    for (const auto& ports : _metric_bindings)
        for (const auto& binding : ports) collect(binding);
    return selected;
}
void MainWindow::consume_numeric_frame(const pipeline::DeliveredNumericFrame& frame) {
    core::perf::Scope profile(core::perf::Stage::NumericFrameReceive);
    if (_restoring_preset)
        for (auto& queue : _vector_delivery_queues) queue.clear();
    if (!_restoring_preset && frame.context && _vector_delivery_device == _application.device_id()) {
        auto found = std::find_if(_vector_delivery_queues.begin(),_vector_delivery_queues.end(),
            [&](const auto& queue) { return queue.endpoint == frame.context->endpoint; });
        if (found != _vector_delivery_queues.end()) {
            queue_numeric_frame(*found,frame);
            return;
        }
    }
    _numeric_views.accept(frame);
    if (frame.context) request_numeric_render(frame.context->endpoint);
}
void MainWindow::append_vector_delivery_queue(std::vector<VectorDeliveryQueue>& prepared,
    const QString& key,const NumericBinding& binding,std::size_t capacity) {
    if (binding.device != _application.device_id()) return;
    auto *dock = _node_views.contains(key) ? _node_views[key].dock : _panels.value(key);
    auto *panel = dock ? qobject_cast<GraphPanel *>(dock->widget()) : nullptr;
    if (!panel || !panel->active_chart() || dock->isHidden() ||
        !dock->property("renderExposed").toBool()) return;
    if (std::any_of(prepared.begin(),prepared.end(),[&](const auto& queue) {
        return queue.endpoint == binding.endpoint;
    })) return;
    restore_vector_queue(prepared,binding,capacity);
}
void MainWindow::restore_vector_queue(std::vector<VectorDeliveryQueue>& prepared,
    const NumericBinding& binding,std::size_t capacity) {
    auto prior = std::find_if(_vector_delivery_queues.begin(),_vector_delivery_queues.end(),
        [&](const auto& queue) { return queue.endpoint == binding.endpoint; });
    if (_vector_delivery_device == _application.device_id() &&
        prior != _vector_delivery_queues.end() && prior->capacity == capacity) {
        prepared.push_back(std::move(*prior));
        return;
    }
    VectorDeliveryQueue queue;
    queue.endpoint = binding.endpoint;
    queue.capacity = capacity;
    prepared.push_back(std::move(queue));
}

void MainWindow::enqueue_vector_frame(VectorDeliveryQueue& queue,const pipeline::DeliveredNumericFrame& frame) {
    const auto& stamp = frame.frame.view().stamp;
    const bool reset = !queue.seen || frame.delivery != queue.fence || queue.context != frame.context ||
        stamp.producer_epoch != queue.producer || stamp.generation != queue.generation || stamp.discontinuity;
    if (!reset && stamp.sequence <= queue.sequence) return;
    if (reset) queue.clear();
    update_vector_queue_stamp(queue,frame);
    if (queue.count == queue.capacity) {
        queue.frames[queue.head] = {};
        queue.head = (queue.head + 1) % queue.capacity;
        --queue.count;
    }
    queue.frames[(queue.head + queue.count) % queue.capacity] = frame;
    ++queue.count;
    request_numeric_render(frame.context->endpoint);
}
void MainWindow::update_vector_queue_stamp(VectorDeliveryQueue& queue,const pipeline::DeliveredNumericFrame& frame) {
    const auto& stamp=frame.frame.view().stamp;
    queue.seen = true;
    queue.fence = frame.delivery;
    queue.context = frame.context;
    queue.producer = stamp.producer_epoch;
    queue.generation = stamp.generation;
    queue.sequence = stamp.sequence;
}

void MainWindow::queue_numeric_frame(VectorDeliveryQueue& queue,const pipeline::DeliveredNumericFrame& frame) {
    if (queue.seen && pipeline::delivery_precedes(frame.delivery,queue.fence)) return;
    bool vector_frame = false;
    if (frame.frame) {
        const auto view = frame.frame.view();
        const auto *numeric = std::get_if<signal_contracts::NumericView>(&view.payload);
        vector_frame = numeric && numeric->contract &&
            numeric->contract->shape.rank == signal_contracts::Rank::Vector;
    }
    const bool available = frame.status == signal_contracts::Status::ok && frame.frame &&
        frame.frame.view().stamp.available && vector_frame;
    if (!available) {
        queue_unavailable_frame(queue,frame);
        return;
    }
    enqueue_vector_frame(queue,frame);
}
void MainWindow::queue_unavailable_frame(VectorDeliveryQueue& queue,const pipeline::DeliveredNumericFrame& frame) {
    queue.clear();
    queue.seen = true;
    queue.fence = frame.delivery;
    queue.context = frame.context;
    queue.sequence = 0;
    _numeric_views.accept(frame);
    request_numeric_render(frame.context->endpoint);
}

void MainWindow::request_numeric_render(const pipeline::EndpointIdentity& endpoint) {
    if(std::find(_numeric_changed_endpoints.begin(),_numeric_changed_endpoints.end(),endpoint)==_numeric_changed_endpoints.end())
        _numeric_changed_endpoints.push_back(endpoint);
    if(_numeric_render_queued)return;
    _numeric_render_queued=true;
    // Collapse this delivery burst, without waiting for another polling tick.
    // Individual views retain their configured publication cadence.
    QMetaObject::invokeMethod(this,[this]{
        _numeric_render_queued=false;
        if(_restoring_preset)return;
        const bool all=_numeric_render_dirty;
        _numeric_render_dirty=false;render_numeric_views(true,!all);
    },Qt::QueuedConnection);
}
void MainWindow::consume_numeric_history(const pipeline::DeliveredNumericHistoryRow& row) {
    core::perf::Scope profile(core::perf::Stage::NumericHistoryReceive);
    // A compact transport event invalidates the displayed snapshot. History
    // belongs to its processing node; the graph never appends rows itself.
    if(_numeric_snapshot_dirty)return;
    _numeric_snapshot_dirty=true;
    // The transport already paces notifications. Request the owning snapshot
    // after this delivery burst instead of waiting an entire GUI polling tick.
    QMetaObject::invokeMethod(this,[this]{
        if(!_numeric_snapshot_dirty)return;
        _numeric_snapshot_dirty=false;
        _application.client().request_numeric_snapshots();
    },Qt::QueuedConnection);
}
void MainWindow::consume_numeric_history_snapshot(const pipeline::DeliveredNumericFrame& frame,quint64 ordinal,quint64 filled_count) {
    core::perf::Scope profile(core::perf::Stage::NumericSnapshotReceive);
    bool vector_history=false;
    if(frame.context && frame.context->contract) {
        const auto contract=frame.context->contract.view();
        const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract);
        vector_history=numeric && numeric->semantic==signal_contracts::Semantic::History &&
            numeric->shape.rank==signal_contracts::Rank::Vector;
    }
    if(vector_history)_numeric_views.accept(frame);
    else _numeric_views.accept_history_snapshot(frame,ordinal,filled_count);
    // A completed snapshot is ready for projection now. The renderer owns its
    // cadence; a second GUI timer wait only adds latency to this response.
    if(frame.context)request_numeric_render(frame.context->endpoint);
}
void MainWindow::render_numeric_views(bool consumeQueuedFrames,bool changedOnly) {
    core::perf::Scope profile(core::perf::Stage::NumericRenderViews);
    if (_restoring_preset) return;
    auto changed = std::move(_numeric_changed_endpoints);
    _numeric_changed_endpoints.clear();
    if (consumeQueuedFrames) consume_queued_vector_frames(changed);
    prepare_numeric_render();
    render_bound_views(changed,changedOnly);
    render_metric_views(changed,changedOnly);
}
void MainWindow::prepare_numeric_render() {
    for (const auto& queue : _vector_delivery_queues)
        if (queue.count) _numeric_render_dirty = true;
    if (_numeric_snapshot_dirty) {
        _numeric_snapshot_dirty = false;
        _application.client().request_numeric_snapshots();
    }
}
void MainWindow::consume_queued_vector_frames(std::vector<pipeline::EndpointIdentity>& changed) {
    for (auto& queue : _vector_delivery_queues) {
        if (!queue.count) continue;
        if (std::find(changed.begin(),changed.end(),queue.endpoint) == changed.end())
            changed.push_back(queue.endpoint);
        consume_vector_delivery_queue(queue);
    }
}
void MainWindow::consume_vector_delivery_queue(VectorDeliveryQueue& queue) {
    // Consume the newest owning preview to avoid GUI-rate FIFO latency.
    const auto newest = (queue.head + queue.count - 1) % queue.capacity;
    auto frame = std::move(queue.frames[newest]);
    while (queue.count) {
        queue.frames[queue.head] = {};
        queue.head = (queue.head + 1) % queue.capacity;
        --queue.count;
    }
    _numeric_views.accept(frame);
}

void MainWindow::render_bound_views(const std::vector<pipeline::EndpointIdentity>& changed,bool changed_only) {
    for (auto it = _numeric_bindings.cbegin(); it != _numeric_bindings.cend(); ++it) {
        if (it->device != _application.device_id()) continue;
        if (changed_only && std::find(changed.begin(),changed.end(),it->endpoint) == changed.end()) continue;
        auto *dock = _node_views.contains(it.key()) ? _node_views[it.key()].dock : _panels.value(it.key());
        auto *panel = dock ? qobject_cast<GraphPanel *>(dock->widget()) : nullptr;
        if (!panel) continue;
        auto target = panel->render_target(!dock->isHidden() && dock->property("renderExposed").toBool());
        _numeric_views.render(it->endpoint,it->plane,target);
    }
}

void MainWindow::render_metric_views(const std::vector<pipeline::EndpointIdentity>& changed,bool changed_only) {
    for (auto it = _metric_bindings.cbegin(); it != _metric_bindings.cend(); ++it)
        render_metric_view(it.key(),it.value(),changed,changed_only);
}
void MainWindow::render_metric_view(const QString& key,const QList<NumericBinding>& bindings,
    const std::vector<pipeline::EndpointIdentity>& changed,bool changed_only) {
        auto *dock = _node_views.contains(key) ? _node_views[key].dock : _panels.value(key);
        auto *panel = dock ? qobject_cast<GraphPanel *>(dock->widget()) : nullptr;
        if (!panel || !panel->metrics_view()) return;
        auto target = panel->render_target(!dock->isHidden() && dock->property("renderExposed").toBool());
        for (const auto& binding : bindings) {
            if (binding.device != _application.device_id()) continue;
            if (changed_only && std::find(changed.begin(),changed.end(),binding.endpoint) == changed.end()) continue;
            _numeric_views.render(binding.endpoint,binding.plane,target);
        }
}

void MainWindow::select_numeric_plane(const QString& key,std::size_t plane) {
    auto found=_numeric_bindings.find(key);if(found==_numeric_bindings.end() || found->plane==plane)return;
    const auto prior=*found;found->plane=plane;
    if(std::none_of(_numeric_bindings.cbegin(),_numeric_bindings.cend(),[&](const auto& binding) {
        return binding.endpoint==prior.endpoint && binding.plane==prior.plane;
    }))_numeric_views.unbind(prior.endpoint,prior.plane);
    _numeric_views.bind(found->endpoint,plane);
    auto preferences=_application.state().file_configs.ui.graph_views[key].toObject();
    preferences["plane"]=int(plane);_application.state().file_configs.ui.graph_views[key]=preferences;
    render_numeric_views();_workspace_save_timer.start();
}
void MainWindow::set_display_refresh_rate_provider(std::function<double(QScreen*)> provider) {
    _display_refresh_rate_provider=std::move(provider);
    apply_render_rate();
}
void MainWindow::refresh_display_cadence() { apply_render_rate(); }
void MainWindow::apply_render_rate() {
    auto *settings = findChild<SettingsWidget *>();
    if (settings) settings->set_display_screen(screen());
    const auto prefs = _application.state().file_configs.ui.graph_rendering;
    QHash<QScreen *,double> refresh_rates;
    const auto refresh_rate = [this,settings,&refresh_rates](QScreen *display) {
        return display_refresh_rate(display,settings,refresh_rates);
    };
    _render_fps = SurfaceSettings::renderFps(prefs,refresh_rate(screen()));
    for (const auto docks = graph_docks(); auto *dock : docks)
        if (dock->isVisible())
            _render_fps = std::max(_render_fps,SurfaceSettings::renderFps(prefs,refresh_rate(dock->screen())));
    // Preview delivery has jitter headroom; processing clock is independent.
    _application.set_numeric_preview_cadence_hz(std::min(240,_render_fps * 2));
    _numeric_render_timer.setInterval(qMax(1,1000 / qMax(1,_render_fps)));
    update_render_precision(prefs,refresh_rates);
}
void MainWindow::update_render_precision(const QJsonObject& prefs,QHash<QScreen*,double>& refresh_rates) {
    const int precision = SurfaceSettings::effective(prefs).value("axis_precision").toInt();
    update_chart_render_rates(precision);
    update_graph_panel_render_rates(prefs,refresh_rates,precision);
}
void MainWindow::update_graph_panel_render_rates(const QJsonObject& prefs,
    QHash<QScreen*,double>& refresh_rates,int precision) {
    auto *settings = findChild<SettingsWidget *>();
    const auto refresh_rate = [this,settings,&refresh_rates](QScreen *display) {
        return display_refresh_rate(display,settings,refresh_rates);
    };
    for (const auto docks = graph_docks(); auto *dock : docks) {
        auto *panel = qobject_cast<GraphPanel *>(dock->widget());
        if (!panel) continue;
        panel->set_render_fps(SurfaceSettings::renderFps(prefs,refresh_rate(panel->screen())));
        panel->set_axis_precision(precision);
    }
}

double MainWindow::display_refresh_rate(QScreen *display,SettingsWidget *settings,
    QHash<QScreen*,double>& cache) const {
    const auto found = cache.constFind(display);
    if (found != cache.cend()) return found.value();
    double rate = 60.0;
    if (_display_refresh_rate_provider) rate = _display_refresh_rate_provider(display);
    else if (settings && display == screen()) rate = settings->display_refresh_rate();
    else if (display) rate = display->refreshRate();
    cache.insert(display,rate);
    return rate;
}

void MainWindow::update_chart_render_rates(int precision) {
    for (auto *chart : {_raw_signal_chart,_window_signal_chart,_windowed_signal_chart,
                        _window_function_chart,_amplitudes_chart,_test_amplitudes_chart}) {
        if (!chart) continue;
        chart->set_render_fps(_render_fps);
        chart->set_axis_precision(precision);
    }
    if (_spectrogram_view) _spectrogram_view->set_render_fps(_render_fps);
    update_node_chart_render_rates(precision);
}
void MainWindow::update_node_chart_render_rates(int precision) {
    for (auto& view : _node_views) {
        if (view.chart) {
            view.chart->set_render_fps(_render_fps);
            view.chart->set_axis_precision(precision);
        }
        if (view.spectrogram) view.spectrogram->set_render_fps(_render_fps);
    }
}

void MainWindow::apply_surface_settings() {
    apply_render_rate();
    const auto& ui = _application.state().file_configs.ui;
    if (_spectrogram_view)
        _spectrogram_view->set_surface_settings(
            SurfaceSettings::effective(ui.graph_rendering,spectrogram_preferences()));
    for (auto it = _node_views.begin(); it != _node_views.end(); ++it)
        if (it->spectrogram)
            it->spectrogram->set_surface_settings(
                SurfaceSettings::effective(ui.graph_rendering,ui.graph_views.value(it.key()).toObject()));
    update_graph_surface_settings();
}
void MainWindow::update_graph_surface_settings() {
    const auto& ui = _application.state().file_configs.ui;
    for (const auto docks = graph_docks(); auto *dock : docks) {
        auto *panel = qobject_cast<GraphPanel *>(dock->widget());
        auto *surface = panel ? panel->active_spectrogram() : nullptr;
        if (!surface || surface == _spectrogram_view) continue;
        surface->set_surface_settings(SurfaceSettings::effective(
            ui.graph_rendering,ui.graph_views.value(dock->property("graphViewKey").toString()).toObject()));
    }
}

QJsonObject MainWindow::spectrogram_preferences() const {
    const auto& ui = _application.state().file_configs.ui;
    auto preferences = ui.graph_views.value("spectrogram").toObject();
    for (auto it = _active_node_views.begin(); it != _active_node_views.end(); ++it) {
        if (it.value() != "spectrogram") continue;
        const auto identity = QString::fromLatin1(
            QJsonDocument(QJsonArray{_application.device_id(),_application.graph_id(),it.key()})
                .toJson(QJsonDocument::Compact).toHex());
        if (ui.graph_views.contains(identity)) preferences = ui.graph_views.value(identity).toObject();
        break;
    }
    return preferences;
}

QString MainWindow::surface_identity(const QString& key) const {
    if (key != "spectrogram") return key;
    for (auto it = _active_node_views.begin(); it != _active_node_views.end(); ++it) {
        if (it.value() != key) continue;
        return QString::fromLatin1(
            QJsonDocument(QJsonArray{_application.device_id(),_application.graph_id(),it.key()})
                .toJson(QJsonDocument::Compact).toHex());
    }
    return key;
}

void MainWindow::connect_surface_status(SpectrogramGraph *view, const QString &key) {
    connect(view,&SpectrogramGraph::surface_settings_changed,this,
            [this,key](const QJsonObject& options) { save_surface_override(key,options); });
    connect(view,&SpectrogramGraph::edit_color_scale_requested,this,[this,key] {
        if (auto *settings = findChild<SettingsWidget *>())
            settings->edit_color_scale(surface_identity(key));
    });
    connect(view,&SpectrogramGraph::surface_settings_error,this,
            [this,key,view](const QString& error) { show_surface_error(key,view,error); });
}

namespace {
void update_aspect_override(QJsonObject& values,const QJsonObject& normalized,
    const QJsonObject& previous,const QJsonObject& global) {
    const bool edited = normalized["scene_aspect_mode"] != previous["scene_aspect_mode"] ||
        normalized["scene_time_aspect"] != previous["scene_time_aspect"] ||
        normalized["scene_band_aspect"] != previous["scene_band_aspect"];
    if (!edited) return;
    const bool custom = normalized["scene_aspect_mode"] != global["scene_aspect_mode"] ||
        normalized["scene_time_aspect"] != global["scene_time_aspect"] ||
        normalized["scene_band_aspect"] != global["scene_band_aspect"];
    // Mode and both ratios are one policy, also after future global changes.
    for (const auto *field : {"scene_aspect_mode","scene_time_aspect","scene_band_aspect"}) {
        if (custom) values[field] = normalized[field];
        else values.remove(field);
    }
}
}

void MainWindow::save_surface_override(const QString& key,const QJsonObject& options) {
    auto& ui = _application.state().file_configs.ui;
    const auto id = surface_identity(key);
    auto preferences = ui.graph_views.value(id).toObject();
    auto override = preferences.value("surface_override").toObject();
    auto values = override.value("values").toObject();
    const auto normalized = SurfaceSettings::normalized(options);
    const auto previous = SurfaceSettings::effective(ui.graph_rendering,preferences);
    for (const auto *field : {"y_range_mode","y_min","y_max","auto_range_interval_seconds",
                              "reverse_time","reverse_amplitude","reverse_bands",
                              "gradient","gradient_layout","gradient_stops"})
        values[field] = normalized[field];
    update_aspect_override(values,normalized,previous,SurfaceSettings::effective(ui.graph_rendering));
    override["enabled"] = true;
    override["values"] = values;
    preferences["surface_override"] = override;
    ui.graph_views[id] = preferences;
    _camera_save_timer.start();
}

void MainWindow::show_surface_error(const QString& key,SpectrogramGraph *view,const QString& error) {
    if (error.isEmpty()) _surface_errors.remove(key);
    else _surface_errors[key] =
        (view->parentWidget() ? view->parentWidget()->windowTitle() : ui::text("Spectrogram")) + ": " + error;
    auto *label = findChild<QLabel *>("surfaceSettingsStatus");
    if (!label) {
        label = new QLabel(this);
        label->setObjectName("surfaceSettingsStatus");
        statusBar()->addPermanentWidget(label);
    }
    label->setText(joined_surface_errors(_surface_errors));
    label->setToolTip(label->text());
    label->setVisible(!_surface_errors.isEmpty());
}
