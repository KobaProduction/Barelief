#include "core_app/main_window.h"
#include <core/performance.h>
#include "core_app/graph_panel.h"
#include "core_app/settings_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "visualization/surface_settings.h"
#include "visualization/ui_text.h"
#include "visualization/charts/line.h"
#include "visualization/charts/freq.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QScreen>
#include <QStatusBar>
#include <algorithm>
#include <desktop/numeric_project_document.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <QLabel>
#include <QLayout>
#include <visualization/numeric_metrics_view.h>

Q_DECLARE_LOGGING_CATEGORY(core_app_window)

QString MainWindow::ensure_node_view(int node, const QString &kind, const QString &device, const QString &graph,
                                     bool open, int instance, const domain::ConfiguredProcessingNode *configured) {
    if(!numeric_editor::is_graph_view(kind))return {};
    QJsonArray identity{device, graph, node};
    if(instance) identity.append(instance);
    const auto key = QString::fromLatin1(QJsonDocument(identity).toJson(QJsonDocument::Compact).toHex());
    const bool existing=_node_views.contains(key);
    NodeView view;
    view.node = node;
    view.device = device;
    view.graph = graph;
    view.kind = kind;
    auto *dock = existing?_node_views[key].dock:new WorkspaceDock(AudioNodeModel(kind).caption()+" #"+QString::number(node), this);
    dock->setProperty("graphKind",kind);
    dock->setProperty("graphViewKey",key);
    dock->setObjectName("nodeView_" + key);
    view.dock = dock;
    dock->setProperty("graphNode",node);
    dock->setProperty("viewInstance",instance);
    if(!existing)setup_graph_dock_controls(dock);
    refresh_graph_title(dock);
    auto preferences = _application.state().file_configs.ui.graph_views.value(key).toObject();
    preferences["kind"] = kind;
    preferences["instance"] = instance;
    // Renderer choice is a view preference, never a numeric semantic type.
    if(kind=="SpectrogramView" && !preferences.contains("display_mode"))preferences["display_mode"]="surface3d";
    if(existing) {
        const auto& previous=_node_views[key];
        if(previous.kind==kind)return key;
        // Keep the dock, ADS host and instance identity; replace only the
        // incompatible renderer. Other endpoint histories remain untouched.
        delete dock->takeWidget();_surface_errors.remove(key);
    }
    view.chart = kind == "FFTView" ? static_cast<AbstractChartView *>(new FreqChartView)
                                   : static_cast<AbstractChartView *>(new LineChartView);
    view.chart->setTitle({});
    view.chart->set_range(-1, 1);
    view.chart->set_auto_resizing(true);
    view.chart->set_auto_gain(true, 1, -1);
    auto *panel = new GraphPanel(key, view.chart, preferences, dock);
    panel->set_render_fps(_render_fps);
    connect(panel,&GraphPanel::renderer_changed,this,[this,panel,key]{
        if(auto* surface=panel->active_spectrogram()) {
            if(!surface->property("numericCompositionConnected").toBool()){
                surface->setProperty("numericCompositionConnected",true);
                connect_surface_status(surface,key);
            }
            surface->set_camera_controls(_application.state().file_configs.ui.camera_controls);
            surface->set_surface_settings(SurfaceSettings::effective(_application.state().file_configs.ui.graph_rendering,
                _application.state().file_configs.ui.graph_views.value(key).toObject()));
        }
        refresh_vector_delivery_queues();_numeric_render_dirty=true;render_numeric_views();
    });
    dock->setWidget(panel);
    connect(panel, &GraphPanel::channel_requested, this,
            [this,key](int plane) {select_numeric_plane(key,std::size_t(std::max(0,plane)));});
    connect(panel, &GraphPanel::presentation_changed, this, [this](const QString &id, const QJsonObject &updates) {
        auto p = _application.state().file_configs.ui.graph_views[id].toObject();
        for (auto it = updates.begin(); it != updates.end(); ++it)
            p[it.key()] = it.value();
        _application.state().file_configs.ui.graph_views[id] = p;
        // Pose already schedules its own surface render. It neither changes
        // supplied numeric data nor needs to reproject other graph endpoints.
        if(updates.size()!=1 || !updates.contains("camera"))_numeric_render_dirty=true;
        _workspace_save_timer.start();
    });
    if (view.chart) view.chart->set_render_fps(_render_fps);
    if (view.spectrogram) view.spectrogram->set_render_fps(_render_fps);
    install_graph_name_control(dock);
    _node_views[key] = view;
    if(auto* panel=qobject_cast<GraphPanel*>(dock->widget()))if(auto* surface=panel->active_spectrogram()){
        surface->setProperty("numericCompositionConnected",true);connect_surface_status(surface,key);
        surface->set_camera_controls(_application.state().file_configs.ui.camera_controls);
    }
    if(existing) {
        set_graph_exposed(key,dock,dock->property("renderExposed").toBool());
        return key;
    }
    set_graph_exposed(key, dock, false);
    connect(dock, &WorkspaceDock::visibilityChanged, this, [this, key, dock](bool visible) {
        set_graph_exposed(key, dock, visible);
        refresh_subscriptions();
    });
    addDockWidget(Qt::RightDockWidgetArea, dock);
    const auto selected_graph=_view_draft?QString::fromStdString(_view_draft->graph_id):_application.graph_id();
    dock->toggleView(device==_application.device_id() && graph==selected_graph && preferences.value("open").toBool(open));
    preferences["device"] = device;
    preferences["graph"] = graph;
    preferences["node"] = node;
    preferences["kind"] = kind;
    preferences["open"] = !dock->isClosed();
    _application.state().file_configs.ui.graph_views[key] = preferences;
    connect(dock, &WorkspaceDock::visibilityChanged, this, [this, key](bool visible) {
        if (_restoring_layout)
            return;
        auto &view = _node_views[key];
        auto p = _application.state().file_configs.ui.graph_views[key].toObject();
        p["open"] = !view.dock->isClosed();
        _application.state().file_configs.ui.graph_views[key] = p;
        refresh_subscriptions();
        _workspace_save_timer.start();
    });
    dock->installEventFilter(this);
    return key;
}
void MainWindow::restore_node_views() {
    const auto preferences = _application.state().file_configs.ui.graph_views;
    for (auto it = preferences.begin(); it != preferences.end(); ++it) {
        const auto p = it.value().toObject();
        if (!p.contains("legacy_panel") && p.contains("node") && p.contains("kind"))
        {
            const int instance=p.value("instance").toInt();
            _next_view_instance=std::max(_next_view_instance,instance+1);
            ensure_node_view(p["node"].toInt(), p["kind"].toString(), p["device"].toString(), p["graph"].toString(),false,instance);
        }
    }
}
void MainWindow::sync_node_views(const QJsonObject& document) {
    if(_view_draft && _view_draft_device!=_application.device_id())_view_draft.reset();
    // An empty authoritative graph is the selected endpoint's retired session.
    // A replacement service may have a lower epoch; discard only presentation
    // owners/fences here before binding its graph, without touching processing.
    if(document.isEmpty() && !_view_draft) {
        for(auto* dock:graph_docks())dock->setProperty("numericEndpoint",QVariant{});
        const auto previous=_active_node_views;
        clear_node_views();
        for(const auto& binding:_numeric_bindings)_numeric_views.unbind(binding.endpoint,binding.plane);
        for(const auto& ports:_metric_bindings)for(const auto& binding:ports)_numeric_views.unbind(binding.endpoint,binding.plane);
        _metric_bindings.clear();_numeric_bindings.clear();_active_node_views.clear();
        refresh_graph_navigation(previous);refresh_subscriptions();
        return;
    }
    domain::NumericProcessingGraph runtime_graph;
    const bool runtime_available=!document.isEmpty() && bool(desktop::decode_numeric_graph(QJsonDocument(document).toJson(QJsonDocument::Compact),runtime_graph));
    if(!runtime_available && !_view_draft)return;
    if(_view_draft)if(auto* editor=data_flow_editor()) {
        domain::NumericProcessingGraph current;
        if(editor->numeric_draft(current))_view_draft=std::move(current);
    }
    const auto& graph=_view_draft?*_view_draft:runtime_graph;
    for(auto* dock:graph_docks())dock->setProperty("numericEndpoint",QVariant{});
    const auto previous=_active_node_views;
    _active_node_views.clear();
    const QMap<QString,QString> fixed_views{{"AudioView","rawSamples"},{"WindowView","windowSignal"},
        {"WindowedView","windowedSignal"},{"WindowFunctionView","windowFunction"},
        {"FFTView","fft"},{"MelView","mel"},{"SpectrogramView","spectrogram"}};
    QSet<QString> used;
    QMap<QString,NumericBinding> bindings;
    const auto admits=[&](const NumericBinding& binding,const domain::ProcessingPort& shown) {
        if(!runtime_available || runtime_graph.graph_id!=binding.endpoint.graph_id)return false;
        const auto node=std::find_if(runtime_graph.nodes.begin(),runtime_graph.nodes.end(),[&](const auto& n){return n.id==binding.endpoint.node;});
        if(node==runtime_graph.nodes.end())return false;
        const auto& ports=binding.endpoint.direction==domain::PortDirection::Input?node->inputs:node->outputs;
        const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==binding.endpoint.port;});
        return port!=ports.end() && signal_contracts::same_contract(port->contract.view(),shown.contract.view());
    };
    for(const auto& node:graph.nodes) {
        const QString kind=QString::fromStdString(node.kind);
        domain::ResolvedProcessingNode resolved;
        if(!domain::resolve_processing_node(node,numeric_editor::limits(),resolved))continue;
        const auto node_kind=domain::node_kind(node.kind);
        if(resolved.role!=domain::NodeRole::Sink || !node_kind || !domain::is_view(*node_kind) || node.inputs.empty())continue;
        QString key=fixed_views.value(kind);
        if(key.isEmpty() || used.contains(key))key=ensure_node_view(node.id,kind,_application.device_id(),QString::fromStdString(graph.graph_id),true,0,&node);
        used.insert(key);_active_node_views[node.id]=key;
        if(auto* dock=_node_views.contains(key)?_node_views[key].dock:_panels.value(key))dock->setProperty("graphNode",int(node.id));
        const auto prefs=_application.state().file_configs.ui.graph_views.value(key).toObject();
        NumericBinding binding{{graph.graph_id,node.id,domain::PortDirection::Input,node.inputs.front().id},
            std::size_t(std::max(0,prefs["plane"].toInt())),_application.device_id()};
        if(kind!="ScalarView" && admits(binding,node.inputs.front()))bindings[key]=binding;
    }
    for(const auto& key:_node_views.keys()) {
        const auto view=_node_views[key];
        if(view.device!=_application.device_id() || view.graph!=QString::fromStdString(graph.graph_id))continue;
        const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==view.node;});
        if(node!=graph.nodes.end())ensure_node_view(view.node,QString::fromStdString(node->kind),view.device,view.graph,false,
            view.dock->property("viewInstance").toInt(),&*node);
    }
    for(auto it=_node_views.begin();it!=_node_views.end();++it) {
        if(it->device!=_application.device_id() || it->graph!=QString::fromStdString(graph.graph_id))continue;
        auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==it->node;});
        if(node==graph.nodes.end() || !numeric_editor::is_graph_view(QString::fromStdString(node->kind))){it->dock->toggleView(false);continue;}
        if(bindings.contains(it.key()) || node->kind=="ScalarView")continue;
        domain::ResolvedProcessingNode resolved;
        if(!domain::resolve_processing_node(*node,numeric_editor::limits(),resolved))continue;
        const auto direction=resolved.role==domain::NodeRole::Sink?domain::PortDirection::Input:domain::PortDirection::Output;
        const auto& ports=direction==domain::PortDirection::Input?node->inputs:node->outputs;
        if(ports.empty())continue;
        const auto prefs=_application.state().file_configs.ui.graph_views.value(it.key()).toObject();
        const auto port=std::uint16_t(prefs.value("port").toInt(ports.front().id));
        if(std::none_of(ports.begin(),ports.end(),[&](const auto& p){return p.id==port;}))continue;
        const auto selected=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==port;});
        NumericBinding binding{{graph.graph_id,node->id,direction,port},std::size_t(std::max(0,prefs["plane"].toInt())),it->device};
        if(admits(binding,*selected))bindings[it.key()]=binding;
        if(!_active_node_views.contains(node->id))_active_node_views[node->id]=it.key();
    }
    for(auto it=_panels.begin();it!=_panels.end();++it)if(it.key()!="dataFlow" && !used.contains(it.key())) {
        it.value()->toggleView(false);
        it.value()->setProperty("graphNode",QVariant{});
    }
    QSet<QString> targets;
    for(auto it=_node_views.cbegin();it!=_node_views.cend();++it)targets.insert(it.key());
    for(auto it=_panels.cbegin();it!=_panels.cend();++it)if(it.key()!="dataFlow")targets.insert(it.key());
    QMap<QString,QList<NumericBinding>> metricBindings;
    for(const auto& key:targets) {
        auto* dock=_node_views.contains(key)?_node_views[key].dock:_panels.value(key);
        auto* panel=dock?qobject_cast<GraphPanel*>(dock->widget()):nullptr;
        if(!panel || !panel->metrics_view())continue;
        QList<NumericMetricsView::Input> inputs;
        const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==dock->property("graphNode").toUInt();});
        if(node!=graph.nodes.end() && node->kind=="ScalarView") {
            const auto presentation=_application.state().data_flow_graph?_application.state().data_flow_graph->delegateModel<AudioNodeModel>(node->id):nullptr;
            const auto options=presentation?presentation->presentation().value("scalar_inputs").toObject():QJsonObject{};
            for(const auto& port:node->inputs) {
                const auto configured=options.value(QString::number(port.id)).toObject();
                if(!configured.value("enabled").toBool(true))continue;
                auto name=configured.value("display_name").toString().trimmed();
                inputs.push_back({port.id,name});
                NumericBinding binding{{graph.graph_id,node->id,domain::PortDirection::Input,port.id},0,_application.device_id()};
                if(admits(binding,port))metricBindings[key].push_back(binding);
            }
        }
        panel->metrics_view()->set_inputs(inputs);
        // Unavailable/disconnected values must not retain a previous reading.
        for(const auto& input:inputs)if(std::none_of(metricBindings[key].cbegin(),metricBindings[key].cend(),[&](const auto& b){return b.endpoint.port==input.port;}))
            panel->metrics_view()->set_unavailable(input.port);
    }
    const auto hasMetric=[&](const NumericBinding& candidate){
        for(const auto& ports:metricBindings)for(const auto& b:ports)if(b.endpoint==candidate.endpoint && b.plane==candidate.plane)return true;
        return false;
    };
    for(const auto& ports:_metric_bindings)for(const auto& old:ports)
        if(!hasMetric(old) && std::none_of(bindings.cbegin(),bindings.cend(),[&](const auto& b){return b.endpoint==old.endpoint && b.plane==old.plane;}))
            _numeric_views.unbind(old.endpoint,old.plane);
    _metric_bindings=metricBindings;
    for(const auto& ports:_metric_bindings)for(const auto& b:ports)_numeric_views.bind(b.endpoint,b.plane);
    for(auto it=_metric_bindings.cbegin();it!=_metric_bindings.cend();++it)if(!it.value().isEmpty()){
        auto* dock=_node_views.contains(it.key())?_node_views[it.key()].dock:_panels.value(it.key());
        const auto& b=it.value().front();
        if(dock)dock->setProperty("numericEndpoint",QVariantMap{{"graph",QString::fromStdString(b.endpoint.graph_id)},
            {"device",b.device},{"node",int(b.endpoint.node)},{"direction","input"},{"port",int(b.endpoint.port)},{"plane",qulonglong(b.plane)}});
    }
    for(const auto& key:targets)if(!bindings.contains(key) && !_metric_bindings.contains(key)) {
        auto* dock=_node_views.contains(key)?_node_views[key].dock:_panels.value(key);
        if(!dock)continue;
        if(auto* panel=qobject_cast<GraphPanel*>(dock->widget())) {
            auto target=panel->all_render_targets();
            presentation::NumericViewAdapter::clear_render_target(target);
            if(!panel->active_spectrogram() || !panel->active_spectrogram()->paused())
                target.exact_label->caption("Renderer projection unavailable",{},false,false);
            if(target.spectrogram)target.spectrogram->caption("Renderer projection unavailable",{},false,false);
            if(target.image)target.image->caption("Renderer projection unavailable",{},false,false);
            if(target.pie)target.pie->caption("Renderer projection unavailable",{},false,false);
        }
    }
    for(auto it=_numeric_bindings.begin();it!=_numeric_bindings.end();++it) {
        // Several dock instances share one endpoint/plane presentation state.
        // Release that state only when the new binding set has no consumer.
        if(!hasMetric(*it) && std::none_of(bindings.cbegin(),bindings.cend(),[&](const auto& binding) {
            return binding.endpoint==it->endpoint && binding.plane==it->plane;
        }))
            _numeric_views.unbind(it->endpoint,it->plane);
    }
    _numeric_bindings=std::move(bindings);
    for(auto it=_numeric_bindings.begin();it!=_numeric_bindings.end();++it) {
        _numeric_views.bind(it->endpoint,it->plane);
        auto* dock=_node_views.contains(it.key())?_node_views[it.key()].dock:_panels.value(it.key());
        if(dock)dock->setProperty("numericEndpoint",QVariantMap{
            {"graph",QString::fromStdString(it->endpoint.graph_id)}, {"device",it->device},
            {"node",int(it->endpoint.node)}, {"direction",it->endpoint.direction==domain::PortDirection::Input?"input":"output"},
            {"port",int(it->endpoint.port)}, {"plane",qulonglong(it->plane)}});
        auto* panel=dock?qobject_cast<GraphPanel*>(dock->widget()):nullptr;
        if(!panel)continue;
        // Only the applied graph grants physical units; an editing draft cannot
        // authorize coordinates before its configuration links are admitted.
        const bool applied=runtime_available && runtime_graph.graph_id==it->endpoint.graph_id;
        panel->set_axis_permissions({
            applied && domain::has_configuration_binding(runtime_graph,it->endpoint.node,domain::ConfigurationRole::FrequencyGrid),
            applied && domain::has_configuration_binding(runtime_graph,it->endpoint.node,domain::ConfigurationRole::SampleClock)});
        const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==it->endpoint.node;});
        if(node==graph.nodes.end())continue;
        const auto& ports=it->endpoint.direction==domain::PortDirection::Input?node->inputs:node->outputs;
        const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==it->endpoint.port;});
        if(port==ports.end())continue;
        const auto data=port->contract.view();const auto* numeric=std::get_if<signal_contracts::NumericContract>(&data);
        panel->set_numeric_plane_count(numeric?numeric->channel_order.size():0,it->plane);
    }
    refresh_graph_navigation(previous);apply_surface_settings();refresh_subscriptions();render_numeric_views();
}
void MainWindow::clear_node_views() {
    _numeric_changed_endpoints.clear();
    _vector_delivery_queues.clear();
    _numeric_views.clear();
    for(auto* dock:graph_docks())if(auto* panel=qobject_cast<GraphPanel*>(dock->widget())){
        panel->set_axis_permissions({});
        presentation::NumericViewAdapter::clear_render_target(panel->all_render_targets());
    }
}
void MainWindow::refresh_vector_delivery_queues() {
    std::vector<pipeline::EndpointIdentity> selected;
    const auto collect=[&](const NumericBinding& binding){
        if(binding.device==_application.device_id()&&std::find(selected.begin(),selected.end(),binding.endpoint)==selected.end())
            selected.push_back(binding.endpoint);
    };
    for(const auto& binding:_numeric_bindings)collect(binding);
    for(const auto& ports:_metric_bindings)for(const auto& binding:ports)collect(binding);
    // The engine preview arena has 128 slots. Reserve three existing leases
    // per subscribed endpoint plus eight scratch slots before adding jitter
    // leases. Larger subscription sets retain the existing latest-only path.
    const auto capacity=selected.empty()||selected.size()>40?std::size_t{0}:
        std::min(std::size_t{3},(120-3*selected.size())/selected.size());
    std::vector<VectorDeliveryQueue> prepared;
    if(capacity>=2&&!_restoring_preset)for(auto it=_numeric_bindings.cbegin();it!=_numeric_bindings.cend();++it){
        if(it->device!=_application.device_id())continue;
        auto* dock=_node_views.contains(it.key())?_node_views[it.key()].dock:_panels.value(it.key());
        auto* panel=dock?qobject_cast<GraphPanel*>(dock->widget()):nullptr;
        if(!panel||!panel->active_chart()||dock->isHidden()||!dock->property("renderExposed").toBool())continue;
        if(std::any_of(prepared.begin(),prepared.end(),[&](const auto& q){return q.endpoint==it->endpoint;}))continue;
        auto prior=std::find_if(_vector_delivery_queues.begin(),_vector_delivery_queues.end(),[&](const auto& q){return q.endpoint==it->endpoint;});
        if(_vector_delivery_device==_application.device_id()&&prior!=_vector_delivery_queues.end()&&prior->capacity==capacity)
            prepared.push_back(std::move(*prior));
        else{VectorDeliveryQueue queue;queue.endpoint=it->endpoint;queue.capacity=capacity;prepared.push_back(std::move(queue));}
    }
    _vector_delivery_queues=std::move(prepared);_vector_delivery_device=_application.device_id();
    for(const auto& queue:_vector_delivery_queues)if(queue.count)_numeric_render_dirty=true;
}
void MainWindow::consume_numeric_frame(const pipeline::DeliveredNumericFrame& frame) {
    core::perf::Scope profile(core::perf::Stage::NumericFrameReceive);
    if(_restoring_preset)for(auto& queue:_vector_delivery_queues)queue.clear();
    if(!_restoring_preset&&frame.context&&_vector_delivery_device==_application.device_id()){
        auto found=std::find_if(_vector_delivery_queues.begin(),_vector_delivery_queues.end(),[&](const auto& q){return q.endpoint==frame.context->endpoint;});
        if(found!=_vector_delivery_queues.end()){
            auto& queue=*found;
            if(queue.seen&&pipeline::delivery_precedes(frame.delivery,queue.fence))return;
            bool vectorFrame=false;
            if(frame.frame){const auto view=frame.frame.view();const auto* numeric=std::get_if<signal_contracts::NumericView>(&view.payload);
                vectorFrame=numeric&&numeric->contract&&numeric->contract->shape.rank==signal_contracts::Rank::Vector;}
            const bool available=frame.status==signal_contracts::Status::ok&&frame.frame&&frame.frame.view().stamp.available&&
                vectorFrame;
            if(!available){
                queue.clear();queue.seen=true;queue.fence=frame.delivery;queue.context=frame.context;queue.sequence=0;
                _numeric_views.accept(frame);request_numeric_render(frame.context->endpoint);return;
            }
            const auto& stamp=frame.frame.view().stamp;
            const bool reset=!queue.seen||frame.delivery!=queue.fence||queue.context!=frame.context||
                stamp.producer_epoch!=queue.producer||stamp.generation!=queue.generation||stamp.discontinuity;
            if(!reset&&stamp.sequence<=queue.sequence)return;
            if(reset)queue.clear();
            queue.seen=true;queue.fence=frame.delivery;queue.context=frame.context;
            queue.producer=stamp.producer_epoch;queue.generation=stamp.generation;queue.sequence=stamp.sequence;
            if(queue.count==queue.capacity){queue.frames[queue.head]={};queue.head=(queue.head+1)%queue.capacity;--queue.count;}
            queue.frames[(queue.head+queue.count)%queue.capacity]=frame;++queue.count;
            request_numeric_render(frame.context->endpoint);return;
        }
    }
    _numeric_views.accept(frame);if(frame.context)request_numeric_render(frame.context->endpoint);
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
    _numeric_views.accept_history_snapshot(frame,ordinal,filled_count);
    // A completed snapshot is ready for projection now. The renderer owns its
    // cadence; a second GUI timer wait only adds latency to this response.
    if(frame.context)request_numeric_render(frame.context->endpoint);
}
void MainWindow::render_numeric_views(bool consumeQueuedFrames,bool changedOnly) {
    core::perf::Scope profile(core::perf::Stage::NumericRenderViews);
    // Removing an ADS area can synchronously expose another old dock while
    // preset-owned docks are being replaced. Project only the completed set.
    if(_restoring_preset)return;
    auto changed=std::move(_numeric_changed_endpoints);_numeric_changed_endpoints.clear();
    const auto affected=[&](const pipeline::EndpointIdentity& endpoint){
        return !changedOnly||std::find(changed.begin(),changed.end(),endpoint)!=changed.end();
    };
    if(consumeQueuedFrames)for(auto& queue:_vector_delivery_queues){
        if(!queue.count)continue;
        // A display consumes the newest owning preview. Walking an audio FIFO
        // at the GUI rate creates growing visible latency when capture is faster.
        const auto newest=(queue.head+queue.count-1)%queue.capacity;
        auto frame=std::move(queue.frames[newest]);
        if(std::find(changed.begin(),changed.end(),queue.endpoint)==changed.end())changed.push_back(queue.endpoint);
        while(queue.count){queue.frames[queue.head]={};queue.head=(queue.head+1)%queue.capacity;--queue.count;}
        _numeric_views.accept(frame);
    }
    for(const auto& queue:_vector_delivery_queues)if(queue.count)_numeric_render_dirty=true;
    if(_numeric_snapshot_dirty){_numeric_snapshot_dirty=false;_application.client().request_numeric_snapshots();}
    for(auto it=_numeric_bindings.cbegin();it!=_numeric_bindings.cend();++it) {
        if(it->device!=_application.device_id()||!affected(it->endpoint))continue;
        auto* dock=_node_views.contains(it.key())?_node_views[it.key()].dock:_panels.value(it.key());
        auto* panel=dock?qobject_cast<GraphPanel*>(dock->widget()):nullptr;
        if(!panel)continue;
        auto target=panel->render_target(!dock->isHidden() && dock->property("renderExposed").toBool());
        _numeric_views.render(it->endpoint,it->plane,target);
    }
    for(auto it=_metric_bindings.cbegin();it!=_metric_bindings.cend();++it) {
        auto* dock=_node_views.contains(it.key())?_node_views[it.key()].dock:_panels.value(it.key());
        auto* panel=dock?qobject_cast<GraphPanel*>(dock->widget()):nullptr;
        if(!panel || !panel->metrics_view())continue;
        auto target=panel->render_target(!dock->isHidden() && dock->property("renderExposed").toBool());
        for(const auto& b:it.value())if(b.device==_application.device_id()&&affected(b.endpoint))_numeric_views.render(b.endpoint,b.plane,target);
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
void MainWindow::apply_render_rate() {
    if (auto *settings = findChild<SettingsWidget *>()) settings->set_display_screen(screen());
    const auto prefs = _application.state().file_configs.ui.graph_rendering;
    _render_fps = SurfaceSettings::renderFps(prefs, screen() ? screen()->refreshRate() : 60.0);
    // Delivery has headroom for source block jitter; each renderer still owns
    // the configured paint cadence. This does not alter the processing clock.
    _application.set_numeric_preview_cadence_hz(std::min(240,_render_fps*2));
    _numeric_render_timer.setInterval(qMax(1,1000/qMax(1,_render_fps)));
    const int precision = SurfaceSettings::effective(prefs)["axis_precision"].toInt();
    for (auto *chart : {_raw_signal_chart, _window_signal_chart, _windowed_signal_chart,
                        _window_function_chart, _amplitudes_chart, _test_amplitudes_chart})
        if (chart) { chart->set_render_fps(_render_fps); chart->set_axis_precision(precision); }
    if (_spectrogram_view) _spectrogram_view->set_render_fps(_render_fps);
    for (auto &view : _node_views) {
        if (view.chart) { view.chart->set_render_fps(_render_fps); view.chart->set_axis_precision(precision); }
        if (view.spectrogram) view.spectrogram->set_render_fps(_render_fps);
    }
    for(auto* dock:graph_docks())if(auto* panel=qobject_cast<GraphPanel*>(dock->widget())){
        panel->set_render_fps(_render_fps);panel->set_axis_precision(precision);
    }
}

void MainWindow::apply_surface_settings() {
    apply_render_rate();
    const auto &ui = _application.state().file_configs.ui;
    auto legacy = ui.graph_views.value("spectrogram").toObject();
    for (auto it = _active_node_views.begin(); it != _active_node_views.end(); ++it)
        if (it.value() == "spectrogram") {
            const auto identity = QString::fromLatin1(
                QJsonDocument(QJsonArray{_application.device_id(), _application.graph_id(), it.key()})
                    .toJson(QJsonDocument::Compact)
                    .toHex());
            if(ui.graph_views.contains(identity))legacy = ui.graph_views.value(identity).toObject();
            break;
        }
    if (_spectrogram_view)
        _spectrogram_view->set_surface_settings(SurfaceSettings::effective(ui.graph_rendering, legacy));
    for (auto it = _node_views.begin(); it != _node_views.end(); ++it)
        if (it->spectrogram)
            it->spectrogram->set_surface_settings(
                SurfaceSettings::effective(ui.graph_rendering, ui.graph_views.value(it.key()).toObject()));
    for(auto* dock:graph_docks())if(auto* panel=qobject_cast<GraphPanel*>(dock->widget()))
        if(auto* surface=panel->active_spectrogram(); surface && surface!=_spectrogram_view)surface->set_surface_settings(
            SurfaceSettings::effective(ui.graph_rendering,ui.graph_views.value(dock->property("graphViewKey").toString()).toObject()));
}

void MainWindow::connect_surface_status(SpectrogramGraph *view, const QString &key) {
    const auto identity = [this, key] {
        if (key != "spectrogram") return key;
        for (auto it = _active_node_views.begin(); it != _active_node_views.end(); ++it)
            if (it.value() == key)
                return QString::fromLatin1(
                    QJsonDocument(QJsonArray{_application.device_id(), _application.graph_id(), it.key()})
                        .toJson(QJsonDocument::Compact).toHex());
        return key;
    };
    connect(view, &SpectrogramGraph::surface_settings_changed, this,
            [this, identity](const QJsonObject &options) {
                auto &ui = _application.state().file_configs.ui;
                const auto id = identity();
                auto prefs = ui.graph_views.value(id).toObject();
                auto override = prefs.value("surface_override").toObject();
                auto values = override.value("values").toObject();
                const auto normalized = SurfaceSettings::normalized(options);
                const auto previous=SurfaceSettings::effective(ui.graph_rendering,prefs);
                const bool aspectEdited=normalized["scene_aspect_mode"]!=previous["scene_aspect_mode"] ||
                    normalized["scene_time_aspect"]!=previous["scene_time_aspect"] ||
                    normalized["scene_band_aspect"]!=previous["scene_band_aspect"];
                for (const auto *field : {"y_range_mode", "y_min", "y_max", "auto_range_interval_seconds", "reverse_time", "reverse_amplitude",
                                         "reverse_bands", "gradient", "gradient_layout", "gradient_stops"})
                    values[field] = normalized[field];
                const auto global=SurfaceSettings::effective(ui.graph_rendering);
                const bool customAspect=normalized["scene_aspect_mode"]!=global["scene_aspect_mode"] ||
                    normalized["scene_time_aspect"]!=global["scene_time_aspect"] ||
                    normalized["scene_band_aspect"]!=global["scene_band_aspect"];
                // Mode and both ratios form one policy: a fixed custom ratio
                // must not become inactive when global mode later changes.
                if(aspectEdited)for(const auto* field:{"scene_aspect_mode","scene_time_aspect","scene_band_aspect"}) {
                    if(customAspect)values[field]=normalized[field];
                    else values.remove(field); // explicitly matching policy inherits future defaults
                }
                override["enabled"] = true;
                override["values"] = values;
                prefs["surface_override"] = override;
                ui.graph_views[id] = prefs;
                _camera_save_timer.start();
            });
    connect(view, &SpectrogramGraph::edit_color_scale_requested, this, [this, identity] {
        if (auto *settings = findChild<SettingsWidget *>()) settings->edit_color_scale(identity());
    });
    connect(view, &SpectrogramGraph::surface_settings_error, this, [this, key, view](const QString &error) {
        if (error.isEmpty())
            _surface_errors.remove(key);
        else
            _surface_errors[key] =
                (view->parentWidget() ? view->parentWidget()->windowTitle() : ui::text("Spectrogram")) + ": " + error;
        auto *label = findChild<QLabel *>("surfaceSettingsStatus");
        if (!label) {
            label = new QLabel(this);
            label->setObjectName("surfaceSettingsStatus");
            statusBar()->addPermanentWidget(label);
        }
        label->setText(_surface_errors.values().join(QStringLiteral(" \u00b7 ")));
        label->setToolTip(label->text());
        label->setVisible(!_surface_errors.isEmpty());
    });
}
