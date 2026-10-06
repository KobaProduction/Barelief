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
    qApp->installEventFilter(this);
    connect(this,&QMainWindow::tabifiedDockWidgetActivated,this,[this](QDockWidget *dock) {
        if (!_restoring_layout && is_graph_dock(dock)) _last_graph_slot=dock;
    });
    auto *devices = new QComboBox(this);
    devices->setObjectName("engineDeviceSelector");
    devices->addItem(ui::text("In-app processor"), QJsonObject{{"uuid", _application.local_device_id()}});
    ui::bind(devices,"toolTip","Processes audio inside this application. External processors connect over TCP.");
    auto *deviceCorner = new QWidget(menuBar());
    auto *deviceLayout = new QHBoxLayout(deviceCorner);
    deviceLayout->setContentsMargins(8, 4, 8, 4);
    devices->hide();
    auto *engineButton = new QToolButton(deviceCorner);
    engineButton->setObjectName("engineSelectorButton");
    engineButton->setMaximumWidth(260);
    engineButton->setPopupMode(QToolButton::InstantPopup);
    auto *engineMenu = new QMenu(engineButton);
    engineButton->setMenu(engineMenu);
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
        if (auto *editor = findChild<SchemeEditorWidget *>()) {
            editor->flush_presentation();
            _application.state().file_configs.scene_graph = editor->document();
        }
        _application.select_device(uuid, host, quint16(port));
    });
    connect(&_application, &GuiPresenter::explicit_stop_requested, this, [this] {
        if (auto *editor = findChild<SchemeEditorWidget *>()) editor->cancel_start_intent();
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
    if (auto *editor = findChild<SchemeEditorWidget *>())
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
    auto *themeButton = new QToolButton(this);
    themeButton->setObjectName("themeMenuButton");
    ui::bind(themeButton, "text", "Theme");
    themeButton->setPopupMode(QToolButton::InstantPopup);
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
    themeButton->setMenu(themeMenu);
    statusBar()->addPermanentWidget(themeButton);
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

    auto *fps = new QLabel(this);
    fps->setObjectName("renderStatus");
    fps->setContentsMargins(8, 0, 8, 0);
    statusBar()->addPermanentWidget(fps);
    auto *fps_timer = new QTimer(this);
    fps_timer->setInterval(500);
    auto refresh_fps = [this, fps] {
        if (!_application.state().file_configs.ui.show_fps)
            return;
        QStringList rates;
        const std::pair<const char *, AbstractChartView *> charts[]{{"rawSamples", _raw_signal_chart},
                                                                    {"windowSignal", _window_signal_chart},
                                                                    {"windowedSignal", _windowed_signal_chart},
                                                                    {"windowFunction", _window_function_chart},
                                                                    {"fft", _amplitudes_chart},
                                                                    {"mel", _test_amplitudes_chart}};
        for (auto [id, chart] : charts)
            if (_render_visible.value(id) && chart->isVisible())
                if (auto *view = dynamic_cast<FPSChartView *>(chart))
                    rates << ui::text("%1: %2 FPS")
                                     .arg(_panels[id]->windowTitle())
                                     .arg(view->display_fps(), 0, 'f', 0);
        if (_render_visible.value("spectrogram") && _spectrogram_view->isVisible())
            rates << ui::text("%1 FPS").arg(_spectrogram_view->display_fps(), 0, 'f', 0);
        for (auto it = _node_views.cbegin(); it != _node_views.cend(); ++it) {
            const auto &view = it.value();
            if (!view.dock || !_render_visible.value(it.key())) continue;
            if (view.spectrogram && view.spectrogram->isVisible())
                rates << ui::text("%1: %2 FPS").arg(view.dock->windowTitle()).arg(view.spectrogram->display_fps(), 0, 'f', 0);
            else if (auto *chart = dynamic_cast<FPSChartView *>(view.chart); chart && chart->isVisible())
                rates << ui::text("%1: %2 FPS").arg(view.dock->windowTitle()).arg(chart->display_fps(), 0, 'f', 0);
        }
        fps->setText(rates.isEmpty() ? ui::text("%1 FPS").arg(0) : rates.join(" · "));
        fps->setVisible(true);
    };
    connect(fps_timer, &QTimer::timeout, this, refresh_fps);
    for (auto *chart : {_raw_signal_chart, _window_signal_chart, _windowed_signal_chart, _window_function_chart,
                        _amplitudes_chart, _test_amplitudes_chart})
        if (auto *view = dynamic_cast<FPSChartView *>(chart))
            connect(view, &FPSChartView::fps_changed, this, refresh_fps);
    auto refresh_preferences = [this, fps, refresh_fps] {
        apply_render_rate();
        if (auto *editor = findChild<SchemeEditorWidget *>()) editor->sync_preferences();
        if (!_application.state().file_configs.ui.show_fps) {
            fps->clear();
            fps->hide();
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
        auto *theme = findChild<QToolButton *>("themeMenuButton");
        auto *flow = findChild<QToolButton *>("pipelineToggle");
        if (theme && flow) {
            const int height = std::max(theme->sizeHint().height(), flow->sizeHint().height());
            theme->setFixedHeight(height);
            flow->setFixedHeight(height);
        }
    });
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
    _restoring_layout = true;
    _workspace_save_timer.stop();
    _camera_save_timer.stop();
    for (auto *panel : _panels) {
        disconnect(panel, nullptr, this, nullptr);
        disconnect(panel->toggleViewAction(), nullptr, this, nullptr);
    }
    if (auto *editor = findChild<SchemeEditorWidget *>())
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
        if (auto *editor = findChild<SchemeEditorWidget *>())
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
        if (auto *editor = findChild<SchemeEditorWidget *>()) editor->flush_presentation();
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
    setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);

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
                if (auto *editor = findChild<SchemeEditorWidget *>())
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
    for (auto *panel : findChildren<GraphPanel *>()) {
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
    auto *pipeline_action = pipeline_panel->toggleViewAction();
    pipeline_action->setObjectName("openPipeline");
    pipeline_action->setShortcut(QKeySequence("Alt+D"));
    auto *pipeline_toggle = new QToolButton(this);
    pipeline_toggle->setObjectName("pipelineToggle");
    pipeline_toggle->setDefaultAction(pipeline_action);
    editor->set_navigation_speed(CameraControls::movementSpeed(_application.state().file_configs.ui.camera_controls));
    statusBar()->addPermanentWidget(pipeline_toggle);
    connect(pipeline_action, &QAction::triggered, this, [pipeline_panel](bool visible) {
        if (visible)
            pipeline_panel->raise();
    });
    connect(editor,&SchemeEditorWidget::graph_view_requested,this,&MainWindow::open_graph_node);
    connect(editor,&SchemeEditorWidget::graph_open_requested,this,&MainWindow::add_graph_node);
    if(auto *presets=editor->findChild<QToolButton *>("pipelinePresets")) {
        statusBar()->addWidget(presets);
        presets->show();
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
        const auto state = QByteArray::fromBase64(workspace.value("state").toString().toLatin1());
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
        const int layout_version = workspace.value("layout_version").toInt();
        // Qt serializes dock object names. Restore legacy names temporarily, then
        // keep canonical names for all subsequent saves and runtime lookups.
        const QMap<QString, QString> legacy_names{
            {"spectrogram", "surfaceDock"}, {"windowSignal", "samplesDock"}, {"windowedSignal", "windowedSamplesDock"}};
        if (layout_version == 2)
            for (auto it = legacy_names.cbegin(); it != legacy_names.cend(); ++it)
                _panels[it.key()]->setObjectName(it.value());
        if ((layout_version == 2 || layout_version == 3 || layout_version == 4) && !state.isEmpty() &&
            restoreState(state, 1)) {
            restored_workspace = true;
            _application.state().file_configs.ui = saved_ui;
            const auto visibility = workspace.value("visibility").toObject();
            for (auto it = _panels.begin(); it != _panels.end(); ++it)
                if (layout_version == 4 && visibility.value(it.key()).isBool())
                    it.value()->setVisible(visibility.value(it.key()).toBool());
            for (auto it = _node_views.begin(); it != _node_views.end(); ++it) {
                const auto identity = "nodeView_" + it.key();
                if (layout_version == 4 && visibility.value(identity).isBool())
                    it.value().dock->setVisible(visibility.value(identity).toBool());
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
        for (auto it = legacy_names.cbegin(); it != legacy_names.cend(); ++it)
            _panels[it.key()]->setObjectName(it.key() + "Dock");
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

void MainWindow::update_chart_visibility(const UIApplicationConfig &config) {
    _application.update_subscription();
    if (config.show_spectrogram) {
        const auto size = screen()->availableGeometry().size();
        _spectrogram_view->initialize(QSize(size.width() / 2, size.height() / 2), size);
    }
    const bool restoring = _restoring_layout;
    _restoring_layout = true;
    _panels["rawSamples"]->setVisible(config.show_raw_samples);
    _panels["windowSignal"]->setVisible(config.show_window_signal);
    _panels["windowedSignal"]->setVisible(config.show_windowed_signal);
    _panels["windowFunction"]->setVisible(config.show_window_function);
    _panels["fft"]->setVisible(config.show_amplitudes);
    _panels["mel"]->setVisible(config.show_test_amplitudes);
    _panels["spectrogram"]->setVisible(config.show_spectrogram);
    _panels["dataFlow"]->setVisible(config.show_data_flow);
    const std::pair<const char *, bool> controls[]{{"showRawSamples", config.show_raw_samples},
                                                   {"showWindowSignal", config.show_window_signal},
                                                   {"showWindowedSignal", config.show_windowed_signal},
                                                   {"showWindowFunction", config.show_window_function},
                                                   {"showFFT", config.show_amplitudes},
                                                   {"showMel", config.show_test_amplitudes},
                                                   {"showSpectrogram", config.show_spectrogram}};
    for (const auto &[name, visible] : controls) {
        if (auto *checkbox = findChild<QCheckBox *>(name)) {
            const QSignalBlocker blocker(checkbox);
            checkbox->setChecked(visible);
        }
        if (auto *action = findChild<QAction *>("toggle_" + QString::fromLatin1(name))) {
            const QSignalBlocker blocker(action);
            action->setChecked(visible);
        }
    }
    _restoring_layout = restoring;
    refresh_subscriptions();

    qCDebug(core_app_window, "Chart visibility updated");
}

void MainWindow::on_update_chart_visibility() {
    update_chart_visibility(_application.state().file_configs.ui);
}

void MainWindow::on_raw_signal_ready(const std::vector<float> &data) {
    if (_raw_signal_chart && _render_visible.value("rawSamples") && _raw_signal_chart->isVisible()) {
        _raw_signal_chart->update(data);
    }
}

void MainWindow::on_window_signal_ready(const std::vector<float> &data) {
    if (_window_signal_chart && _render_visible.value("windowSignal") && _window_signal_chart->isVisible()) {
        _window_signal_chart->update(data);
    }
}

void MainWindow::on_windowed_signal_ready(const std::vector<float> &data) {
    if (_windowed_signal_chart && _render_visible.value("windowedSignal") && _windowed_signal_chart->isVisible()) {
        _windowed_signal_chart->update(data);
    }
}

void MainWindow::on_window_function_ready(const std::vector<float> &data) {
    if (_window_function_chart && _render_visible.value("windowFunction") && _window_function_chart->isVisible()) {
        _window_function_chart->update(data);
    }
}

void MainWindow::on_amplitudes_ready(const std::vector<float> &data) {
    if (_amplitudes_chart && _render_visible.value("fft") && _amplitudes_chart->isVisible()) {
        static_cast<FreqChartView *>(_amplitudes_chart)->update_freq_step(_application.frequency_step());
        _amplitudes_chart->update(data);
    }
}

void MainWindow::on_test_amplitudes_ready(const std::vector<float> &data) {
    if (_test_amplitudes_chart && _render_visible.value("mel") && _test_amplitudes_chart->isVisible()) {
        _test_amplitudes_chart->update(data);
    }
}

void MainWindow::on_spectrogram_updated(const std::vector<std::vector<float>> &data) const {
    if (_spectrogram_view && _render_visible.value("spectrogram") && _spectrogram_view->isVisible()) {
        _spectrogram_view->update(data);
    }
}

void MainWindow::on_sample_rate_changed(uint32_t sample_rate) {
    if (_amplitudes_chart) {
        auto *freq_chart = static_cast<FreqChartView *>(_amplitudes_chart);
        freq_chart->update_freq_step(_application.frequency_step());
    }
    qCDebug(core_app_window, "Sample rate changed: %u Hz", sample_rate);
}
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
    auto *close=new QToolButton(strip);close->setText("×");ui::bind(close,"toolTip","Close");row->addWidget(close);
    connect(close,&QToolButton::clicked,dock,&QDockWidget::hide);
    dock->setTitleBarWidget(strip);
    connect(dock,&QDockWidget::topLevelChanged,dock,[floatingTitle](bool floating) {
        floatingTitle->setVisible(floating);
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
QString MainWindow::ensure_node_view(int node, const QString &kind, const QString &device, const QString &graph,
                                     bool open) {
    const auto key =
        QString::fromLatin1(QJsonDocument(QJsonArray{device, graph, node}).toJson(QJsonDocument::Compact).toHex());
    if (_node_views.contains(key))
        return key;
    NodeView view;
    view.node = node;
    view.device = device;
    view.graph = graph;
    view.kind = kind;
    auto *dock = new QDockWidget(AudioNodeModel(kind).caption()+" #"+QString::number(node), this);
    connect(&ui::Language::instance(),&ui::Language::changed,dock,[dock,kind,node] {
        dock->setWindowTitle(AudioNodeModel(kind).caption()+" #"+QString::number(node));
    });
    dock->setObjectName("nodeView_" + key);
    view.dock = dock;
    setup_graph_dock_controls(dock);
    auto preferences = _application.state().file_configs.ui.graph_views.value(key).toObject();
    preferences["kind"] = kind;
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
    connect(dock, &QDockWidget::visibilityChanged, this, [this, key, dock](bool visible) {
        set_graph_exposed(key, dock, visible);
        refresh_subscriptions();
    });
    addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->setVisible(preferences.value("open").toBool(open));
    preferences["device"] = device;
    preferences["graph"] = graph;
    preferences["node"] = node;
    preferences["kind"] = kind;
    preferences["open"] = !dock->isHidden();
    _application.state().file_configs.ui.graph_views[key] = preferences;
    connect(dock, &QDockWidget::visibilityChanged, this, [this, key](bool visible) {
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
            ensure_node_view(p["node"].toInt(), p["kind"].toString(), p["device"].toString(), p["graph"].toString());
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
    for (auto it = _active_node_views.begin(); it != _active_node_views.end(); ++it) {
        const auto key = it.value();
        AbstractChartView *chart = nullptr;
        SpectrogramGraph *spectrogram = nullptr;
        QDockWidget *dock = nullptr;
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
        if (!views.contains(it.key()) || !views[it.key()].available) {
            if (chart && chart->value_count())
                chart->update(std::vector<float>(chart->value_count(), 0.0f));
            // A missing clock/contract cannot fabricate history; hold it until
            // the engine delivers correctly typed zero frames or explicit Stop.
            continue;
        }
        const auto &frame = views[it.key()];
        if (spectrogram) {
            if (auto *settings = findChild<SettingsWidget *>()) {
                const auto identity = QString::fromLatin1(
                    QJsonDocument(QJsonArray{_application.device_id(), _application.graph_id(), it.key()})
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
        label->setText(_surface_errors.values().join(" · "));
        label->setToolTip(label->text());
        label->setVisible(!_surface_errors.isEmpty());
    });
}
