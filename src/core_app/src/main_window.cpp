#include "visualization/ui_text.h"
#include "core_app/graph_panel.h"
#include "visualization/abstract/fps.h"
#include "core_app/main_window.h"
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

#include "core_app/gui_presenter.h"
#include "core_app/settings_widget.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "visualization/charts/line.h"
#include "visualization/charts/bar.h"
#include "visualization/charts/freq.h"

Q_LOGGING_CATEGORY(core_app_window, "core_app.window")

MainWindow::MainWindow(GuiPresenter &application) : _application(application) {
    ui::Theme::instance().set(_application.state().file_configs.ui.theme);
    ui::Language::instance().set(_application.state().file_configs.ui.language);
    ui::bind(this,"windowTitle","Data Processing System Core");
    setMinimumSize(800, 600);
    resize(1000, 800);

    setup_ui();
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
    connect(&_application,&GuiPresenter::command_rejected,this,[this](quint64, const QString &error) {
        statusBar()->showMessage("Engine command rejected: " + error,10000);
    });
    connect(&_application, &GuiPresenter::sample_rate_changed, this, &MainWindow::on_sample_rate_changed);

    auto *status = new QLabel(ui::text("Capture: starting"),this);
    status->setObjectName("pipelineStatus");
    status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    statusBar()->addWidget(status, 1);
    struct Component { int state; QString error; };
    auto states = std::make_shared<QMap<QString,Component>>();
    auto refresh_status=[status,states] {
        const char *names[]{"stopped","starting","running","recovering","failed"}; QStringList lines;
        for (auto it=states->cbegin(); it!=states->cend(); ++it) {
            const auto &value=it.value();
            lines << ui::text(it.key().toUtf8().constData())+": "+ui::text(value.state>=0 && value.state<5 ? names[value.state] : "unknown")+
                (value.error.isEmpty() ? "" : " — "+value.error);
        }
        status->setText(lines.join(" | "));
    };
    connect(&_application, &GuiPresenter::component_status_changed, this, [states,refresh_status](QString component, int state, QString error) {
        (*states)[component]={state,error}; refresh_status();
    });
    connect(&ui::Language::instance(),&ui::Language::changed,status,refresh_status);
    auto *fps=new QLabel(this); fps->setObjectName("renderStatus");
    fps->setStyleSheet("padding: 0 8px; color: palette(text);");
    statusBar()->addPermanentWidget(fps);
    auto *fps_timer=new QTimer(this); fps_timer->setInterval(500);
    auto refresh_fps=[this,fps] {
        if (!_application.state().file_configs.ui.show_fps) return;
        QStringList rates;
        const std::pair<const char *,AbstractChartView *> charts[]{{"rawSamples",_raw_signal_chart},{"windowSignal",_window_signal_chart},
            {"windowedSignal",_windowed_signal_chart},{"windowFunction",_window_function_chart},
            {"fft",_amplitudes_chart},{"mel",_test_amplitudes_chart}};
        for (auto [id,chart] : charts) if (_render_visible.value(id) && chart->isVisible())
            if (auto *view=dynamic_cast<FPSChartView *>(chart)) rates << ui::text("%1: %2 FPS").arg(_panels[id]->windowTitle()).arg(view->display_fps(),0,'f',0);
        if (_render_visible.value("spectrogram") && _spectrogram_view->isVisible()) rates << ui::text("%1: %2 FPS").arg(_panels["spectrogram"]->windowTitle()).arg(_spectrogram_view->display_fps(),0,'f',0);
        fps->setText(rates.join(" · "));
    };
    connect(fps_timer,&QTimer::timeout,this,refresh_fps);
    for (auto *chart : {_raw_signal_chart,_window_signal_chart,_windowed_signal_chart,_window_function_chart,_amplitudes_chart,_test_amplitudes_chart})
        if (auto *view=dynamic_cast<FPSChartView *>(chart)) connect(view,&FPSChartView::fps_changed,this,refresh_fps);
    auto refresh_preferences=[this,fps] { fps->setVisible(_application.state().file_configs.ui.show_fps); };
    refresh_preferences();
    connect(findChild<SettingsWidget *>(),&SettingsWidget::presentation_preferences_changed,this,refresh_preferences);
    fps_timer->start();
    connect(&_application, &GuiPresenter::raw_signal_ready, this, &MainWindow::on_raw_signal_ready);
    connect(&_application, &GuiPresenter::window_signal_ready, this, &MainWindow::on_window_signal_ready);
    connect(&_application, &GuiPresenter::windowed_signal_ready, this, &MainWindow::on_windowed_signal_ready);
    connect(&_application, &GuiPresenter::window_function_ready, this, &MainWindow::on_window_function_ready);
    connect(&_application, &GuiPresenter::amplitudes_ready, this, &MainWindow::on_amplitudes_ready);
    connect(&_application, &GuiPresenter::test_amplitudes_ready, this, &MainWindow::on_test_amplitudes_ready);
    connect(&_application, &GuiPresenter::spectrogram_updated, this, &MainWindow::on_spectrogram_updated);
}

MainWindow::~MainWindow() {
    _restoring_layout = true; _workspace_save_timer.stop(); _camera_save_timer.stop();
    for (auto *panel : _panels) { disconnect(panel,nullptr,this,nullptr); disconnect(panel->toggleViewAction(),nullptr,this,nullptr); }
    if (auto *editor = findChild<SchemeEditorWidget *>()) editor->flush_presentation();
    save_workspace();
    if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
}

void MainWindow::setup_ui() {
    auto *saveAction = new QAction(this);
    addAction(saveAction);
    ui::bind(saveAction,"text","Save settings");
    saveAction->setObjectName("saveSettings");
    saveAction->setShortcut(QKeySequence::Save);
    saveAction->setShortcutContext(Qt::ApplicationShortcut);

    connect(saveAction, &QAction::triggered, this, [this]() {
        if (auto *editor = findChild<SchemeEditorWidget *>()) editor->flush_presentation();
        save_workspace();
        _application.state().editor_draft_modified = true;
        emit on_config_updated();
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
    _spectrogram_view->set_camera_state(_application.state().file_configs.ui.camera);
    _spectrogram_view->set_camera_controls(_application.state().file_configs.ui.camera_controls);
    _spectrogram_view->set_side_panel_ratio(_application.state().file_configs.ui.spectrogram_side_panel_ratio);
    _camera_save_timer.setSingleShot(true);
    _camera_save_timer.setInterval(500);
    connect(&_camera_save_timer, &QTimer::timeout, this, [this] {
        if (_application.state().file_configs.ui.autosave_enabled) emit on_config_updated();
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
    ui::bind(settings_dialog,"windowTitle","Settings");
    settings_dialog->resize(680, 660);
    settings_dialog->setMinimumSize(560,420);
    auto *settings_layout = new QVBoxLayout(settings_dialog);
    const auto settings_widget = new SettingsWidget(_application.state(), settings_dialog);
    settings_layout->addWidget(settings_widget);
    auto *dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Close, settings_dialog);
    ui::bind(dialog_buttons->button(QDialogButtonBox::Close),"text","Close");
    settings_layout->addWidget(dialog_buttons);
    connect(dialog_buttons, &QDialogButtonBox::rejected, settings_dialog, &QDialog::hide);
    auto *settings_menu=new QMenu(this);
    settings_menu->setObjectName("settingsMenu"); ui::bind(settings_menu,"title","Settings");
    const char *sections[]{"General","Audio processing","WLED","Network","Controls"};
    for (int i : {0,1,3,4}) {
        auto *action=settings_menu->addAction(ui::text(sections[i]));
        ui::bind(action,"text",sections[i]);
        action->setObjectName(i==0 ? "openSettings" : QString("openSettingsSection%1").arg(i));
        action->setShortcut(i==0 ? QKeySequence("Ctrl+,") : QKeySequence(QString("Ctrl+Alt+%1").arg(i+1)));
        connect(action,&QAction::triggered,this,[settings_dialog,settings_widget,i] {
            settings_widget->select_section(i);
            settings_dialog->show(); settings_dialog->raise(); settings_dialog->activateWindow();
        });
    }
    connect(settings_widget, &SettingsWidget::camera_controls_changed, this, [this] {
        _spectrogram_view->set_camera_controls(_application.state().file_configs.ui.camera_controls);
    });
    connect(settings_widget, &SettingsWidget::reset_camera_requested, _spectrogram_view, &SpectrogramGraph::reset_camera);
    connect(settings_widget, &SettingsWidget::on_config_changed, &_application, &GuiPresenter::on_config_updated);
    connect(settings_widget, &SettingsWidget::on_rebuild_charts, this, &MainWindow::on_update_chart_visibility);
    connect(settings_widget, &SettingsWidget::start_wled_server_requested, this, &MainWindow::start_wled_server_requested);
    connect(settings_widget, &SettingsWidget::stop_wled_server_requested, this, &MainWindow::stop_wled_server_requested);
    connect(settings_widget, &SettingsWidget::wled_server_config_changed, this, &MainWindow::wled_server_config_changed);
    for (auto *panel : findChildren<GraphPanel *>()) {
        auto refresh=[this,panel] { panel->set_processing_choices(_application.state().file_configs.core.spectrum_channel,
            static_cast<int>(_application.state().file_configs.core.spectrum_merge)); };
        refresh(); connect(&_application,&GuiPresenter::processing_state_changed,panel,refresh);
        connect(panel,&GraphPanel::channel_requested,&_application,&GuiPresenter::set_spectrum_channel);
        connect(panel,&GraphPanel::merge_requested,&_application,&GuiPresenter::set_spectrum_merge);
    }
    auto *editor = new SchemeEditorWidget(_application,this);
    connect(editor,&SchemeEditorWidget::status_message,this,[this](const QString &text) { statusBar()->showMessage(text,5000); });
    auto *pipeline_panel = add_panel("dataFlow", "Data Flow", editor, &UIApplicationConfig::show_data_flow);
    auto *pipeline_action = pipeline_panel->toggleViewAction();
    pipeline_action->setObjectName("openPipeline");
    pipeline_action->setShortcut(QKeySequence("Ctrl+D"));
    auto *pipeline_toggle=new QToolButton(this); pipeline_toggle->setObjectName("pipelineToggle");
    pipeline_toggle->setDefaultAction(pipeline_action);
    statusBar()->addPermanentWidget(pipeline_toggle);
    connect(pipeline_action,&QAction::triggered,this,[pipeline_panel](bool visible) {
        if (visible) pipeline_panel->raise();
    });
    auto *view_menu = menuBar()->addMenu(ui::text("Graphs"));
    view_menu->setObjectName("graphsMenu"); ui::bind(view_menu,"title","Graphs");
    for (const auto &id : {"spectrogram","fft","mel","rawSamples","windowSignal","windowedSignal","windowFunction"})
        view_menu->addAction(_panels[id]->toggleViewAction());
    view_menu->addSeparator(); view_menu->addAction(pipeline_action);
    view_menu->addSeparator();
    auto *reset = view_menu->addAction(ui::text("Reset layout")); ui::bind(reset,"text","Reset layout"); reset->setObjectName("resetWorkspace");
    connect(reset,&QAction::triggered,this,[this] { reset_workspace(); _workspace_save_timer.start(); });
    auto *wled_menu=menuBar()->addMenu("WLED"); wled_menu->setObjectName("wledMenu");
    auto *wled_action=wled_menu->addAction("WLED"); wled_action->setObjectName("openSettingsSection2");
    wled_action->setShortcut(QKeySequence("Ctrl+Alt+3"));
    connect(wled_action,&QAction::triggered,this,[settings_dialog,settings_widget] {
        settings_widget->select_section(2); settings_dialog->show(); settings_dialog->raise(); settings_dialog->activateWindow();
    });
    settings_menu->addSeparator(); settings_menu->addAction(saveAction);
    menuBar()->addMenu(settings_menu);
    _workspace_save_timer.setSingleShot(true); _workspace_save_timer.setInterval(350);
    connect(&_workspace_save_timer,&QTimer::timeout,this,[this] {
        save_workspace();
        if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
    });
    const auto saved_ui = _application.state().file_configs.ui;
    const auto workspace = saved_ui.workspace;
    reset_workspace();
    if (workspace.value("version").toInt() == 1) {
        const auto geometry = QByteArray::fromBase64(workspace.value("geometry").toString().toLatin1());
        const auto state = QByteArray::fromBase64(workspace.value("state").toString().toLatin1());
        if (!geometry.isEmpty()) restoreGeometry(geometry);
        const int layout_version=workspace.value("layout_version").toInt();
        // Qt serializes dock object names. Restore legacy names temporarily, then
        // keep canonical names for all subsequent saves and runtime lookups.
        const QMap<QString,QString> legacy_names{{"spectrogram","surfaceDock"},{"windowSignal","samplesDock"},{"windowedSignal","windowedSamplesDock"}};
        if (layout_version==2) for (auto it=legacy_names.cbegin();it!=legacy_names.cend();++it) _panels[it.key()]->setObjectName(it.value());
        if ((layout_version==2 || layout_version==3) && !state.isEmpty() && restoreState(state,1))
            _application.state().file_configs.ui = saved_ui;
        for (auto it=legacy_names.cbegin();it!=legacy_names.cend();++it) _panels[it.key()]->setObjectName(it.key()+"Dock");
    }
    _restoring_layout = false;
    QTimer::singleShot(0,this,[this] { recover_offscreen_panels(); refresh_subscriptions(); });
    connect(qApp,&QGuiApplication::screenRemoved,this,[this] { recover_offscreen_panels(); });

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
    const bool restoring = _restoring_layout; _restoring_layout = true;
    _panels["rawSamples"]->setVisible(config.show_raw_samples);
    _panels["windowSignal"]->setVisible(config.show_window_signal);
    _panels["windowedSignal"]->setVisible(config.show_windowed_signal);
    _panels["windowFunction"]->setVisible(config.show_window_function);
    _panels["fft"]->setVisible(config.show_amplitudes);
    _panels["mel"]->setVisible(config.show_test_amplitudes);
    _panels["spectrogram"]->setVisible(config.show_spectrogram);
    _panels["dataFlow"]->setVisible(config.show_data_flow);
    const std::pair<const char *, bool> controls[]{{"showRawSamples",config.show_raw_samples},
        {"showWindowSignal",config.show_window_signal},{"showWindowedSignal",config.show_windowed_signal},
        {"showWindowFunction",config.show_window_function},{"showFFT",config.show_amplitudes},
        {"showMel",config.show_test_amplitudes},{"showSpectrogram",config.show_spectrogram}};
    for (const auto &[name, visible] : controls) {
        if (auto *checkbox = findChild<QCheckBox *>(name)) {
            const QSignalBlocker blocker(checkbox); checkbox->setChecked(visible);
        }
        if (auto *action = findChild<QAction *>("toggle_" + QString::fromLatin1(name))) {
            const QSignalBlocker blocker(action); action->setChecked(visible);
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
        static_cast<FreqChartView *>(_amplitudes_chart)->update_freq_step( _application.frequency_step() );
        _amplitudes_chart->update(data);
    }
}

void MainWindow::on_test_amplitudes_ready(const std::vector<float> &data) {
    if (_test_amplitudes_chart && _render_visible.value("mel") && _test_amplitudes_chart->isVisible()) {
        _test_amplitudes_chart->update(data);
    }
}

void MainWindow::on_spectrogram_updated(const std::vector<std::vector<float> > &data) const {
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
QDockWidget *MainWindow::add_panel(const QString &id, const QString &title, QWidget *view, bool UIApplicationConfig::*flag) {
    auto *dock = new QDockWidget(title,this); dock->setObjectName(id + "Dock");
    dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    ui::bind(dock,"windowTitle",title);
    ui::bind(dock->toggleViewAction(),"text",title);
    if (auto *chart=dynamic_cast<AbstractChartView *>(view)) {
        auto *panel=new GraphPanel(id,chart,_application.state().file_configs.ui.graph_views.value(id).toObject(),dock);
        connect(panel,&GraphPanel::presentation_changed,this,[this](const QString &id,const QJsonObject &prefs) {
            auto saved=_application.state().file_configs.ui.graph_views.value(id).toObject();
            for (auto it=prefs.constBegin(); it!=prefs.constEnd(); ++it) saved[it.key()]=it.value();
            _application.state().file_configs.ui.graph_views[id]=saved; _workspace_save_timer.start();
        });
        dock->setWidget(panel);
    } else dock->setWidget(view); dock->installEventFilter(this); _panels[id] = dock;
    addDockWidget(Qt::LeftDockWidgetArea,dock);
    if (id!="dataFlow") {
        // A compact drag strip keeps docking controls without repeating the tab caption.
        auto *strip=new QWidget(dock); strip->setObjectName(id+"DockControls");
        auto *row=new QHBoxLayout(strip); row->setContentsMargins(4,0,4,0); row->addStretch();
        auto *detach=new QToolButton(strip); detach->setText("↗"); ui::bind(detach,"toolTip","Detach graph");
        auto *close=new QToolButton(strip); close->setText("×"); ui::bind(close,"toolTip","Close");
        row->addWidget(detach); row->addWidget(close);
        connect(detach,&QToolButton::clicked,dock,[dock] { dock->setFloating(true); });
        connect(close,&QToolButton::clicked,dock,&QDockWidget::hide);
        dock->setTitleBarWidget(strip);
        connect(dock,&QDockWidget::topLevelChanged,dock,[dock,strip](bool floating) {
            dock->setTitleBarWidget(floating ? nullptr : strip);
        });
    }
    connect(dock->toggleViewAction(), &QAction::toggled,this,[this,flag,id](bool open) {
        if (_restoring_layout) return;
        _application.state().file_configs.ui.*flag = open;
        const QMap<QString,QString> controls{{"rawSamples","showRawSamples"},{"windowSignal","showWindowSignal"},
            {"windowedSignal","showWindowedSignal"},{"windowFunction","showWindowFunction"},
            {"fft","showFFT"},{"mel","showMel"},{"spectrogram","showSpectrogram"}};
        if (controls.contains(id)) {
            const auto name = controls.value(id);
            if (auto *checkbox = findChild<QCheckBox *>(name)) {
                const QSignalBlocker blocker(checkbox); checkbox->setChecked(open);
            }
            if (auto *action = findChild<QAction *>("toggle_" + name)) {
                const QSignalBlocker blocker(action); action->setChecked(open);
            }
        }
        _workspace_save_timer.start(); refresh_subscriptions();
    });
    connect(dock,&QDockWidget::visibilityChanged,this,[this,id](bool visible) {
        if (auto *panel=qobject_cast<GraphPanel *>(_panels[id]->widget())) panel->set_render_visible(visible);
        _render_visible[id] = visible; refresh_subscriptions();
        if (!_restoring_layout) _workspace_save_timer.start();
    });
    connect(dock,&QDockWidget::topLevelChanged,this,[this] { if (!_restoring_layout) _workspace_save_timer.start(); });
    connect(dock,&QDockWidget::dockLocationChanged,this,[this] { if (!_restoring_layout) _workspace_save_timer.start(); });
    return dock;
}
void MainWindow::reset_workspace() {
    const bool restoring = _restoring_layout; _restoring_layout = true;
    if (!_default_workspace_state.isEmpty()) {
        // Let Qt restore its tab groups atomically instead of removing and reusing
        // docks while old floating/tab layouts still refer to them.
        restoreState(_default_workspace_state, 1);
    } else {
        for (auto *dock : _panels) removeDockWidget(dock);
        addDockWidget(Qt::LeftDockWidgetArea,_panels["spectrogram"]);
        splitDockWidget(_panels["spectrogram"],_panels["dataFlow"],Qt::Vertical);
        for (const auto &id : {"fft","mel","rawSamples","windowSignal","windowedSignal","windowFunction"}) {
            addDockWidget(Qt::LeftDockWidgetArea,_panels[id]); tabifyDockWidget(_panels["spectrogram"],_panels[id]);
        }
    }
    auto &ui = _application.state().file_configs.ui;
    ui.show_spectrogram = ui.show_data_flow = true;
    ui.show_amplitudes = ui.show_test_amplitudes = ui.show_raw_samples = ui.show_window_signal =
        ui.show_windowed_signal = ui.show_window_function = false;
    update_chart_visibility(ui);
    _panels["spectrogram"]->raise();
    resizeDocks({_panels["spectrogram"],_panels["dataFlow"]},{560,320},Qt::Vertical);
    if (_default_workspace_state.isEmpty()) _default_workspace_state = saveState(1);
    _restoring_layout = restoring;
    refresh_subscriptions();
}
void MainWindow::save_workspace() {
    _application.state().file_configs.ui.workspace = {{"version",1},
        {"layout_version",3},
        {"geometry",QString::fromLatin1(saveGeometry().toBase64())},
        {"state",QString::fromLatin1(saveState(1).toBase64())}};
}
void MainWindow::refresh_subscriptions() {
    if (_restoring_layout) return;
    quint32 mask = 0;
    const auto visible = [this](const QString &id) { return _render_visible.value(id) && !_panels[id]->isHidden(); };
    if (visible("dataFlow")) mask |= pipeline::EngineCommand::Previews | pipeline::EngineCommand::Views;
    if (visible("rawSamples")) mask |= pipeline::EngineCommand::Audio;
    if (visible("spectrogram")) mask |= pipeline::EngineCommand::History | pipeline::EngineCommand::Views;
    for (const auto &id : {"fft","mel","windowSignal","windowedSignal","windowFunction"})
        if (visible(id)) mask |= pipeline::EngineCommand::Views;
    for (const auto &id : {"windowSignal","windowedSignal","windowFunction"})
        if (visible(id)) mask |= pipeline::EngineCommand::Diagnostics;
    _application.set_visible_views(mask);
}
void MainWindow::recover_offscreen_panels() {
    const auto screens = QGuiApplication::screens(); if (screens.isEmpty()) return;
    for (auto *dock : _panels) if (dock->isFloating()) {
        QScreen *target = nullptr;
        const auto title = QRect(dock->frameGeometry().topLeft(), QSize(dock->width(), 30));
        for (auto *screen : screens)
            if (screen->availableGeometry().contains(title.center())) { target = screen; break; }
        if (!target) target = screens.front();
        const auto available = target->availableGeometry().adjusted(8,8,-8,-8);
        dock->resize(dock->size().boundedTo(available.size()));
        auto position = dock->frameGeometry().topLeft();
        position.setX(std::clamp(position.x(), available.left(),
            std::max(available.left(), available.right()-dock->frameGeometry().width()+1)));
        position.setY(std::clamp(position.y(), available.top(),
            std::max(available.top(), available.bottom()-dock->frameGeometry().height()+1)));
        dock->move(position);
    }
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (!_restoring_layout && qobject_cast<QDockWidget *>(watched) &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize))
        _workspace_save_timer.start();
    return QMainWindow::eventFilter(watched,event);
}
void MainWindow::closeEvent(QCloseEvent *event) {
    _workspace_save_timer.stop(); _camera_save_timer.stop();
    if (auto *editor = findChild<SchemeEditorWidget *>()) editor->flush_presentation();
    save_workspace();
    if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
    QMainWindow::closeEvent(event);
}
void MainWindow::moveEvent(QMoveEvent *event) {
    QMainWindow::moveEvent(event); if (!_restoring_layout) _workspace_save_timer.start();
}
void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event); if (!_restoring_layout) _workspace_save_timer.start();
}
