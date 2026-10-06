#include "visualization/surface_settings.h"
#include "visualization/ui_text.h"
#include "core_app/graph_panel.h"
#include "visualization/abstract/fps.h"
#include "core_app/main_window.h"
#include <cmath>
#include <QStyle>
#include <QStatusBar>
#include <QLabel>

#include <QMenuBar>
#include <QVBoxLayout>
#include <QtWidgets/QStatusBar>
#include <QtCore/QLoggingCategory>
#include <QtGui/QShowEvent>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QToolButton>
#include <QPushButton>
#include <QSignalBlocker>
#include <QCloseEvent>
#include <QScreen>
#include <QWindow>
#include <QGuiApplication>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QMouseEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QFormLayout>
#include <QStandardItemModel>
#include <QActionGroup>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTabBar>
#include <QFileDialog>
#include "aggregator/aggregator_client.h"

#include "core_app/gui_presenter.h"
#include "core_app/settings_widget.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "visualization/charts/line.h"
#include "visualization/charts/bar.h"
#include "visualization/charts/freq.h"

Q_LOGGING_CATEGORY(core_app_window, "core_app.window")

MainWindow::MainWindow(GuiPresenter &application) : _application(application) {
    ui::Theme::instance().set(_application.state().file_configs.ui.theme);
    ui::Language::instance().set(_application.state().file_configs.ui.language);
    ui::bind(this, "windowTitle", "Data Processing System Core");
    setMinimumSize(800, 600);
    resize(1000, 800);

    setup_ui();
    if(auto *editor=data_flow_editor()) {
        auto *fileMenu=findChild<QMenu *>("fileMenu");
        fileMenu->addSeparator();
        auto *undo=editor->history().createUndoAction(fileMenu,ui::text("Undo"));
        auto *redo=editor->history().createRedoAction(fileMenu,ui::text("Redo"));
        undo->setObjectName("undoDataFlow"); redo->setObjectName("redoDataFlow");
        undo->setShortcut(QKeySequence("Alt+Z")); redo->setShortcut(QKeySequence("Alt+Shift+Z"));
        for(auto *action:{undo,redo}) { action->setShortcutContext(Qt::ApplicationShortcut); action->setShortcutVisibleInContextMenu(true); fileMenu->addAction(action); }
    }
    qApp->installEventFilter(this);
    // Familiar JetBrains navigation belongs to presentation, never the DSP core.
    auto *navigation=findChild<QMenu *>("graphsMenu");
    auto action=[this,navigation](const char *label,const char *id,const char *key,auto callback) {
        auto *item=navigation->addAction(ui::text(label)); item->setObjectName(id);
        ui::bind(item,"text",label); item->setShortcut(QKeySequence(QString::fromLatin1(key)));
        item->setShortcutContext(Qt::ApplicationShortcut); item->setShortcutVisibleInContextMenu(true);
        connect(item,&QAction::triggered,this,[this,callback] {
            auto *active=QApplication::activeWindow();
            bool belongs=active==this;
            for(auto *dock:workspaceDocks()) if(dock->window()==active) belongs=true;
            if(belongs) callback();
        });
    };
    navigation->addSeparator();
    action("Next tab","workspaceNextTab","Alt+Right",[this]{activateAdjacentTab(1);});
    action("Previous tab","workspacePreviousTab","Alt+Left",[this]{activateAdjacentTab(-1);});
    action("Detach active tab","workspaceDetachTab","Shift+F4",[this]{if(auto *dock=activeDock()) dock->setFloating(true);});
    action("Hide active panel","workspaceHidePanel","Shift+Escape",[this]{if(auto *dock=activeDock()) dock->hide();});
    action("Focus Data Flow","workspaceFocusFlow","F12",[this]{
        auto *flow=_panels.value("dataFlow"); flow->raise(); focusDock(flow);
    });
    action("Hide or restore tool panels","workspaceToggleTools","Ctrl+Shift+F12",[this]{
        auto *flow=_panels.value("dataFlow"); flow->toggleView(flow->isHidden());
    });
    action("Switch workspace tab…","workspaceSwitcher","Ctrl+Tab",[this]{
        auto *menu=new QMenu(this); menu->setAttribute(Qt::WA_DeleteOnClose);
        for(auto *dock:workspaceDocks()) if(!dock->isHidden()) {
            auto *item=menu->addAction(dock->windowTitle());
            connect(item,&QAction::triggered,this,[this,dock]{dock->raise();focusDock(dock);});
        }
        menu->popup(mapToGlobal(rect().center()));
    });

    connect(this,&WorkspaceWindow::graphAreaActivated,this,[this](WorkspaceDock *dock) {
        if (!_restoring_layout && is_graph_dock(dock) && in_editor_window(dock)) _last_graph_slot=dock;
    });
    auto *devices = new QComboBox(this);
    devices->setObjectName("engineDeviceSelector");
    devices->addItem(ui::text("In-app processor"), QJsonObject{{"uuid", _application.local_device_id()}});
    ui::bind(devices,"toolTip","Processes audio inside this application. External processors connect over TCP.");
    auto *deviceCorner = new QWidget(menuBar());
    deviceCorner->setObjectName("navigationControls");
    auto *deviceLayout = new QHBoxLayout(deviceCorner);
    deviceLayout->setContentsMargins(4, 0, 4, 0); deviceLayout->setSpacing(4);
    for(const auto *id:{"pipelinePresets","pipelineToggle"})
        if(auto *button=findChild<QToolButton *>(id)) { deviceLayout->addWidget(button); button->show(); }
    devices->hide();
    auto *engineButton = ui::tool_button(deviceCorner);
    engineButton->setObjectName("engineSelectorButton");
    engineButton->setMaximumWidth(260);
    auto *engineMenu = new QMenu(engineButton);
    ui::button_menu(engineButton,engineMenu);
    deviceLayout->addWidget(engineButton);
    const auto refreshEngineCaption = [devices, engineButton] {
        engineButton->setText(devices->currentText()); engineButton->setToolTip(devices->currentText());
    };
    connect(devices, &QComboBox::currentTextChanged, engineButton, refreshEngineCaption);
    refreshEngineCaption();
    menuBar()->setCornerWidget(deviceCorner);
    if (_application.is_remote()) {
        const auto endpoint = _application.device_id();
        const auto separator = endpoint.lastIndexOf(':');
        devices->addItem(endpoint, QJsonObject{{"uuid", endpoint},
            {"address", endpoint.left(separator)}, {"port", endpoint.mid(separator + 1).toInt()}});
        devices->setCurrentIndex(1);
    }
    auto *permissions = new QLabel(this);
    permissions->setObjectName("selectedEnginePermission");
    statusBar()->addPermanentWidget(permissions);
    connect(devices, &QComboBox::currentIndexChanged, this, [devices, permissions](int index) {
        const auto metadata = devices->itemData(index).toJsonObject();
        permissions->setText(metadata.contains("allow_mutations")
                                 ? (metadata.value("allow_mutations").toBool() ? ui::text("Editing allowed")
                                                                               : ui::text("Read-only engine"))
                                 : "");
    });
    auto *registry_dialog = new QDialog(this);
    registry_dialog->setObjectName("engineRegistryDialog");
    ui::bind(registry_dialog, "windowTitle", "Manage processors");
    registry_dialog->resize(600, 520);
    auto *registry_layout = new QVBoxLayout(registry_dialog);
    auto *explanation = ui::make<QLabel>("Local processing needs no registry. Discover external processors through an aggregator.", registry_dialog);
    explanation->setWordWrap(true);
    registry_layout->addWidget(explanation);
    auto *processorList = new QTreeWidget(registry_dialog);
    processorList->setObjectName("processorManagerList");
    processorList->setRootIsDecorated(false);
    processorList->setHeaderLabels({ui::text("Processor"), ui::text("Endpoint"), ui::text("State")});
    processorList->setMinimumHeight(110);
    processorList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    processorList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    processorList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    registry_layout->addWidget(processorList);
    auto *directGroup = ui::make<QGroupBox>("Direct connection", registry_dialog);
    auto *directForm = new QFormLayout(directGroup);
    auto *directHost = new QLineEdit("127.0.0.1", directGroup);
    directHost->setObjectName("processorHost");
    auto *directPort = new QSpinBox(directGroup);
    directPort->setObjectName("processorPort");
    directPort->setRange(1, 65535); directPort->setValue(8213);
    directForm->addRow(ui::make<QLabel>("Address", directGroup), directHost);
    directForm->addRow(ui::make<QLabel>("Port", directGroup), directPort);
    auto *connectProcessor = ui::make<QPushButton>("Connect processor", directGroup);
    connectProcessor->setObjectName("connectProcessor");
    directForm->addRow(connectProcessor);
    registry_layout->addWidget(directGroup);
    auto *discoveryGroup = ui::make<QGroupBox>("Discovery through registry", registry_dialog);
    auto *discoveryLayout = new QVBoxLayout(discoveryGroup);
    registry_layout->addWidget(discoveryGroup);
    const auto network = _application.state().file_configs.raw_document.value("aggregator").toObject();
    auto *host = new QLineEdit(network.value("host").toString("127.0.0.1"), registry_dialog);
    host->setObjectName("registryHost");
    auto *port = new QSpinBox(registry_dialog);
    port->setObjectName("registryPort");
    port->setRange(1, 65535);
    port->setValue(network.value("port").toInt(8212));
    auto *address_form = new QFormLayout;
    address_form->addRow(ui::make<QLabel>("Registry address", registry_dialog), host);
    address_form->addRow(ui::make<QLabel>("Registry port", registry_dialog), port);
    discoveryLayout->addLayout(address_form);
    auto *discover = ui::make<QPushButton>("Discover engines", registry_dialog);
    discover->setObjectName("discoverEngines");
    auto *disconnect = ui::make<QPushButton>("Disconnect registry", registry_dialog);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(discover);
    buttons->addWidget(disconnect);
    discoveryLayout->addLayout(buttons);
    auto *registry_status = new QLabel(registry_dialog);
    registry_status->setObjectName("engineRegistryStatus");
    registry_status->setWordWrap(true);
    registry_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    discoveryLayout->addWidget(registry_status);
    auto *warning = new QLabel(_application.client().network_warning(), registry_dialog);
    warning->setWordWrap(true);
    warning->setVisible(!warning->text().isEmpty());
    registry_layout->addWidget(warning);
    const auto refreshWarning = [this, warning] {
        warning->setText(_application.client().network_warning());
        warning->setVisible(!warning->text().isEmpty());
    };
    connect(&_application, &GuiPresenter::apply_status_changed, warning, refreshWarning);
    connect(&_application, &GuiPresenter::selected_device_changed, warning, refreshWarning);
    auto *registry = new AggregatorClient({QUuid::createUuid(), ComponentType::data_sink, 0}, this);
    registry->setObjectName("engineRegistryClient");
    const auto canonical = [](const QString &value) {
        const QUuid uuid(value);
        return uuid.isNull() ? value : uuid.toString(QUuid::WithoutBraces);
    };
    const auto refreshRegistry = [this, registry, devices, engineButton, permissions, registry_status, canonical, processorList] {
        const QSignalBlocker blocker(devices);
        int available = 0;
        const auto components = registry->components();
        for (auto it = components.cbegin(); it != components.cend(); ++it) {
            if (it.value().type != ComponentType::core || canonical(it.key().toString()) == canonical(_application.local_device_id()))
                continue;
            const auto endpoint = registry->resolve_engine(it.key());
            if (endpoint.isEmpty())
                continue;
            ++available;
            int row = -1;
            for (int i = 0; i < devices->count(); ++i)
                if (canonical(devices->itemData(i).toJsonObject().value("uuid").toString()) == canonical(it.key().toString()))
                    row = i;
            auto data = endpoint;
            data["from_registry"] = true;
            if (row < 0) {
                devices->addItem(data.value("name").toString(canonical(it.key().toString())), data);
            } else {
                // Preserve the identity used by the selected device's saved document.
                data["uuid"] = devices->itemData(row).toJsonObject().value("uuid");
                devices->setItemData(row, data);
            }
        }
        auto *model = qobject_cast<QStandardItemModel *>(devices->model());
        for (int i = 0; i < devices->count(); ++i) {
            auto data = devices->itemData(i).toJsonObject();
            if (!data.value("from_registry").toBool())
                continue;
            QString error;
            const bool reachable = !registry->resolve_engine(QUuid(data.value("uuid").toString()), &error).isEmpty();
            data["control_available"] = reachable;
            devices->setItemData(i, data);
            devices->setItemData(i, reachable ? (data.value("allow_mutations").toBool() ? ui::text("Editing allowed") : ui::text("Read-only engine")) : error, Qt::ToolTipRole);
            if (model && model->item(i))
                model->item(i)->setEnabled(reachable);
        }
        switch (registry->runtime_state()) {
        case core::RuntimeState::Stopped: registry_status->setText(ui::text("Registry disconnected. Local processing remains available.")); break;
        case core::RuntimeState::Starting: registry_status->setText(ui::text("Connecting to registry…")); break;
        case core::RuntimeState::Running:
            registry_status->setText(available ? ui::text("Available external processors: %1").arg(available) : ui::text("Registry connected; no external processors with a control endpoint.")); break;
        default: registry_status->setText(ui::text("Registry unavailable: %1")
            .arg(ui::text(registry->error().toUtf8().constData()))); break;
        }
        const auto selected = devices->currentData().toJsonObject();
        if (selected.value("from_registry").toBool() && !selected.value("control_available").toBool())
            permissions->setText(ui::text("Unavailable"));
        else
            permissions->setText(selected.contains("allow_mutations")
                ? (selected.value("allow_mutations").toBool() ? ui::text("Editing allowed") : ui::text("Read-only engine"))
                : QString{});
        processorList->clear();
        processorList->setHeaderLabels({ui::text("Processor"), ui::text("Endpoint"), ui::text("State")});
        for (int index = 0; index < devices->count(); ++index) {
            const auto data = devices->itemData(index).toJsonObject();
            const bool enabled = !model || model->item(index)->isEnabled();
            const auto endpoint = index == 0 ? ui::text("Inside this application")
                : data.value("address").toString() + ":" + QString::number(data.value("port").toInt());
            auto *row = new QTreeWidgetItem(processorList, {devices->itemText(index), endpoint,
                enabled ? ui::text("Available") : ui::text("Unavailable")});
            row->setData(0, Qt::UserRole, index);
            if (!enabled) row->setDisabled(true);
            if (index == devices->currentIndex()) processorList->setCurrentItem(row);
        }
        engineButton->setText(devices->currentText());
    };
    refreshRegistry();
    connect(discover, &QPushButton::clicked, this, [registry, host, port, registry_status] {
        const QHostAddress address(host->text().trimmed());
        if (address.isNull()) {
            registry_status->setText(ui::text("Registry address must be an IP address"));
            return;
        }
        // Discovery is a transient connection, not an implicit settings save.
        registry->connect_to_aggregator_server(address, quint16(port->value()));
    });
    connect(disconnect, &QPushButton::clicked, registry, &AggregatorClient::disconnect_from_aggregator_server);
    connect(registry, &AggregatorClient::status_changed, this, refreshRegistry);
    connect(registry, &AggregatorClient::capabilities_changed, this, refreshRegistry);
    connect(registry, &AggregatorClient::on_update_component_info, this, refreshRegistry);
    connect(registry, &AggregatorClient::on_update_component_state, this, refreshRegistry);
    connect(&ui::Language::instance(), &ui::Language::changed, this, [devices, refreshRegistry] {
        devices->setItemText(0, ui::text("In-app processor"));
        refreshRegistry();
    });
    auto *registry_action = findChild<QMenu *>("settingsMenu")->addAction(ui::text("Manage processors"));
    ui::bind(registry_action, "text", "Manage processors");
    registry_action->setObjectName("openEngineRegistry");
    const auto showProcessors = [registry_dialog] {
        registry_dialog->show(); registry_dialog->raise(); registry_dialog->activateWindow();
    };
    registry_action->setShortcut(QKeySequence("Alt+P"));
    connect(registry_action, &QAction::triggered, this, showProcessors);
    auto *engineActions = new QActionGroup(engineMenu);
    engineActions->setExclusive(true);
    connect(engineMenu, &QMenu::aboutToShow, this, [devices, engineMenu, engineActions, showProcessors] {
        engineMenu->clear();
        auto *model = qobject_cast<QStandardItemModel *>(devices->model());
        for (int index = 0; index < devices->count(); ++index) {
            auto *action = engineMenu->addAction(devices->itemText(index));
            action->setCheckable(true); action->setChecked(index == devices->currentIndex());
            action->setEnabled(!model || model->item(index)->isEnabled());
            engineActions->addAction(action);
            connect(action, &QAction::triggered, devices, [devices, index] {
                devices->setCurrentIndex(index);
                emit devices->activated(index);
            });
        }
        engineMenu->addSeparator();
        auto *manage = engineMenu->addAction(ui::text("Manage processors…"));
        connect(manage, &QAction::triggered, engineMenu, showProcessors);
    });
    connect(processorList, &QTreeWidget::itemActivated, devices, [devices](QTreeWidgetItem *row, int) {
        if (row->isDisabled()) return;
        const int index = row->data(0, Qt::UserRole).toInt();
        devices->setCurrentIndex(index); emit devices->activated(index);
    });
    connect(connectProcessor, &QPushButton::clicked, this, [devices, directHost, directPort, registry_status, refreshRegistry] {
        const QHostAddress address(directHost->text().trimmed());
        if (address.isNull()) { registry_status->setText(ui::text("Processor address must be an IP address")); return; }
        const auto host = address.toString(); const int port = directPort->value();
        const auto uuid = host + ":" + QString::number(port);
        int index = -1;
        for (int i = 0; i < devices->count(); ++i)
            if (devices->itemData(i).toJsonObject().value("uuid").toString() == uuid) index = i;
        if (index < 0) {
            devices->addItem(uuid, QJsonObject{{"uuid", uuid}, {"address", host}, {"port", port}});
            index = devices->count() - 1;
        }
        devices->setCurrentIndex(index); emit devices->activated(index); refreshRegistry();
    });
    connect(devices, &QComboBox::activated, this, [this, devices, registry, canonical](int index) {
        auto data = devices->itemData(index).toJsonObject();
        auto uuid = data.value("uuid").toString();
        if (canonical(uuid) == canonical(_application.local_device_id()))
            uuid = _application.local_device_id();
        if (canonical(uuid) == canonical(_application.device_id()))
            return;
        if (data.value("from_registry").toBool()) {
            QString error;
            const auto endpoint = registry->resolve_engine(QUuid(uuid), &error);
            if (endpoint.isEmpty()) {
                statusBar()->showMessage(ui::text("Registry unavailable: %1").arg(error), 5000);
                const QSignalBlocker blocker(devices);
                for (int i = 0; i < devices->count(); ++i)
                    if (canonical(devices->itemData(i).toJsonObject().value("uuid").toString()) == canonical(_application.device_id())) devices->setCurrentIndex(i);
                return;
            }
            for (auto it = endpoint.begin(); it != endpoint.end(); ++it) data[it.key()] = it.value();
        }
        const auto host = data.value("address").toString();
        const int port = data.value("port").toInt(0);
        if (uuid != _application.local_device_id() && (host.isEmpty() || port < 1 || port > 65535)) {
            statusBar()->showMessage(ui::text("Engine endpoint is unavailable"), 5000);
            return;
        }
        if (auto *editor = data_flow_editor()) {
            editor->flush_presentation();
            _application.state().file_configs.scene_graph = editor->document();
        }
        _application.select_device(uuid, host, quint16(port));
    });
    connect(&_application, &GuiPresenter::explicit_stop_requested, this, [this] {
        if (auto *editor = data_flow_editor()) editor->cancel_start_intent();
    });
    connect(&_application, &GuiPresenter::active_graph_changed, this, &MainWindow::sync_node_views);
    connect(&_application, &GuiPresenter::selected_device_changed, engineButton, refreshEngineCaption);
    connect(&_application, &GuiPresenter::node_views_ready, this, &MainWindow::consume_node_views);
    connect(&_application, &GuiPresenter::views_cleared, this, &MainWindow::clear_node_views);
    auto *apply_state = new QLabel(this);
    apply_state->setObjectName("graphApplyStatus");
    statusBar()->addPermanentWidget(apply_state);
    auto updateApplyStatus = [apply_state](const QString &text) {
        // Routine state is already represented by the editor and component icons.
        // Keep this area for actionable errors rather than transient success noise.
        const bool error = text.startsWith(ui::text("Not applied: ")) || text.startsWith("Error:");
        apply_state->setText(error ? QStringLiteral("⚠") : QString{});
        apply_state->setAccessibleName(text);
        apply_state->setToolTip(text);
        apply_state->setVisible(error);
    };
    updateApplyStatus(_application.apply_status());
    connect(&_application, &GuiPresenter::apply_status_changed, apply_state, updateApplyStatus);
    if (auto *editor = data_flow_editor())
        connect(editor, &SchemeEditorWidget::status_message, apply_state, updateApplyStatus);
    auto *themeSelector = new QComboBox(this);
    themeSelector->setObjectName("statusTheme");
    ui::items(themeSelector, {"System theme", "Light", "Dark"});
    const QStringList themeCodes{"system", "light", "dark"};
    for (int i = 0; i < themeCodes.size(); ++i)
        themeSelector->setItemData(i, themeCodes[i]);
    ui::bind(themeSelector, "toolTip", "Interface theme");
    themeSelector->setMaximumWidth(160);
    themeSelector->hide();
    auto *themeButton = ui::tool_button(deviceCorner,"Theme");
    themeButton->setObjectName("themeMenuButton");
    auto *themeMenu = new QMenu(themeButton);
    auto *themeActions = new QActionGroup(themeMenu);
    themeActions->setExclusive(true);
    for (int index = 0; index < themeCodes.size(); ++index) {
        auto *action = themeMenu->addAction(themeSelector->itemText(index));
        action->setCheckable(true); action->setData(index); themeActions->addAction(action);
        connect(action, &QAction::triggered, themeSelector, [themeSelector, index] { themeSelector->setCurrentIndex(index); });
    }
    connect(themeMenu, &QMenu::aboutToShow, themeMenu, [themeActions, themeSelector] {
        for (auto *action : themeActions->actions()) {
            const int index = action->data().toInt();
            action->setText(themeSelector->itemText(index));
            action->setChecked(themeSelector->currentIndex() == index);
        }
    });
    ui::button_menu(themeButton,themeMenu);
    deviceLayout->insertWidget(2,themeButton);
    auto syncTheme = [this, themeSelector, themeCodes] {
        const auto code = ui::Theme::instance().code();
        QSignalBlocker block(themeSelector);
        themeSelector->setCurrentIndex(std::max(0, int(themeCodes.indexOf(code))));
        if (auto *settingsTheme = findChild<QComboBox *>("interfaceTheme")) {
            QSignalBlocker settingsBlock(settingsTheme);
            settingsTheme->setCurrentIndex(QStringList{"system", "dark", "light"}.indexOf(code));
        }
    };
    syncTheme();
    connect(&ui::Theme::instance(), &ui::Theme::changed, this, syncTheme);
    connect(themeSelector, &QComboBox::currentIndexChanged, this, [this, themeCodes](int index) {
        if (index < 0 || index >= themeCodes.size())
            return;
        if (ui::Theme::instance().set(themeCodes[index])) {
            auto &ui = _application.state().file_configs.ui;
            ui.theme = themeCodes[index];
            if (ui.autosave_enabled)
                emit _application.on_config_updated();
        }
    });
    auto *saved_warning = new QLabel(this);
    saved_warning->setObjectName("savedPipelineWarning");
    saved_warning->setWordWrap(true);
    saved_warning->setVisible(false);
    statusBar()->addPermanentWidget(saved_warning, 1);
    connect(&_application, &GuiPresenter::saved_graph_warning_changed, saved_warning,
            [saved_warning](const QString &warning) {
                saved_warning->setText(warning);
                saved_warning->setVisible(!warning.isEmpty());
            });
    connect(&_application, &GuiPresenter::command_rejected, this, [this](quint64, const QString &error) {
        statusBar()->showMessage(ui::text("Engine command rejected: %1").arg(error), 10000);
    });
    connect(&_application, &GuiPresenter::sample_rate_changed, this, &MainWindow::on_sample_rate_changed);

    auto *fps_timer = new QTimer(this);
    fps_timer->setInterval(500);
    auto refresh_fps = [this] {
        if (!_application.state().file_configs.ui.show_fps)
            return;
        for (auto *dock:workspaceDocks()) if(is_graph_dock(dock) && !dock->property("renderExposed").toBool()) dock->setRenderFPS(0,true);
        const std::pair<const char *, AbstractChartView *> charts[]{{"rawSamples", _raw_signal_chart},
                                                                    {"windowSignal", _window_signal_chart},
                                                                    {"windowedSignal", _windowed_signal_chart},
                                                                    {"windowFunction", _window_function_chart},
                                                                    {"fft", _amplitudes_chart},
                                                                    {"mel", _test_amplitudes_chart}};
        for (auto [id, chart] : charts)
            if (_render_visible.value(id) && chart->isVisible())
                if (auto *view = dynamic_cast<FPSChartView *>(chart))
                    _panels[id]->setRenderFPS(view->display_fps(),true);
        if (_render_visible.value("spectrogram") && _spectrogram_view->isVisible())
            _panels["spectrogram"]->setRenderFPS(_spectrogram_view->display_fps(),true);
        for (auto it = _node_views.cbegin(); it != _node_views.cend(); ++it) {
            const auto &view = it.value();
            if (!view.dock || !_render_visible.value(it.key())) continue;
            if (view.spectrogram && view.spectrogram->isVisible())
                view.dock->setRenderFPS(view.spectrogram->display_fps(),true);
            else if (auto *chart = dynamic_cast<FPSChartView *>(view.chart); chart && chart->isVisible())
                view.dock->setRenderFPS(chart->display_fps(),true);
        }
    };
    connect(fps_timer, &QTimer::timeout, this, refresh_fps);
    for (auto *chart : {_raw_signal_chart, _window_signal_chart, _windowed_signal_chart, _window_function_chart,
                        _amplitudes_chart, _test_amplitudes_chart})
        if (auto *view = dynamic_cast<FPSChartView *>(chart))
            connect(view, &FPSChartView::fps_changed, this, refresh_fps);
    auto refresh_preferences = [this, refresh_fps] {
        apply_render_rate();
        if (auto *editor = data_flow_editor()) editor->sync_preferences();
        if (!_application.state().file_configs.ui.show_fps) {
            for(auto *dock:workspaceDocks()) dock->setRenderFPS(0,false);
        } else
            refresh_fps();
    };
    refresh_preferences();
    connect(findChild<SettingsWidget *>(), &SettingsWidget::presentation_preferences_changed, this,
            refresh_preferences);
    fps_timer->start();
    const auto watchScreen = [this](QScreen *display) {
        connect(display, &QScreen::refreshRateChanged, this, [this] { apply_render_rate(); });
    };
    for (auto *display : QGuiApplication::screens()) watchScreen(display);
    connect(qApp, &QGuiApplication::screenAdded, this, watchScreen);
    QTimer::singleShot(0, this, [this] {
        if (windowHandle())
            connect(windowHandle(), &QWindow::screenChanged, this, [this] { apply_render_rate(); });
        apply_render_rate();
    });
    statusBar()->setSizeGripEnabled(false);
    auto refreshStatusVisibility=[this,apply_state,saved_warning] {
        statusBar()->setVisible(!statusBar()->currentMessage().isEmpty() || !apply_state->text().isEmpty() || !saved_warning->text().isEmpty());
    };
    refreshStatusVisibility();
    connect(statusBar(),&QStatusBar::messageChanged,this,refreshStatusVisibility);
    connect(&_application,&GuiPresenter::apply_status_changed,this,refreshStatusVisibility);
    connect(&_application,&GuiPresenter::saved_graph_warning_changed,this,refreshStatusVisibility);
}

MainWindow::~MainWindow() {
    disconnect(workspace(),nullptr,this,nullptr);
    qApp->removeEventFilter(this);
    _restoring_layout = true;
    _workspace_save_timer.stop();
    _camera_save_timer.stop();
    for (auto *panel : _panels) {
        disconnect(panel, nullptr, this, nullptr);
        disconnect(panel->toggleViewAction(), nullptr, this, nullptr);
    }
    // Floating children close during QWidget teardown, after our maps have
    // already been destroyed. Their visibility callbacks must stop here.
    for(const auto &view:_node_views) disconnect(view.dock,nullptr,this,nullptr);
    if (auto *editor = data_flow_editor())
        editor->flush_presentation();
    if (!_workspace_closed) {
        save_workspace();
        if (_application.state().file_configs.ui.autosave_enabled)
            emit _application.on_config_updated();
    }
}

void MainWindow::setup_ui() {
    auto *file_menu = menuBar()->addMenu(ui::text("File"));
    file_menu->setObjectName("fileMenu");
    ui::bind(file_menu, "title", "File");
    auto *saveAction = new QAction(this);
    addAction(saveAction);
    ui::bind(saveAction, "text", "Save settings");
    saveAction->setObjectName("saveSettings");
    saveAction->setShortcut(QKeySequence("Ctrl+S"));
    saveAction->setShortcutContext(Qt::ApplicationShortcut);
    saveAction->setShortcutVisibleInContextMenu(true);

    connect(saveAction, &QAction::triggered, this, [this]() {
        if (auto *editor = data_flow_editor())
            editor->flush_presentation();
        save_workspace();
        _application.state().editor_draft_modified = true;
        emit on_config_updated();
    });
    file_menu->addAction(saveAction);
    auto *saveAs = file_menu->addAction(ui::text("Save settings as…"));
    saveAs->setObjectName("saveSettingsAs");
    saveAs->setShortcut(QKeySequence::SaveAs);
    saveAs->setShortcutContext(Qt::ApplicationShortcut);
    saveAs->setShortcutVisibleInContextMenu(true);
    ui::bind(saveAs, "text", "Save settings as…");
    connect(saveAs, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, ui::text("Save settings as…"), {},
                                                     ui::text("Settings (*.json)"));
        if (path.isEmpty()) return;
        if (auto *editor = data_flow_editor()) editor->flush_presentation();
        save_workspace();
        _application.state().editor_draft_modified = true;
        emit save_configuration_as_requested(path);
    });
    auto *open = file_menu->addAction(ui::text("Open settings…"));
    open->setObjectName("openSettingsFile");
    open->setShortcut(QKeySequence::Open);
    open->setShortcutContext(Qt::ApplicationShortcut);
    open->setShortcutVisibleInContextMenu(true);
    ui::bind(open, "text", "Open settings…");
    ui::bind(open, "toolTip", "Open a settings file and restart the application");
    connect(open, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, ui::text("Open settings…"), {},
                                                     ui::text("Settings (*.json)"));
        if (!path.isEmpty()) emit open_configuration_requested(path);
    });

    // Native 3D windows must be reparented synchronously when rebuilding the layout.

    // Создаем все графики один раз

    // Raw Samples
    _raw_signal_chart = new LineChartView();
    _raw_signal_chart->chart()->setTitle(QString());
    _raw_signal_chart->set_range(-1, 1);
    _raw_signal_chart->set_auto_resizing(true);
    _raw_signal_chart->set_auto_gain(true, 1, -1);
    add_panel("rawSamples", "Raw Samples", _raw_signal_chart, &UIApplicationConfig::show_raw_samples);

    // Window signal before weighting
    _window_signal_chart = new LineChartView();
    _window_signal_chart->chart()->setTitle(QString());
    _window_signal_chart->set_range(-1, 1);
    _window_signal_chart->set_auto_resizing(true);
    _window_signal_chart->set_auto_gain(true, 1, -1);
    add_panel("windowSignal", "Window signal", _window_signal_chart, &UIApplicationConfig::show_window_signal);

    // Window signal after applying the window function
    _windowed_signal_chart = new LineChartView();
    _windowed_signal_chart->chart()->setTitle(QString());
    _windowed_signal_chart->set_range(-1, 1);
    _windowed_signal_chart->set_auto_resizing(true);
    _windowed_signal_chart->set_auto_gain(true, 1, -1);
    add_panel("windowedSignal", "Windowed signal", _windowed_signal_chart, &UIApplicationConfig::show_windowed_signal);

    //  window function
    _window_function_chart = new LineChartView();
    _window_function_chart->chart()->setTitle(QString());
    _window_function_chart->set_range(-0.2, 1.2);
    _window_function_chart->update(_application.window());
    add_panel("windowFunction", "Window function", _window_function_chart, &UIApplicationConfig::show_window_function);

    // Amplitudes (FFT)
    _amplitudes_chart = new FreqChartView();
    _amplitudes_chart->chart()->setTitle(QString());
    _amplitudes_chart->set_range(0, 1);
    _amplitudes_chart->set_auto_resizing(true);
    _amplitudes_chart->set_auto_gain(true, 1, 0);

    add_panel("fft", "FFT spectrum", _amplitudes_chart, &UIApplicationConfig::show_amplitudes);

    // Test Amplitudes (Mel)
    _test_amplitudes_chart = new LineChartView();
    _test_amplitudes_chart->chart()->setTitle(QString());
    _test_amplitudes_chart->set_range(0, 1);
    _test_amplitudes_chart->set_auto_resizing(true);
    _test_amplitudes_chart->set_auto_gain(true, 1, 0);
    add_panel("mel", "Mel spectrum", _test_amplitudes_chart, &UIApplicationConfig::show_test_amplitudes);

    // 3D Spectrogram
    _spectrogram_view = new SpectrogramGraph(this);
    connect_surface_status(_spectrogram_view, "spectrogram");
    _spectrogram_view->set_presentation_state(
        _application.state().file_configs.ui.graph_views.value("spectrogram").toObject().value("renderer").toObject());
    _spectrogram_view->set_camera_state(_application.state().file_configs.ui.camera);
    _spectrogram_view->set_camera_controls(_application.state().file_configs.ui.camera_controls);
    _spectrogram_view->set_side_panel_ratio(_application.state().file_configs.ui.spectrogram_side_panel_ratio);
    _camera_save_timer.setSingleShot(true);
    _camera_save_timer.setInterval(500);
    connect(&_camera_save_timer, &QTimer::timeout, this, [this] {
        if (_application.state().file_configs.ui.autosave_enabled)
            emit on_config_updated();
    });
    connect(_spectrogram_view, &SpectrogramGraph::presentation_changed, this, [this](const QJsonObject &state) {
        auto prefs = _application.state().file_configs.ui.graph_views.value("spectrogram").toObject();
        prefs["renderer"] = state;
        _application.state().file_configs.ui.graph_views["spectrogram"] = prefs;
        _camera_save_timer.start();
    });
    connect(_spectrogram_view, &SpectrogramGraph::camera_changed, this, [this](const QJsonObject &state) {
        _application.state().file_configs.ui.camera = state;
        _camera_save_timer.start();
    });
    connect(_spectrogram_view, &SpectrogramGraph::side_panel_resized, this, [this](float ratio) {
        _application.state().file_configs.ui.spectrogram_side_panel_ratio = ratio;
        _camera_save_timer.start();
    });
    add_panel("spectrogram", "Spectrogram", _spectrogram_view, &UIApplicationConfig::show_spectrogram);

    // Settings
    auto *settings_dialog = new QDialog(this);
    settings_dialog->setObjectName("settingsDialog");
    ui::bind(settings_dialog, "windowTitle", "Settings");
    settings_dialog->resize(680, 660);
    settings_dialog->setMinimumSize(560, 420);
    auto *settings_layout = new QVBoxLayout(settings_dialog);
    const auto settings_widget = new SettingsWidget(_application.state(), settings_dialog);
    settings_layout->addWidget(settings_widget);
    auto *dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Close, settings_dialog);
    ui::bind(dialog_buttons->button(QDialogButtonBox::Close), "text", "Close");
    settings_layout->addWidget(dialog_buttons);
    connect(dialog_buttons, &QDialogButtonBox::rejected, settings_dialog, &QDialog::hide);
    auto *settings_menu = new QMenu(this);
    settings_menu->setObjectName("settingsMenu");
    ui::bind(settings_menu, "title", "Settings");
    const char *sections[]{"General", "Audio processing", "Graphs", "WLED", "Network", "Controls"};
    for (int i : {0, 1, 2, 4, 5}) {
        auto *action = settings_menu->addAction(ui::text(sections[i]));
        ui::bind(action, "text", sections[i]);
        action->setObjectName(i == 0 ? "openSettings" : QString("openSettingsSection%1").arg(i));
        action->setShortcut(i == 0 ? QKeySequence("Alt+,") : QKeySequence(QString("Alt+%1").arg(i + 1)));
        connect(action, &QAction::triggered, this, [settings_dialog, settings_widget, i] {
            settings_widget->select_section(i);
            settings_dialog->show();
            settings_dialog->raise();
            settings_dialog->activateWindow();
        });
    }
    connect(settings_widget, &SettingsWidget::camera_controls_changed, this,
            [this] {
                const auto controls = _application.state().file_configs.ui.camera_controls;
                _spectrogram_view->set_camera_controls(controls);
                for (auto &view : _node_views) if (view.spectrogram) view.spectrogram->set_camera_controls(controls);
                if (auto *editor = data_flow_editor())
                    editor->set_navigation_speed(CameraControls::movementSpeed(controls));
            });
    connect(settings_widget, &SettingsWidget::reset_camera_requested, _spectrogram_view,
            &SpectrogramGraph::reset_camera);
    connect(settings_widget, &SettingsWidget::surface_settings_changed, this, &MainWindow::apply_surface_settings);
    connect(settings_widget, &SettingsWidget::on_config_changed, &_application, &GuiPresenter::on_config_updated);
    connect(settings_widget, &SettingsWidget::on_rebuild_charts, this, &MainWindow::on_update_chart_visibility);
    connect(settings_widget, &SettingsWidget::start_wled_server_requested, this,
            &MainWindow::start_wled_server_requested);
    connect(settings_widget, &SettingsWidget::stop_wled_server_requested, this,
            &MainWindow::stop_wled_server_requested);
    connect(settings_widget, &SettingsWidget::wled_server_config_changed, this,
            &MainWindow::wled_server_config_changed);
    for (auto *dock : _panels) if (auto *panel=qobject_cast<GraphPanel *>(dock->widget())) {
        auto refresh = [this, panel] {
            panel->set_processing_choices(_application.state().file_configs.core.spectrum_channel,
                                          static_cast<int>(_application.state().file_configs.core.spectrum_merge));
        };
        refresh();
        connect(&_application, &GuiPresenter::processing_state_changed, panel, refresh);
        connect(panel, &GraphPanel::channel_requested, &_application, &GuiPresenter::set_spectrum_channel);
        if (panel->objectName() == "fftGraphPanel")
            connect(panel, &GraphPanel::merge_requested, this,
                    [this](int merge) { _application.set_node_parameters(5, {{"merge", merge}}); });
        else
            connect(panel, &GraphPanel::merge_requested, &_application, &GuiPresenter::set_spectrum_merge);
    }
    auto *editor = new SchemeEditorWidget(_application, this);
    connect(editor, &SchemeEditorWidget::status_message, this, [this](const QString &text) {
        if (text != ui::text("Data Flow applied"))
            statusBar()->showMessage(text, 5000);
    });
    auto *pipeline_panel = add_panel("dataFlow", "Data Flow", editor, &UIApplicationConfig::show_data_flow);
    setup_graph_dock_controls(pipeline_panel);
    connect(settings_widget,&SettingsWidget::workspace_position_changed,this,[this] {
        const bool restoring=_restoring_layout; _restoring_layout=true;
        rebuild_graph_areas(nullptr,nullptr,-1);

        _restoring_layout=restoring; refresh_subscriptions(); _workspace_save_timer.start();
    });
    auto *pipeline_action = pipeline_panel->toggleViewAction();
    pipeline_action->setObjectName("openPipeline");
    pipeline_action->setShortcut(QKeySequence("Alt+D"));
    auto *pipeline_toggle = ui::tool_button(this);
    pipeline_toggle->setObjectName("pipelineToggle");
    pipeline_toggle->setDefaultAction(pipeline_action);
    editor->set_navigation_speed(CameraControls::movementSpeed(_application.state().file_configs.ui.camera_controls));
    connect(pipeline_action, &QAction::triggered, this, [pipeline_panel](bool visible) {
        if (visible)
            pipeline_panel->raise();
    });
    connect(editor,&SchemeEditorWidget::graph_view_requested,this,&MainWindow::open_graph_node);
    connect(editor,&SchemeEditorWidget::graph_open_requested,this,&MainWindow::add_graph_node);
    editor->set_graph_menu_provider([this](int node,QMenu *menu) { populate_graph_menu(node,menu); });
    if(auto *presets=editor->findChild<QToolButton *>("pipelinePresets")) {
        presets->hide(); // Added to the shared upper navigation bar by the constructor.
    }
    auto *view_menu = menuBar()->addMenu(ui::text("Display"));
    view_menu->setObjectName("graphsMenu");
    ui::bind(view_menu, "title", "Display");
    view_menu->addAction(pipeline_action);
    view_menu->addSeparator();
    auto *reset = view_menu->addAction(ui::text("Reset layout"));
    ui::bind(reset, "text", "Reset layout");
    reset->setObjectName("resetWorkspace");
    reset->setShortcut(QKeySequence("Alt+L"));
    ui::bind(reset,"toolTip","Restore the default workspace layout");
    connect(reset, &QAction::triggered, this, [this] {
        reset_workspace();
        _workspace_save_timer.start();
    });
    auto *resetCamera=view_menu->addAction(ui::text("Reset camera"));
    resetCamera->setObjectName("resetCameraView");ui::bind(resetCamera,"text","Reset camera");
    resetCamera->setShortcut(QKeySequence("Alt+R"));
    connect(resetCamera,&QAction::triggered,this,[this] {
        for(const auto &key:_active_node_views) {
            auto *surface=key=="spectrogram" ? _spectrogram_view : _node_views.value(key).spectrogram;
            if(surface && surface->isVisible()) {surface->reset_camera();return;}
        }
    });
    view_menu->addSeparator();
    if(auto *arrange=editor->findChild<QAction *>("arrangePipelineShortcut")) view_menu->addAction(arrange);
    if(auto *center=editor->findChild<QAction *>("fitPipelineShortcut")) view_menu->addAction(center);
    if(auto *presets=editor->presets_button()) view_menu->addMenu(presets->menu());
    auto *wled_menu = menuBar()->addMenu("WLED");
    wled_menu->setObjectName("wledMenu");
    auto *wled_action = wled_menu->addAction("WLED");
    wled_action->setObjectName("openSettingsSection3");
    wled_action->setShortcut(QKeySequence("Alt+4"));
    connect(wled_action, &QAction::triggered, this, [settings_dialog, settings_widget] {
        settings_widget->select_section(SettingsWidget::WLED);
        settings_dialog->show();
        settings_dialog->raise();
        settings_dialog->activateWindow();
    });
    menuBar()->addMenu(settings_menu);
    _workspace_save_timer.setSingleShot(true);
    _workspace_save_timer.setInterval(350);
    connect(&_workspace_save_timer, &QTimer::timeout, this, [this] {
        save_workspace();
        if (_application.state().file_configs.ui.autosave_enabled)
            emit _application.on_config_updated();
    });
    const auto saved_ui = _application.state().file_configs.ui;
    const auto workspace = saved_ui.workspace;
    reset_workspace();
    restore_node_views();
    bool restored_workspace = false;
    if (workspace.value("version").toInt() == 1) {
        const auto geometry = QByteArray::fromBase64(workspace.value("geometry").toString().toLatin1());
        const auto state = QByteArray::fromBase64(workspace.value(workspace.contains("ads_state") ? "ads_state" : "state").toString().toLatin1());
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
        const int layout_version = workspace.value("layout_version").toInt();
        if ((layout_version >= 2 && layout_version <= 5) && !state.isEmpty() &&
            (workspace.contains("ads_state") ? restoreState(state, 1) : importLegacyState(state, 1, layout_version==2))) {
            restored_workspace = true;
            _application.state().file_configs.ui = saved_ui;
            const auto visibility = workspace.value("visibility").toObject();
            for (auto it = _panels.begin(); it != _panels.end(); ++it)
                if (layout_version >= 4 && visibility.value(it.key()).isBool())
                    it.value()->toggleView(visibility.value(it.key()).toBool());
            for (auto it = _node_views.begin(); it != _node_views.end(); ++it) {
                const auto identity = "nodeView_" + it.key();
                if (layout_version >= 4 && visibility.value(identity).isBool())
                    it.value().dock->toggleView(visibility.value(identity).toBool());
                auto p = _application.state().file_configs.ui.graph_views[it.key()].toObject();
                p["open"] = !it.value().dock->isHidden();
                _application.state().file_configs.ui.graph_views[it.key()] = p;
            }
            // The Qt dock state is authoritative for older workspaces. Legacy
            // flags may describe parent/tab visibility rather than an open dock.
            auto &ui = _application.state().file_configs.ui;
            ui.show_spectrogram = !_panels["spectrogram"]->isHidden();
            ui.show_data_flow = !_panels["dataFlow"]->isHidden();
            ui.show_amplitudes = !_panels["fft"]->isHidden();
            ui.show_test_amplitudes = !_panels["mel"]->isHidden();
            ui.show_raw_samples = !_panels["rawSamples"]->isHidden();
            ui.show_window_signal = !_panels["windowSignal"]->isHidden();
            ui.show_windowed_signal = !_panels["windowedSignal"]->isHidden();
            ui.show_window_function = !_panels["windowFunction"]->isHidden();
        }

    }
    if (!restored_workspace) {
        // A damaged serialized layout can partly mutate docks before failing.
        reset_workspace();
    }
    _restoring_layout = false;
    QTimer::singleShot(0, this, [this, restored_workspace] {
        recover_offscreen_panels();
        if (!restored_workspace && !_panels["spectrogram"]->isHidden() && !_panels["dataFlow"]->isHidden())
            resizeDocks({_panels["spectrogram"], _panels["dataFlow"]}, {560, 320}, Qt::Vertical);
        refresh_subscriptions();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { recover_offscreen_panels(); });

    // Инициализируем видимость графиков на основе текущего конфига
    update_chart_visibility(_application.state().file_configs.ui);

    qCInfo(core_app_window, "UI setup completed");
}

