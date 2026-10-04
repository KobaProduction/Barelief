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
#include <QToolBar>
#include <QToolButton>
#include <QSignalBlocker>

#include "core_app/gui_presenter.h"
#include "core_app/settings_widget.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "visualization/charts/line.h"
#include "visualization/charts/bar.h"
#include "visualization/charts/freq.h"

Q_LOGGING_CATEGORY(core_app_window, "core_app.window")

MainWindow::MainWindow(GuiPresenter &application) : _application(application) {
    setWindowTitle("Data Processing System Core");
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

    auto *status = new QLabel("Capture: starting", this);
    status->setObjectName("pipelineStatus");
    status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    statusBar()->addWidget(status, 1);
    auto states = std::make_shared<QMap<QString,QString>>();
    connect(&_application, &GuiPresenter::component_status_changed, this, [status,states](QString component, int state, QString error) {
        const QStringList names{"stopped","starting","running","recovering","failed"};
        (*states)[component] = component + ": " + names.value(state,"unknown") + (error.isEmpty() ? "" : " — " + error);
        status->setText(states->values().join(" | "));
    });
    connect(&_application, &GuiPresenter::raw_samples_ready, this, &MainWindow::on_raw_samples_ready);
    connect(&_application, &GuiPresenter::samples_ready, this, &MainWindow::on_samples_ready);
    connect(&_application, &GuiPresenter::samples_after_window_function_ready, this, &MainWindow::on_samples_after_window_function_ready);
    connect(&_application, &GuiPresenter::window_function_ready, this, &MainWindow::on_window_function_ready);
    connect(&_application, &GuiPresenter::amplitudes_ready, this, &MainWindow::on_amplitudes_ready);
    connect(&_application, &GuiPresenter::test_amplitudes_ready, this, &MainWindow::on_test_amplitudes_ready);
    connect(&_application, &GuiPresenter::spectrogram_updated, this, &MainWindow::on_spectrogram_updated);
}

MainWindow::~MainWindow() {
    if (_camera_save_timer.isActive() && _application.state().file_configs.ui.autosave_enabled) emit on_config_updated();
}

void MainWindow::setup_ui() {
    auto *menu = menuBar()->addMenu("File");
    const auto saveAction = menu->addAction("Save settings");
    saveAction->setShortcut(QKeySequence::Save);
    saveAction->setShortcutContext(Qt::ApplicationShortcut);

    connect(saveAction, &QAction::triggered, this, [this]() {
        _application.state().editor_draft_modified = true;
        emit on_config_updated();
    });

    _tab_widget = new QTabWidget(this);

    // Создаем все графики один раз

    // Raw Samples
    _raw_samples_chart = new LineChartView();
    _raw_samples_chart->chart()->setTitle("Raw Samples");
    _raw_samples_chart->set_range(-1, 1);
    _raw_samples_chart->set_auto_resizing(true);
    _raw_samples_chart->set_auto_gain(true, 1, -1);
    _raw_samples_tab_index = _tab_widget->addTab(_raw_samples_chart, "Raw Samples");

    // Samples
    _samples_chart = new LineChartView();
    _samples_chart->chart()->setTitle("Samples");
    _samples_chart->set_range(-1, 1);
    _samples_chart->set_auto_resizing(true);
    _samples_chart->set_auto_gain(true, 1, -1);
    _samples_tab_index = _tab_widget->addTab(_samples_chart, "Samples");

    // Samples after window function
    _samples_after_window_function_chart = new LineChartView();
    _samples_after_window_function_chart->chart()->setTitle("Samples after window function");
    _samples_after_window_function_chart->set_range(-1, 1);
    _samples_after_window_function_chart->set_auto_resizing(true);
    _samples_after_window_function_chart->set_auto_gain(true, 1, -1);
    _samples_after_window_function_tab_index = _tab_widget->addTab(_samples_after_window_function_chart, "Samples after window function");

    //  window function
    _window_function_chart = new LineChartView();
    _window_function_chart->chart()->setTitle("Window function values");
    _window_function_chart->set_range(-0.2, 1.2);
    _window_function_chart->update(_application.window());
    _window_function_tab_index = _tab_widget->addTab(_window_function_chart, "Window function");

    // Amplitudes (FFT)
    _amplitudes_chart = new FreqChartView();
    _amplitudes_chart->chart()->setTitle("Amplitudes FFT");
    _amplitudes_chart->set_range(0, 1);
    _amplitudes_chart->set_auto_resizing(true);
    _amplitudes_chart->set_auto_gain(true, 1, 0);

    _amplitudes_tab_index = _tab_widget->addTab(_amplitudes_chart, "Amplitudes (FFT)");

    // Test Amplitudes (Mel)
    _test_amplitudes_chart = new LineChartView();
    _test_amplitudes_chart->chart()->setTitle("Test Amplitudes (Mel)");
    _test_amplitudes_chart->set_range(0, 1);
    _test_amplitudes_chart->set_auto_resizing(true);
    _test_amplitudes_chart->set_auto_gain(true, 1, 0);
    _test_amplitudes_tab_index = _tab_widget->addTab(_test_amplitudes_chart, "Test Amplitudes (Mel)");

    // 3D Surface
    _surface_view = new SurfaceGraph(this);
    _surface_view->set_camera_state(_application.state().file_configs.ui.camera);
    _surface_view->set_camera_controls(_application.state().file_configs.ui.camera_controls);
    _surface_view->set_side_panel_ratio(_application.state().file_configs.ui.surface_side_panel_ratio);
    _camera_save_timer.setSingleShot(true);
    _camera_save_timer.setInterval(500);
    connect(&_camera_save_timer, &QTimer::timeout, this, [this] {
        if (_application.state().file_configs.ui.autosave_enabled) emit on_config_updated();
    });
    connect(_surface_view, &SurfaceGraph::camera_changed, this, [this](const QJsonObject &state) {
        _application.state().file_configs.ui.camera = state;
        _camera_save_timer.start();
    });
    connect(_surface_view, &SurfaceGraph::side_panel_resized, this, [this](float ratio) {
        _application.state().file_configs.ui.surface_side_panel_ratio = ratio;
        _camera_save_timer.start();
    });
    _surface_tab_index = _tab_widget->addTab(_surface_view, "3D Surface");

    // Settings
    auto *settings_dialog = new QDialog(this);
    settings_dialog->setObjectName("settingsDialog");
    settings_dialog->setWindowTitle("Settings");
    settings_dialog->resize(680, 660);
    settings_dialog->setMinimumSize(560,420);
    auto *settings_layout = new QVBoxLayout(settings_dialog);
    const auto settings_widget = new SettingsWidget(_application.state(), settings_dialog);
    settings_layout->addWidget(settings_widget);
    auto *dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Close, settings_dialog);
    settings_layout->addWidget(dialog_buttons);
    connect(dialog_buttons, &QDialogButtonBox::rejected, settings_dialog, &QDialog::hide);
    auto *settings_action = menuBar()->addAction("Settings");
    settings_action->setObjectName("openSettings");
    settings_action->setShortcut(QKeySequence("Ctrl+,"));
    connect(settings_action, &QAction::triggered, this, [settings_dialog] {
        settings_dialog->show(); settings_dialog->raise(); settings_dialog->activateWindow();
    });
    connect(&_application,&GuiPresenter::processing_state_changed,settings_widget,&SettingsWidget::refresh_processing_settings);
    connect(settings_widget, &SettingsWidget::processing_settings_changed, &_application, &GuiPresenter::apply_processing_settings);
    connect(settings_widget, &SettingsWidget::camera_controls_changed, this, [this] {
        _surface_view->set_camera_controls(_application.state().file_configs.ui.camera_controls);
    });
    connect(settings_widget, &SettingsWidget::reset_camera_requested, _surface_view, &SurfaceGraph::reset_camera);
    connect(settings_widget, &SettingsWidget::on_config_changed, this, &MainWindow::on_config_updated);
    connect(settings_widget, &SettingsWidget::on_rebuild_charts, this, &MainWindow::on_update_chart_visibility);
    connect(settings_widget, &SettingsWidget::start_loopback_requested, this, &MainWindow::start_loopback_requested);
    connect(settings_widget, &SettingsWidget::stop_loopback_requested, this, &MainWindow::stop_loopback_requested);
    connect(settings_widget, &SettingsWidget::start_analyzer_requested, this, &MainWindow::start_analyzer_requested);
    connect(settings_widget, &SettingsWidget::stop_analyzer_requested, this, &MainWindow::stop_analyzer_requested);
    connect(settings_widget, &SettingsWidget::start_wled_server_requested, this, &MainWindow::start_wled_server_requested);
    connect(settings_widget, &SettingsWidget::stop_wled_server_requested, this, &MainWindow::stop_wled_server_requested);
    connect(settings_widget, &SettingsWidget::wled_server_config_changed, this, &MainWindow::wled_server_config_changed);
    auto *toolbar = addToolBar("Pipeline and graphs");
    toolbar->setObjectName("pipelineToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->setStyleSheet("QToolButton { padding: 5px 8px; border-radius: 4px; } QToolButton:checked { background: palette(highlight); color: palette(highlighted-text); } QToolButton:hover { border: 1px solid palette(mid); }");
    auto bind_toggle = [this,settings_widget](QToolBar *bar, QMenu *menu, const char *title, const char *checkbox_name) {
        auto *checkbox = settings_widget->findChild<QCheckBox *>(checkbox_name);
        auto *action = new QAction(title,this);
        action->setObjectName(QString("toggle_%1").arg(checkbox_name));
        action->setCheckable(true);
        action->setChecked(checkbox->isChecked());
        connect(action,&QAction::toggled,checkbox,&QCheckBox::setChecked);
        connect(checkbox,&QCheckBox::toggled,action,[action](bool enabled) {
            const QSignalBlocker blocker(action); action->setChecked(enabled);
        });
        if (bar) bar->addAction(action);
        if (menu) menu->addAction(action);
    };
    bind_toggle(toolbar,nullptr,"Capture","enableLoopback");
    bind_toggle(toolbar,nullptr,"Analyzer","enableAnalyzer");
    toolbar->addSeparator();
    bind_toggle(toolbar,nullptr,"FFT","showFFT");
    bind_toggle(toolbar,nullptr,"Mel","showMel");
    bind_toggle(toolbar,nullptr,"3D","showSurface");
    auto *graphs_menu = new QMenu("Graphs",this);
    bind_toggle(nullptr,graphs_menu,"Raw samples","showRawSamples");
    bind_toggle(nullptr,graphs_menu,"Samples","showSamples");
    bind_toggle(nullptr,graphs_menu,"Windowed samples","showWindowedSamples");
    bind_toggle(nullptr,graphs_menu,"Window function","showWindowFunction");
    auto *graphs_button = new QToolButton(toolbar);
    graphs_button->setText("More graphs");
    graphs_button->setMenu(graphs_menu);
    graphs_button->setPopupMode(QToolButton::InstantPopup);
    toolbar->addWidget(graphs_button);
    toolbar->addSeparator();
    toolbar->addAction(settings_action);
    auto *pipeline_dialog = new QDialog(this);
    pipeline_dialog->setObjectName("pipelineDialog"); pipeline_dialog->setWindowTitle("Audio pipeline"); pipeline_dialog->resize(1200,800);
    auto *pipeline_layout = new QVBoxLayout(pipeline_dialog);
    pipeline_layout->addWidget(new SchemeEditorWidget(_application,pipeline_dialog));
    auto *pipeline_action = toolbar->addAction("Pipeline"); pipeline_action->setObjectName("openPipeline");
    connect(pipeline_action,&QAction::triggered,pipeline_dialog,[pipeline_dialog]{ pipeline_dialog->show(); pipeline_dialog->raise(); });

    setCentralWidget(_tab_widget);

    // Инициализируем видимость графиков на основе текущего конфига
    update_chart_visibility(_application.state().file_configs.ui);

    qCInfo(core_app_window, "UI setup completed");
}

void MainWindow::update_chart_visibility(const UIApplicationConfig &config) {
    _application.update_subscription();
    if (config.show_surface) {
        const auto size = screen()->availableGeometry().size();
        _surface_view->initialize(QSize(size.width() / 2, size.height() / 2), size);
    }
    _tab_widget->setTabVisible(_raw_samples_tab_index, config.show_raw_samples);
    _tab_widget->setTabVisible(_samples_tab_index, config.show_samples);
    _tab_widget->setTabVisible(_samples_after_window_function_tab_index, config.show_samples_after_window_function);
    _tab_widget->setTabVisible(_window_function_tab_index, config.show_window_function);
    _tab_widget->setTabVisible(_amplitudes_tab_index, config.show_amplitudes);
    _tab_widget->setTabVisible(_test_amplitudes_tab_index, config.show_test_amplitudes);
    _tab_widget->setTabVisible(_surface_tab_index, config.show_surface);

    qCDebug(core_app_window, "Chart visibility updated");
}

void MainWindow::on_update_chart_visibility() {
    update_chart_visibility(_application.state().file_configs.ui);
}

void MainWindow::on_raw_samples_ready(const std::vector<float> &data) {
    if (_raw_samples_chart && _raw_samples_chart->isVisible()) {
        _raw_samples_chart->update(data);
    }
}

void MainWindow::on_samples_ready(const std::vector<float> &data) {
    if (_samples_chart && _samples_chart->isVisible()) {
        _samples_chart->update(data);
    }
}

void MainWindow::on_samples_after_window_function_ready(const std::vector<float> &data) {
    if (_samples_after_window_function_chart && _samples_after_window_function_chart->isVisible()) {
        _samples_after_window_function_chart->update(data);
    }
}

void MainWindow::on_window_function_ready(const std::vector<float> &data) {
    if (_window_function_chart && _window_function_chart->isVisible()) {
        _window_function_chart->update(data);
    }
}

void MainWindow::on_amplitudes_ready(const std::vector<float> &data) {
    if (_amplitudes_chart && _amplitudes_chart->isVisible()) {
        reinterpret_cast<FreqChartView *>(_amplitudes_chart)->update_freq_step( _application.frequency_step() );
        _amplitudes_chart->update(data);
    }
}

void MainWindow::on_test_amplitudes_ready(const std::vector<float> &data) {
    if (_test_amplitudes_chart && _test_amplitudes_chart->isVisible()) {
        _test_amplitudes_chart->update(data);
    }
}

void MainWindow::on_spectrogram_updated(const std::vector<std::vector<float> > &data) const {
    if (_surface_view && _surface_view->isVisible()) {
        _surface_view->update(data);
    }
}

void MainWindow::on_sample_rate_changed(uint32_t sample_rate) {
    if (_amplitudes_chart) {
        auto *freq_chart = reinterpret_cast<FreqChartView *>(_amplitudes_chart);
        freq_chart->update_freq_step(_application.frequency_step());
    }
    qCDebug(core_app_window, "Sample rate changed: %u Hz", sample_rate);
}
