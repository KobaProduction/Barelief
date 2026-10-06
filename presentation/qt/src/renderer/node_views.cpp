#include "core_app/main_window.h"
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

Q_DECLARE_LOGGING_CATEGORY(core_app_window)

QString MainWindow::ensure_node_view(int node, const QString &kind, const QString &device, const QString &graph,
                                     bool open, int instance) {
    QJsonArray identity{device, graph, node};
    if(instance) identity.append(instance);
    const auto key = QString::fromLatin1(QJsonDocument(identity).toJson(QJsonDocument::Compact).toHex());
    if (_node_views.contains(key))
        return key;
    NodeView view;
    view.node = node;
    view.device = device;
    view.graph = graph;
    view.kind = kind;
    auto *dock = new WorkspaceDock(AudioNodeModel(kind).caption()+" #"+QString::number(node), this);
    connect(&ui::Language::instance(),&ui::Language::changed,dock,[dock,kind,node] {
        dock->setWindowTitle(AudioNodeModel(kind).caption()+" #"+QString::number(node));
    });
    dock->setObjectName("nodeView_" + key);
    view.dock = dock;
    dock->setProperty("graphNode",node);
    dock->setProperty("viewInstance",instance);
    setup_graph_dock_controls(dock);
    auto preferences = _application.state().file_configs.ui.graph_views.value(key).toObject();
    preferences["kind"] = kind;
    preferences["instance"] = instance;
    if (kind == "SpectrogramView") {
        view.spectrogram = new SpectrogramGraph(dock);
        connect_surface_status(view.spectrogram, key);
        view.spectrogram->set_presentation_state(preferences.value("renderer").toObject());
        view.spectrogram->set_camera_controls(_application.state().file_configs.ui.camera_controls);
        view.spectrogram->set_camera_state(preferences.value("camera").toObject());
        view.spectrogram->set_side_panel_ratio(preferences.value("side_ratio").toDouble(.25));
        view.spectrogram->set_surface_settings(
            SurfaceSettings::effective(_application.state().file_configs.ui.graph_rendering, preferences));
        view.spectrogram->initialize({240, 180}, {1200, 900});
        dock->setWidget(view.spectrogram);
        connect(view.spectrogram, &SpectrogramGraph::presentation_changed, this, [this, key](const QJsonObject &state) {
            auto prefs = _application.state().file_configs.ui.graph_views[key].toObject();
            prefs["renderer"] = state;
            _application.state().file_configs.ui.graph_views[key] = prefs;
            _camera_save_timer.start();
        });
        connect(view.spectrogram, &SpectrogramGraph::camera_changed, this, [this, key](const QJsonObject &camera) {
            auto p = _application.state().file_configs.ui.graph_views[key].toObject();
            p["camera"] = camera;
            _application.state().file_configs.ui.graph_views[key] = p;
            _camera_save_timer.start();
        });
        connect(view.spectrogram, &SpectrogramGraph::side_panel_resized, this, [this, key](float ratio) {
            auto p = _application.state().file_configs.ui.graph_views[key].toObject();
            p["side_ratio"] = ratio;
            _application.state().file_configs.ui.graph_views[key] = p;
            _camera_save_timer.start();
        });
    } else {
        view.chart = kind == "FFTView" ? static_cast<AbstractChartView *>(new FreqChartView)
                                       : static_cast<AbstractChartView *>(new LineChartView);
        view.chart->chart()->setTitle({});
        view.chart->set_range(-1, 1);
        view.chart->set_auto_resizing(true);
        view.chart->set_auto_gain(true, 1, -1);
        auto *panel = new GraphPanel(key, view.chart, preferences, dock);
        dock->setWidget(panel);
        auto refresh = [this, node, panel] {
            for (const auto &value : _application.client().snapshot().graph.value("nodes").toArray()) {
                auto n = value.toObject();
                if (n["id"].toInt() == node) {
                    auto p = n["internal-data"].toObject();
                    panel->set_processing_choices(p.value("channel").toInt(), p.value("merge").toInt(2));
                }
            }
        };
        refresh();
        connect(&_application, &GuiPresenter::processing_state_changed, panel, refresh);
        connect(panel, &GraphPanel::channel_requested, this,
                [this, node](int channel) { _application.set_node_parameters(node, {{"channel", channel}}); });
        connect(panel, &GraphPanel::merge_requested, this, [this, node, kind](int merge) {
            if (kind == "FFTView")
                _application.set_node_parameters(node, {{"merge", merge}});
        });
        connect(panel, &GraphPanel::presentation_changed, this, [this](const QString &id, const QJsonObject &updates) {
            auto p = _application.state().file_configs.ui.graph_views[id].toObject();
            for (auto it = updates.begin(); it != updates.end(); ++it)
                p[it.key()] = it.value();
            _application.state().file_configs.ui.graph_views[id] = p;
            _workspace_save_timer.start();
        });
    }
    if (view.chart) view.chart->set_render_fps(_render_fps);
    if (view.spectrogram) view.spectrogram->set_render_fps(_render_fps);
    _node_views[key] = view;
    set_graph_exposed(key, dock, false);
    connect(dock, &WorkspaceDock::visibilityChanged, this, [this, key, dock](bool visible) {
        set_graph_exposed(key, dock, visible);
        refresh_subscriptions();
    });
    addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->toggleView(preferences.value("open").toBool(open));
    preferences["device"] = device;
    preferences["graph"] = graph;
    preferences["node"] = node;
    preferences["kind"] = kind;
    preferences["open"] = !dock->isHidden();
    _application.state().file_configs.ui.graph_views[key] = preferences;
    connect(dock, &WorkspaceDock::visibilityChanged, this, [this, key](bool visible) {
        if (_restoring_layout)
            return;
        auto &view = _node_views[key];
        auto p = _application.state().file_configs.ui.graph_views[key].toObject();
        p["open"] = !view.dock->isHidden();
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
void MainWindow::sync_node_views(const QJsonObject &graph) {
    const auto previousViews = _active_node_views;
    _active_node_views.clear();
    const QMap<QString, QString> legacy{{"AudioView", "rawSamples"},
                                        {"WindowView", "windowSignal"},
                                        {"WindowedView", "windowedSignal"},
                                        {"WindowFunctionView", "windowFunction"},
                                        {"FFTView", "fft"},
                                        {"MelView", "mel"},
                                        {"SpectrogramView", "spectrogram"}};
    QSet<QString> used;
    const QMap<QString, int> legacy_ids{{"AudioView", 9}, {"FFTView", 5}, {"MelView", 6}, {"SpectrogramView", 7}};
    for (const auto &value : graph.value("nodes").toArray()) {
        const auto n = value.toObject();
        const auto kind = n.value("internal-data").toObject().value("model-name").toString();
        const auto definition = pipeline::node_definition(kind);
        if (!legacy.contains(kind) && definition.category != "views")
            continue;
        const int id = n.value("id").toInt();
        const auto old = legacy.value(kind);
        if (_application.device_id() == _application.local_device_id() && _application.graph_id() == "main" &&
            !used.contains(old) && !old.isEmpty() && legacy_ids.value(kind, -1) == id) {
            used.insert(old);
            _active_node_views[id] = old;
            _panels[old]->setProperty("graphNode",id);
            const auto identity =
                QString::fromLatin1(QJsonDocument(QJsonArray{_application.device_id(), _application.graph_id(), id})
                                        .toJson(QJsonDocument::Compact)
                                        .toHex());
            auto preferences = _application.state().file_configs.ui.graph_views.value(identity).toObject();
            if (preferences.isEmpty())
                preferences = _application.state().file_configs.ui.graph_views.value(old).toObject();
            preferences["device"] = _application.device_id();
            preferences["graph"] = _application.graph_id();
            preferences["node"] = id;
            preferences["kind"] = kind;
            preferences["legacy_panel"] = old;
            _application.state().file_configs.ui.graph_views[identity] = preferences;
        } else
            _active_node_views[id] = ensure_node_view(id, kind, _application.device_id(), _application.graph_id());
    }
    // Removed views are cleared, while compatible views retain their history.
    for (auto it = previousViews.begin(); it != previousViews.end(); ++it) {
        if (_active_node_views.value(it.key()) == it.value()) continue;
        const auto key = it.value();
        auto *removedDock=_panels.contains(key) ? _panels.value(key) : _node_views.value(key).dock;
        if(removedDock) removedDock->hide();
        if (key == "spectrogram") _spectrogram_view->update({});
        else if (_node_views.contains(key)) {
            auto &view = _node_views[key];
            if (view.chart) view.chart->update({});
            if (view.spectrogram) view.spectrogram->update({});
        } else if (auto *panel = qobject_cast<GraphPanel *>(_panels.value(key) ? _panels[key]->widget() : nullptr)) {
            Q_UNUSED(panel);
            const QMap<QString, AbstractChartView *> charts{{"rawSamples", _raw_signal_chart},
                {"windowSignal", _window_signal_chart}, {"windowedSignal", _windowed_signal_chart},
                {"windowFunction", _window_function_chart}, {"fft", _amplitudes_chart}, {"mel", _test_amplitudes_chart}};
            if (charts.value(key)) charts[key]->update({});
        }
    }
    // Resolve declared dimensions for an empty view before its first frame.
    for(const auto &view:_node_views)
        if(view.device==_application.device_id() && view.graph==_application.graph_id() && !_active_node_views.contains(view.node))
            view.dock->hide();
    // Runtime metadata subsequently becomes authoritative; clearing retains it.
    QMap<int, QJsonObject> parameters;
    QMultiMap<int, int> parents;
    for (const auto &v : graph.value("nodes").toArray()) {
        const auto n = v.toObject();
        parameters[n["id"].toInt()] = n["internal-data"].toObject();
    }
    for (const auto &v : graph.value("connections").toArray()) {
        const auto c = v.toObject();
        parents.insert(c["inNodeId"].toInt(), c["outNodeId"].toInt());
    }
    for (auto it = _active_node_views.begin(); it != _active_node_views.end(); ++it) {
        auto *surface = it.value() == "spectrogram" ? _spectrogram_view : _node_views.value(it.value()).spectrogram;
        if (!surface)
            continue;
        size_t capacity = 400, bands = 50;
        QSet<int> seen;
        QList<int> queue{it.key()};
        bool foundCapacity = false, foundBands = false;
        while (!queue.isEmpty()) {
            const int id = queue.takeFirst();
            if (seen.contains(id))
                continue;
            seen.insert(id);
            const auto p = parameters.value(id);
            const auto kind = p["model-name"].toString();
            if (kind == "Spectrogram" && !foundCapacity) {
                capacity = size_t(std::clamp(p.value("history_size").toInt(400), 2, 2000));
                foundCapacity = true;
            }
            if (kind == "Mel" && !foundBands) {
                bands = size_t(std::clamp(p.value("bands").toInt(50), 1, 50));
                foundBands = true;
            }
            for (int parent : parents.values(id))
                queue.append(parent);
        }
        if ((foundCapacity && foundBands) || !previousViews.contains(it.key()))
            surface->set_history_dimensions(capacity, bands);
    }
    refresh_graph_navigation(previousViews);
    apply_surface_settings();
    refresh_subscriptions();
}
void MainWindow::clear_node_views() {
    for (auto *chart : {_raw_signal_chart, _window_signal_chart, _windowed_signal_chart, _window_function_chart,
                        _amplitudes_chart, _test_amplitudes_chart})
        chart->update({});
    _spectrogram_view->update({});
    for (auto &view : _node_views) {
        if (view.chart)
            view.chart->update({});
        if (view.spectrogram)
            view.spectrogram->update({});
    }
}
void MainWindow::consume_node_views(const QMap<int, pipeline::TypedFrame> &views) {
    QMap<QString,int> presentations;
    for(auto it=_active_node_views.cbegin();it!=_active_node_views.cend();++it) presentations[it.value()]=it.key();
    for(auto it=_node_views.cbegin();it!=_node_views.cend();++it)
        if(it->device==_application.device_id() && it->graph==_application.graph_id() && _active_node_views.contains(it->node))
            presentations[it.key()]=it->node;
    for (auto it = presentations.begin(); it != presentations.end(); ++it) {
        const auto key = it.key();
        const int node=it.value();
        AbstractChartView *chart = nullptr;
        SpectrogramGraph *spectrogram = nullptr;
        WorkspaceDock *dock = nullptr;
        if (_node_views.contains(key)) {
            const auto &view = _node_views[key];
            chart = view.chart;
            spectrogram = view.spectrogram;
            dock = view.dock;
        } else {
            dock = _panels.value(key);
            if (key == "spectrogram")
                spectrogram = _spectrogram_view;
            else if (key == "rawSamples")
                chart = _raw_signal_chart;
            else if (key == "windowSignal")
                chart = _window_signal_chart;
            else if (key == "windowedSignal")
                chart = _windowed_signal_chart;
            else if (key == "windowFunction")
                chart = _window_function_chart;
            else if (key == "fft")
                chart = _amplitudes_chart;
            else if (key == "mel")
                chart = _test_amplitudes_chart;
        }
        if (!dock || !_render_visible.value(key))
            continue;
        if (!views.contains(node) || !views[node].available) {
            if (chart && chart->value_count())
                chart->update(std::vector<float>(chart->value_count(), 0.0f));
            // A missing clock/contract cannot fabricate history; hold it until
            // the engine delivers correctly typed zero frames or explicit Stop.
            continue;
        }
        const auto &frame = views[node];
        if (spectrogram) {
            if (auto *settings = findChild<SettingsWidget *>()) {
                const auto identity = QString::fromLatin1(
                    QJsonDocument(QJsonArray{_application.device_id(), _application.graph_id(), node})
                        .toJson(QJsonDocument::Compact).toHex());
                settings->set_graph_dimensions(identity, frame.history_capacity, frame.bands);
            }
            spectrogram->set_history_dimensions(frame.history_capacity, frame.bands);
            spectrogram->update_frame_history(frame.history);
        }
        if (chart) {
            if (auto *frequency = dynamic_cast<FreqChartView *>(chart))
                frequency->update_freq_step(frame.frequency_step);
            chart->update(frame.values.empty() ? frame.left : frame.values);
        }
    }
}

void MainWindow::apply_render_rate() {
    if (auto *settings = findChild<SettingsWidget *>()) settings->set_display_screen(screen());
    const auto prefs = _application.state().file_configs.ui.graph_rendering;
    _render_fps = SurfaceSettings::renderFps(prefs, screen() ? screen()->refreshRate() : 60.0);
    const int precision = SurfaceSettings::effective(prefs)["axis_precision"].toInt();
    for (auto *chart : {_raw_signal_chart, _window_signal_chart, _windowed_signal_chart,
                        _window_function_chart, _amplitudes_chart, _test_amplitudes_chart})
        if (chart) { chart->set_render_fps(_render_fps); chart->set_axis_precision(precision); }
    if (_spectrogram_view) _spectrogram_view->set_render_fps(_render_fps);
    for (auto &view : _node_views) {
        if (view.chart) { view.chart->set_render_fps(_render_fps); view.chart->set_axis_precision(precision); }
        if (view.spectrogram) view.spectrogram->set_render_fps(_render_fps);
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
            legacy = ui.graph_views.value(identity).toObject();
            break;
        }
    if (_spectrogram_view)
        _spectrogram_view->set_surface_settings(SurfaceSettings::effective(ui.graph_rendering, legacy));
    for (auto it = _node_views.begin(); it != _node_views.end(); ++it)
        if (it->spectrogram)
            it->spectrogram->set_surface_settings(
                SurfaceSettings::effective(ui.graph_rendering, ui.graph_views.value(it.key()).toObject()));
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
                for (const auto *field : {"y_range_mode", "y_min", "y_max", "reverse_time", "reverse_amplitude",
                                         "reverse_bands", "gradient", "gradient_layout", "gradient_stops"})
                    values[field] = normalized[field];
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
