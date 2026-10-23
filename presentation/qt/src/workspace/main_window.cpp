#include "barelief/surface_settings.h"
#include "barelief/ui_text.h"
#include "core_app/graph_panel.h"
#include "barelief/abstract/fps.h"
#include "core_app/main_window.h"
#include <core/performance.h>
#include <cmath>
#include <utility>
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
#include <QPainter>
#include <QStyleOption>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScopedValueRollback>
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
#include <QStringView>
#include <QSpinBox>
#include <QFormLayout>
#include <QStandardItemModel>
#include <QActionGroup>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTabBar>
#include <QFileDialog>
#include "core_app/workspace/ui_command_registry.h"
#include "core_app/workspace/processor_manager_dialog.h"

#include "core_app/gui_presenter.h"
#include "core_app/settings_widget.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "barelief/charts/line.h"
#include "barelief/charts/bar.h"
#include "barelief/charts/freq.h"

Q_LOGGING_CATEGORY(core_app_window, "core_app.window")

namespace {
// The status occupies only unused menu-bar space; actions and corner controls
// keep the sizing and input behavior provided by QMenuBar.
class MenuRowStatus final : public QLabel {
  public:
    using QLabel::QLabel;
  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        QStyleOption option;
        option.initFrom(this);
        style()->drawItemText(&painter, contentsRect(), alignment(), option.palette, isEnabled(),
                             fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()), foregroundRole());
    }
};

class NavigationMenuBar final : public QMenuBar {
  public:
    explicit NavigationMenuBar(QWidget *parent) : QMenuBar(parent) { setNativeMenuBar(false); }
    void setCenterWidget(QWidget *widget) {
        _center = widget;
        widget->setParent(this);
        widget->show();
        updateCenter();
    }
    void watchCornerWidget(QWidget *widget) {
        widget->installEventFilter(this);
        for (auto corner : {Qt::TopLeftCorner, Qt::TopRightCorner})
            if (cornerWidget(corner) == widget)
                _corner_hints[corner == Qt::TopLeftCorner ? 0 : 1] = {widget, widget->sizeHint()};
    }
  protected:
    bool event(QEvent *event) override {
        const bool result = QMenuBar::event(event);
        switch (event->type()) {
        case QEvent::Show: case QEvent::FontChange: case QEvent::StyleChange:
        case QEvent::LayoutDirectionChange:
        case QEvent::ActionAdded: case QEvent::ActionChanged: case QEvent::ActionRemoved:
            // The native action/corner cache also depends on menu metrics and
            // first-show polish, even when a corner reports the same size hint.
            _corner_metrics_dirty = true;
            scheduleCornerUpdate();
            updateCenter();
            break;
        case QEvent::Resize: case QEvent::LayoutRequest:
            updateCenter();
            break;
        default: break;
        }
        return result;
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        const bool result = QMenuBar::eventFilter(object, event);
        if (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
            event->type() == QEvent::Show || event->type() == QEvent::Hide ||
            event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent ||
            event->type() == QEvent::LayoutRequest) {
            updateCenter();
            // QMenuBar lays out corner widgets while updating its action cache.
            // Recheck after that entire layout operation has completed.
            scheduleCornerUpdate();
        }
        return result;
    }
  private:
    QPointer<QWidget> _center;
    struct CornerHint { QPointer<QWidget> widget; QSize size; };
    CornerHint _corner_hints[2];
    bool _corner_metrics_dirty = true;
    bool _positioning = false;
    bool _position_pending = false;
    void refreshCornerHints() {
        const bool metricsChanged = std::exchange(_corner_metrics_dirty, false);
        for (auto corner : {Qt::TopLeftCorner, Qt::TopRightCorner}) {
            auto *widget = cornerWidget(corner);
            auto &cached = _corner_hints[corner == Qt::TopLeftCorner ? 0 : 1];
            const QSize hint = widget ? widget->sizeHint() : QSize{};
            if (!metricsChanged && cached.widget == widget && cached.size == hint) continue;
            cached = {widget, hint};
            if (widget) QMenuBar::setCornerWidget(widget, corner);
        }
    }
    void scheduleCornerUpdate() {
        if (!_position_pending) {
            _position_pending = true;
            QTimer::singleShot(0, this, [this] {
                    _position_pending = false;
                    // Corner layouts can change their size hint without a bar
                    // resize (for example after loading a font or translating).
                    // QMenuBar uses sizeHint() to reserve corner space. Its
                    // setCornerWidget() repaints even for the same widget, so
                    // refresh that cache only when its sizing input changed.
                    refreshCornerHints();
                    updateCenter();
            });
        }
    }
    void accountActionBounds(int& left,int& right) const {
        const auto visibleActions=actions();
        for (auto *action : visibleActions) {
            if (!action->isVisible()) continue;
            const auto geometry = actionGeometry(action);
            if (layoutDirection() == Qt::RightToLeft) right = qMin(right, geometry.left());
            else left = qMax(left, geometry.right() + 1);
        }
    }
    void accountCornerBounds(int& left,int& right) const {
        for (auto corner : {Qt::TopLeftCorner, Qt::TopRightCorner}) {
            auto *widget = cornerWidget(corner);
            if (!widget || widget->isHidden()) continue;
            if ((corner == Qt::TopLeftCorner) != (layoutDirection() == Qt::RightToLeft))
                left = qMax(left, widget->geometry().right() + 1);
            else right = qMin(right, widget->geometry().left());
        }
    }
    void updateCenter() {
        if (!_center || _positioning) return;
        const QScopedValueRollback positioning(_positioning, true);
        const int spacing = style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing, nullptr, this);
        int left = contentsRect().left();
        int right = contentsRect().right() + 1;
        accountActionBounds(left,right);
        accountCornerBounds(left,right);
        const int gap = qMax(4, spacing);
        const int start = qBound(contentsRect().left(), left + gap, contentsRect().right() + 1);
        _center->setGeometry(start, contentsRect().top(), qMax(0, right - gap - start), contentsRect().height());
    }
};
}

void MainWindow::setup_editor_commands() {
    if (auto *editor = data_flow_editor()) {
        _commands->set_editor(editor);
        auto *menu = findChild<QMenu *>("fileMenu"); menu->addSeparator();
        menu->addAction(_commands->register_command({"undoDataFlow","Undo",QKeySequence("Ctrl+Z"),presentation::UiCommandContext::SchemaEditor,
            [editor] { editor->history().undo(); }, [editor] { return editor->history().canUndo(); }}));
        menu->addAction(_commands->register_command({"redoDataFlow","Redo",QKeySequence("Ctrl+Shift+Z"),presentation::UiCommandContext::SchemaEditor,
            [editor] { editor->history().redo(); }, [editor] { return editor->history().canRedo(); }}));
        connect(&editor->history(),&QUndoStack::canUndoChanged,_commands,&presentation::UiCommandRegistry::refresh);
        connect(&editor->history(),&QUndoStack::canRedoChanged,_commands,&presentation::UiCommandRegistry::refresh);
    }
}

void MainWindow::setup_navigation_commands() {
    // Familiar JetBrains navigation belongs to presentation, never the DSP core.
    auto *navigation=findChild<QMenu *>("graphsMenu");
    navigation->addSeparator();
    register_navigation_tab_actions(navigation);
}
void MainWindow::register_navigation_tab_actions(QMenu* navigation) {
    auto action=[this,navigation](const char *label,const char *id,const char *key,auto callback) {
        navigation->addAction(_commands->register_command({QString::fromLatin1(id),label,QKeySequence(QString::fromLatin1(key)),
            presentation::UiCommandContext::Workspace,callback,{}}));
    };
    action("Next tab","workspaceNextTab","Alt+Right",[this]{activateAdjacentTab(1);});
    action("Previous tab","workspacePreviousTab","Alt+Left",[this]{activateAdjacentTab(-1);});
    action("Detach active tab","workspaceDetachTab","Shift+F4",[this]{detach_active_tab();});
    action("Hide active panel","workspaceHidePanel","Shift+Escape",[this]{hide_active_panel();});
    action("Focus Data Flow","workspaceFocusFlow","F12",[this]{focus_data_flow();});
    action("Show or hide Data Flow","workspaceToggleTools","Ctrl+Shift+F12",[this]{toggle_data_flow();});
    action("Switch workspace tab…","workspaceSwitcher","Ctrl+Tab",[this]{show_workspace_switcher();});
}

void MainWindow::detach_active_tab() {if(auto* dock=activeDock())dock->setFloating(true);}
void MainWindow::hide_active_panel() {if(auto* dock=activeDock())dock->hide();}
void MainWindow::focus_data_flow() {
    auto *flow=_panels.value("dataFlow");
    flow->raise();
    focusDock(flow);
}
void MainWindow::toggle_data_flow() {
    auto *flow=_panels.value("dataFlow");
    flow->toggleView(flow->isHidden());
}

void MainWindow::show_workspace_switcher() {
    auto *menu=new QMenu(this); menu->setAttribute(Qt::WA_DeleteOnClose);
    for(const auto docks=workspaceDocks(); auto *dock:docks) if(!dock->isHidden()) {
        auto *item=menu->addAction(dock->windowTitle());
        connect(item,&QAction::triggered,this,[this,dock]{dock->raise();focusDock(dock);});
    }
    menu->popup(mapToGlobal(rect().center()));
}

QHBoxLayout *MainWindow::setup_processor_selector(std::shared_ptr<presentation::ProcessorDiscoveryPort> discovery) {
    auto *directory=create_processor_directory(std::move(discovery));
    auto *manage=setup_processor_manager(directory);
    auto *deviceLayout=setup_processor_corner(directory,manage);
    bind_processor_status(directory);
    bind_processor_selection(directory);
    return deviceLayout;
}

presentation::ProcessorDirectoryModel *MainWindow::create_processor_directory(std::shared_ptr<presentation::ProcessorDiscoveryPort> discovery) {
    auto *directory = new presentation::ProcessorDirectoryModel(_application.local_device_id(),std::move(discovery),this);
    if (_application.is_remote()) {
        const auto endpoint=_application.device_id(); const auto separator=endpoint.lastIndexOf(':');
        directory->add_direct(endpoint,endpoint.left(separator),quint16(QStringView(endpoint).mid(separator+1).toUInt()));
    }
    directory->set_selected(_application.device_id());
    return directory;
}

QAction *MainWindow::setup_processor_manager(presentation::ProcessorDirectoryModel *directory) {
    const auto network=_application.state().file_configs.raw_document.value("aggregator").toObject();
    auto *registry_dialog=new presentation::ProcessorManagerDialog(*directory,network.value("host").toString("127.0.0.1"),quint16(network.value("port").toInt(8212)),this);
    const auto refreshWarning=[this,registry_dialog] { registry_dialog->set_warning(_application.client().network_warning()); };
    refreshWarning();
    connect(&_application,&GuiPresenter::apply_status_changed,registry_dialog,refreshWarning);
    connect(&_application,&GuiPresenter::selected_device_changed,registry_dialog,refreshWarning);
    auto *manage=_commands->register_command({"openEngineRegistry","Manage processors",QKeySequence("Alt+P"),presentation::UiCommandContext::Workspace,
        [registry_dialog] { registry_dialog->show(); registry_dialog->raise(); registry_dialog->activateWindow(); },{}});
    findChild<QMenu *>("settingsMenu")->addAction(manage);
    return manage;
}

QHBoxLayout *MainWindow::setup_processor_corner(presentation::ProcessorDirectoryModel *directory,QAction *manage) {
    auto *deviceCorner=new QWidget(menuBar()); deviceCorner->setObjectName("navigationControls");
    auto *deviceLayout=new QHBoxLayout(deviceCorner); deviceLayout->setContentsMargins(4,0,4,0); deviceLayout->setSpacing(4);
    for (const auto *id : {"pipelinePresets","pipelineToggle"}) if (auto *button=findChild<QToolButton *>(id)) { deviceLayout->addWidget(button); button->show(); }
    auto *engineButton=new presentation::ProcessorSelector(*directory,manage,deviceCorner); deviceLayout->addWidget(engineButton);
    menuBar()->setCornerWidget(deviceCorner);
    static_cast<NavigationMenuBar *>(menuBar())->watchCornerWidget(deviceCorner);
    return deviceLayout;
}

void MainWindow::bind_processor_status(presentation::ProcessorDirectoryModel *directory) {
    auto *permissions=new QLabel(this); permissions->setObjectName("selectedEnginePermission"); statusBar()->addPermanentWidget(permissions);
    const auto refreshPermissions=[directory,permissions] {
        const auto *selected=directory->selected();
        permissions->setText(!selected || selected->local ? QString{} : !selected->available ? ui::text("Unavailable") :
            selected->allow_mutations ? ui::text("Editing allowed") : ui::text("Read-only engine"));
    };
    connect(directory,&presentation::ProcessorDirectoryModel::changed,permissions,refreshPermissions);
    connect(&ui::Language::instance(),&ui::Language::changed,permissions,refreshPermissions); refreshPermissions();
}

void MainWindow::bind_processor_selection(presentation::ProcessorDirectoryModel *directory) {
    connect(directory,&presentation::ProcessorDirectoryModel::selection_requested,this,[this](const presentation::ProcessorEndpoint &endpoint) {
        if (endpoint.id==_application.device_id()) return;
        if (!endpoint.local && (endpoint.address.isEmpty() || !endpoint.port)) { statusBar()->showMessage(ui::text("Engine endpoint is unavailable"),5000); return; }
        if (auto *editor=data_flow_editor()) { editor->flush_presentation(); _application.state().file_configs.scene_graph=editor->document(); }
        _application.select_device(endpoint.id,endpoint.address,endpoint.port);
    });
    connect(&_application,&GuiPresenter::selected_device_changed,directory,[this,directory] { directory->set_selected(_application.device_id()); });
}

void MainWindow::bind_presenter_views() {
    connect(&_application, &GuiPresenter::active_graph_changed, this, &MainWindow::sync_node_views);
    connect(&_application, &GuiPresenter::selected_device_changed, this, [this] { _view_draft.reset(); });
    connect(&_application, &GuiPresenter::numeric_frame_ready, this, &MainWindow::consume_numeric_frame);
    connect(&_application, &GuiPresenter::numeric_history_ready, this, &MainWindow::consume_numeric_history);
    connect(&_application, &GuiPresenter::numeric_history_snapshot_ready, this, &MainWindow::consume_numeric_history_snapshot);
    connect(&_application, &GuiPresenter::views_cleared, this, &MainWindow::clear_node_views);
}

void MainWindow::setup_theme_selector(QHBoxLayout *deviceLayout) {
    auto *themeSelector=create_theme_combo();
    setup_theme_menu(themeSelector,deviceLayout);
    bind_theme_combo(themeSelector,QStringList{"system", "light", "dark"});
    connect(&_application, &GuiPresenter::command_rejected, this, [this](quint64, const QString &error) {
        statusBar()->showMessage(ui::text("Engine command rejected: %1").arg(error), 10000);
    });
}

QComboBox *MainWindow::create_theme_combo() {
    auto *themeSelector = new QComboBox(this);
    themeSelector->setObjectName("statusTheme");
    ui::items(themeSelector, {"System theme", "Light", "Dark"});
    const QStringList themeCodes{"system", "light", "dark"};
    for (int i = 0; i < themeCodes.size(); ++i)
        themeSelector->setItemData(i, themeCodes[i]);
    ui::bind(themeSelector, "toolTip", "Interface theme");
    themeSelector->setMaximumWidth(160);
    themeSelector->hide();
    return themeSelector;
}

void MainWindow::setup_theme_menu(QComboBox *themeSelector,QHBoxLayout *deviceLayout) {
    auto *deviceCorner = deviceLayout->parentWidget();
    auto *themeButton = ui::tool_button(deviceCorner,"Theme");
    themeButton->setObjectName("themeMenuButton");
    auto *themeMenu = new QMenu(themeButton);
    auto *themeActions = new QActionGroup(themeMenu);
    themeActions->setExclusive(true);
    register_theme_actions(themeMenu,themeSelector,themeActions);
    connect(themeMenu, &QMenu::aboutToShow, themeMenu, [themeActions, themeSelector] {
        for (const auto actions = themeActions->actions(); auto *action : actions) {
            const int index = action->data().toInt();
            action->setText(themeSelector->itemText(index));
            action->setChecked(themeSelector->currentIndex() == index);
        }
    });
    ui::button_menu(themeButton,themeMenu);
    deviceLayout->insertWidget(2,themeButton);
}
void MainWindow::register_theme_actions(QMenu* themeMenu,QComboBox* themeSelector,QActionGroup* themeActions) {
    const QStringList themeCodes{"system", "light", "dark"};
    for (int index = 0; index < themeCodes.size(); ++index) {
        auto *action = themeMenu->addAction(themeSelector->itemText(index));
        action->setCheckable(true); action->setData(index); themeActions->addAction(action);
        connect(action, &QAction::triggered, themeSelector, [themeSelector, index] { themeSelector->setCurrentIndex(index); });
    }
}

void MainWindow::bind_theme_combo(QComboBox *themeSelector,const QStringList &themeCodes) {
    auto syncTheme = [this, themeSelector, themeCodes] {sync_theme_combo(themeSelector,themeCodes);};
    syncTheme();
    connect(&ui::Theme::instance(), &ui::Theme::changed, this, syncTheme);
    connect(themeSelector, &QComboBox::currentIndexChanged, this, [this, themeCodes](int index) {
        if (index < 0 || index >= themeCodes.size())
            return;
        if (ui::Theme::instance().set(themeCodes[index])) {
            auto &ui = _application.state().file_configs.ui;
            ui.theme = themeCodes[index];
            if (ui.autosave_enabled)
                emit settings_change_requested({ui::SettingsChangeScope::ApplicationPreferences,false});
        }
    });
}
void MainWindow::sync_theme_combo(QComboBox* themeSelector,const QStringList& themeCodes) {
    const auto code = ui::Theme::instance().code();
    QSignalBlocker block(themeSelector);
    themeSelector->setCurrentIndex(std::max(0, int(themeCodes.indexOf(code))));
    if (auto *settingsTheme = findChild<QComboBox *>("interfaceTheme")) {
        QSignalBlocker settingsBlock(settingsTheme);
        settingsTheme->setCurrentIndex(QStringList{"system", "dark", "light"}.indexOf(code));
    }
}

void MainWindow::setup_render_telemetry() {
    auto *fps_timer = new QTimer(this);
    fps_timer->setInterval(500);
    connect(fps_timer, &QTimer::timeout, this, &MainWindow::refresh_fps);
    for (auto *chart : {_raw_signal_chart, _window_signal_chart, _windowed_signal_chart, _window_function_chart,
                        _amplitudes_chart, _test_amplitudes_chart})
        if (auto *view = dynamic_cast<FPSChartView *>(chart))
            connect(view, &FPSChartView::fps_changed, this, &MainWindow::refresh_fps);
    refresh_presentation_preferences();
    connect(findChild<SettingsWidget *>(), &SettingsWidget::presentation_preferences_changed, this,
            &MainWindow::refresh_presentation_preferences);
    fps_timer->start();
    bind_display_refresh();
    bind_status_visibility();
}

void MainWindow::refresh_fps() {
    if (!_application.state().file_configs.ui.show_fps) return;
    for (const auto docks=workspaceDocks(); auto *dock:docks)
        if(is_graph_dock(dock) && !dock->property("renderExposed").toBool()) dock->setRenderFPS(0,true);
    refresh_static_chart_fps();
    refresh_node_view_fps();
}

void MainWindow::refresh_static_chart_fps() {
        const std::pair<const char *, AbstractChartView *> charts[]{{"rawSamples", _raw_signal_chart},
                                                                    {"windowSignal", _window_signal_chart},
                                                                    {"windowedSignal", _windowed_signal_chart},
                                                                    {"windowFunction", _window_function_chart},
                                                                    {"fft", _amplitudes_chart},
                                                                    {"mel", _test_amplitudes_chart}};
        for (auto [id, chart] : charts) {
            if (!_render_visible.value(id)) continue;
            if (auto *panel = qobject_cast<GraphPanel *>(_panels[id]->widget()); panel && panel->isVisible())
                _panels[id]->setRenderFPS(panel->display_fps(),true);
            else if (chart->isVisible())
                if (auto *view = dynamic_cast<FPSChartView *>(chart))
                    _panels[id]->setRenderFPS(view->display_fps(),true);
        }
        refresh_spectrogram_fps();
}
void MainWindow::refresh_spectrogram_fps() {
        if (_render_visible.value("spectrogram")) {
            if (auto *panel = qobject_cast<GraphPanel *>(_panels["spectrogram"]->widget()); panel && panel->isVisible())
                _panels["spectrogram"]->setRenderFPS(panel->display_fps(),true);
            else if (_spectrogram_view->isVisible())
                _panels["spectrogram"]->setRenderFPS(_spectrogram_view->display_fps(),true);
        }
}

void MainWindow::refresh_node_view_fps() {
        for (auto it = _node_views.cbegin(); it != _node_views.cend(); ++it) {
            const auto &view = it.value();
            if (!view.dock || !_render_visible.value(it.key())) continue;
            if (auto *panel = qobject_cast<GraphPanel *>(view.dock->widget()); panel && panel->isVisible())
                view.dock->setRenderFPS(panel->display_fps(),true);
            else if (view.spectrogram && view.spectrogram->isVisible())
                view.dock->setRenderFPS(view.spectrogram->display_fps(),true);
            else if (auto *chart = dynamic_cast<FPSChartView *>(view.chart); chart && chart->isVisible())
                view.dock->setRenderFPS(chart->display_fps(),true);
        }
}

void MainWindow::refresh_presentation_preferences() {
        apply_render_rate();
        if (auto *editor = data_flow_editor()) editor->sync_preferences();
        if (!_application.state().file_configs.ui.show_fps) {
            for(const auto docks=workspaceDocks(); auto *dock:docks) dock->setRenderFPS(0,false);
        } else
            refresh_fps();
}

void MainWindow::bind_display_refresh() {
    const auto watchScreen = [this](QScreen *display) {
        connect(display, &QScreen::refreshRateChanged, this, [this] { apply_render_rate(); });
    };
    for (const auto screens = QGuiApplication::screens(); auto *display : screens) watchScreen(display);
    connect(qApp, &QGuiApplication::screenAdded, this, watchScreen);
    QTimer::singleShot(0, this, [this] {
        if (windowHandle())
            connect(windowHandle(), &QWindow::screenChanged, this, [this] { apply_render_rate(); });
        apply_render_rate();
    });
}

void MainWindow::bind_status_visibility() {
    statusBar()->setSizeGripEnabled(false);
    auto refreshStatusVisibility=[this] {
        statusBar()->setVisible(!statusBar()->currentMessage().isEmpty());
    };
    refreshStatusVisibility();
    connect(statusBar(),&QStatusBar::messageChanged,this,refreshStatusVisibility);
}

MainWindow::MainWindow(GuiPresenter &application, std::shared_ptr<presentation::ProcessorDiscoveryPort> discovery) : _application(application) {
    setup_command_registry();
    ui::Theme::instance().set(_application.state().file_configs.ui.theme);
    ui::Language::instance().set(_application.state().file_configs.ui.language);
    ui::bind(this, "windowTitle", "Barelief | Signal Processing & Visualization");
    setMinimumSize(800, 600);
    resize(1000, 800);
    setup_numeric_render_timer();
    setup_ui();
    apply_render_rate();
    _numeric_render_timer.start();
    setup_editor_commands();
    qApp->installEventFilter(this);
    setup_navigation_commands();
    bind_workspace_tracking();
    auto *deviceLayout=setup_processor_selector(std::move(discovery));
    bind_presenter_views();
    setup_theme_selector(deviceLayout);
    setup_render_telemetry();
}
void MainWindow::bind_workspace_tracking() {
    connect(this,&WorkspaceWindow::graphAreaActivated,this,[this](WorkspaceDock *dock) {
        if (!_restoring_layout && is_graph_dock(dock) && in_editor_window(dock)) _last_graph_slot=dock;
    });
}

void MainWindow::setup_command_registry() {
    _commands = new presentation::UiCommandRegistry(this);
    _commands->set_workspace_guard([this] {
        auto *active = QApplication::activeWindow();
        if (active == this) return true;
        for (const auto docks = workspaceDocks(); auto *dock : docks) if (dock->window() == active) return true;
        return false;
    });
}

void MainWindow::setup_numeric_render_timer() {
    _numeric_render_timer.setParent(this);
    _numeric_render_timer.setObjectName("numericRenderTimer");
    _numeric_render_timer.setTimerType(Qt::PreciseTimer);
    connect(&_numeric_render_timer,&QTimer::timeout,this,[this] {
        core::perf::Scope profile(core::perf::Stage::NumericRenderTimer);
        if(!_numeric_render_dirty)return;
        _numeric_render_dirty=false;render_numeric_views(true);
    });
}

MainWindow::~MainWindow() {
    delete _commands;
    _commands = nullptr;
    disconnect(workspace(),nullptr,this,nullptr);
    qApp->removeEventFilter(this);
    _restoring_layout = true;
    _workspace_save_timer.stop();
    _camera_save_timer.stop();
    disconnect_panel_callbacks();
    if (auto *editor = data_flow_editor())
        editor->flush_presentation();
    if (!_workspace_closed) {
        save_workspace();
        if (_application.state().file_configs.ui.autosave_enabled)
            emit settings_change_requested({ui::SettingsChangeScope::SessionWorkspace,false});
    }
}
void MainWindow::disconnect_panel_callbacks() {
    for (auto *panel : std::as_const(_panels)) {
        disconnect(panel, nullptr, this, nullptr);
        disconnect(panel->toggleViewAction(), nullptr, this, nullptr);
    }
    // Floating children close during QWidget teardown, after our maps have
    // already been destroyed. Their visibility callbacks must stop here.
    for(const auto &view:std::as_const(_node_views)) disconnect(view.dock,nullptr,this,nullptr);
}

static const char* configuration_state_key(MainWindow::ConfigurationSaveState state) {
    switch(state) {
    case MainWindow::ConfigurationSaveState::Unsaved:return "Unsaved configuration";
    case MainWindow::ConfigurationSaveState::Saving:return "Saving configuration…";
    case MainWindow::ConfigurationSaveState::Saved:return "Configuration saved to disk";
    case MainWindow::ConfigurationSaveState::Error:return "Configuration save error";
    case MainWindow::ConfigurationSaveState::LoadError:return "Configuration could not be opened";
    }
    return "Unsaved configuration";
}
void MainWindow::set_configuration_save_state(ConfigurationSaveState state, const QString &path, const QString &error) {
    _configuration_save_state = state;
    _configuration_save_path = path;
    _configuration_save_error = error;
    if (!_configuration_save_status) return;
    const char *key=configuration_state_key(state);
    _configuration_save_status->setText(ui::text(key));
    _configuration_save_status->setToolTip(error.isEmpty() ? path : path + "\n" + error);
    _configuration_save_status->setProperty("saveState", int(state));
    _configuration_save_status->setAccessibleName(ui::text(key));
}

LineChartView *MainWindow::make_line_panel(const char *id,const char *title,double minimum,double maximum,
                                           bool auto_gain,double gain_floor,bool UIApplicationConfig::*flag) {
    auto *chart=new LineChartView();
    chart->setTitle(QString());
    chart->set_range(minimum,maximum);
    if(auto_gain) {
        chart->set_auto_resizing(true);
        chart->set_auto_gain(true,1,gain_floor);
    }
    add_panel(id,title,chart,flag);
    return chart;
}

void MainWindow::setup_static_charts() {
    _raw_signal_chart=make_line_panel("rawSamples","Raw Samples",-1,1,true,-1,&UIApplicationConfig::show_raw_samples);
    _window_signal_chart=make_line_panel("windowSignal","Window signal",-1,1,true,-1,&UIApplicationConfig::show_window_signal);
    _windowed_signal_chart=make_line_panel("windowedSignal","Windowed signal",-1,1,true,-1,&UIApplicationConfig::show_windowed_signal);
    _window_function_chart=make_line_panel("windowFunction","Window function",-0.2,1.2,false,0,&UIApplicationConfig::show_window_function);
    _amplitudes_chart = new FreqChartView();
    _amplitudes_chart->setTitle(QString());
    _amplitudes_chart->set_range(0, 1);
    _amplitudes_chart->set_auto_resizing(true);
    _amplitudes_chart->set_auto_gain(true, 1, 0);

    add_panel("fft", "FFT spectrum", _amplitudes_chart, &UIApplicationConfig::show_amplitudes);

    _test_amplitudes_chart=make_line_panel("mel","Mel spectrum",0,1,true,0,&UIApplicationConfig::show_test_amplitudes);
}

void MainWindow::setup_spectrogram() {
    // 3D Spectrogram
    _spectrogram_view = new SpectrogramGraph(this);
    connect_surface_status(_spectrogram_view, "spectrogram");
    _spectrogram_view->set_presentation_state(
        _application.state().file_configs.ui.graph_views.value("spectrogram").toObject().value("renderer").toObject());
    _spectrogram_view->set_camera_state(_application.state().file_configs.ui.camera);
    _spectrogram_view->set_camera_controls(_application.state().file_configs.ui.camera_controls);
    _spectrogram_view->set_side_panel_ratio(_application.state().file_configs.ui.spectrogram_side_panel_ratio);
    bind_spectrogram_state();
    add_panel("spectrogram", "Spectrogram graph", _spectrogram_view, &UIApplicationConfig::show_spectrogram);
}

void MainWindow::store_spectrogram_presentation(const QJsonObject& state) {
    auto prefs=_application.state().file_configs.ui.graph_views.value("spectrogram").toObject();
    if(prefs.value("renderer").toObject()==state)return;
    prefs["renderer"]=state;
    _application.state().file_configs.ui.graph_views["spectrogram"]=prefs;
    _camera_save_timer.start();
}
void MainWindow::bind_spectrogram_state() {
    _camera_save_timer.setSingleShot(true);
    _camera_save_timer.setInterval(1500);
    connect(&_camera_save_timer, &QTimer::timeout, this, [this] {
        if(QGuiApplication::mouseButtons()!=Qt::NoButton){_camera_save_timer.start();return;}
        emit settings_change_requested({ui::SettingsChangeScope::ProjectPresentation,false});
    });
    connect(_spectrogram_view, &SpectrogramGraph::presentation_changed,
        this, &MainWindow::store_spectrogram_presentation);
    connect(_spectrogram_view, &SpectrogramGraph::camera_changed, this, [this](const QJsonObject &state) {
        // Navigation does not dirty the project; Save/close captures this state.
        _application.state().file_configs.ui.camera = state;
    });
    connect(_spectrogram_view, &SpectrogramGraph::side_panel_resized, this, [this](float ratio) {
        _application.state().file_configs.ui.spectrogram_side_panel_ratio = ratio;
        _camera_save_timer.start();
    });
}

std::pair<SettingsWidget *,QMenu *> MainWindow::setup_settings_dialog() {
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
    auto *settings_menu=setup_settings_menu(settings_dialog,settings_widget);
    bind_settings_signals(settings_widget);
    bind_static_graph_controls();
    return {settings_widget,settings_menu};
}

QMenu *MainWindow::setup_settings_menu(QDialog *settings_dialog,SettingsWidget *settings_widget) {
    auto *settings_menu = new QMenu(this);
    settings_menu->setObjectName("settingsMenu");
    ui::bind(settings_menu, "title", "Settings");
    for (const auto &section : SettingsWidget::available_sections()) {
        const auto id=section.section==SettingsWidget::General ? QString("openSettings") : QString("openSettingsSection%1").arg(int(section.section));
        settings_menu->addAction(_commands->register_command({id,section.title_key,QKeySequence(QString::fromLatin1(section.default_shortcut)),presentation::UiCommandContext::Workspace,
            [settings_dialog,settings_widget,index=section.section] {
                settings_widget->select_section(index); settings_dialog->show(); settings_dialog->raise(); settings_dialog->activateWindow();
            },{}}));
    }
    return settings_menu;
}

void MainWindow::bind_settings_signals(SettingsWidget *settings_widget) {
    connect(settings_widget, &SettingsWidget::camera_controls_changed, this,&MainWindow::update_camera_controls_from_settings);
    connect(settings_widget, &SettingsWidget::reset_camera_requested, _spectrogram_view,
            &SpectrogramGraph::reset_camera);
    connect(settings_widget, &SettingsWidget::surface_settings_changed, this, &MainWindow::apply_surface_settings);
    connect(settings_widget, &SettingsWidget::settings_change_requested, this, &MainWindow::settings_change_requested);
    connect(settings_widget, &SettingsWidget::start_wled_server_requested, this,
            &MainWindow::start_wled_server_requested);
    connect(settings_widget, &SettingsWidget::stop_wled_server_requested, this,
            &MainWindow::stop_wled_server_requested);
    connect(settings_widget, &SettingsWidget::wled_server_config_changed, this,
            &MainWindow::wled_server_config_changed);
}
void MainWindow::update_camera_controls_from_settings() {
    const auto controls = _application.state().file_configs.ui.camera_controls;
    _spectrogram_view->set_camera_controls(controls);
    for(const auto docks=graph_docks(); auto* dock:docks)if(auto* panel=qobject_cast<GraphPanel*>(dock->widget()))
        if(auto* surface=panel->owned_surface())surface->set_camera_controls(controls);
    if (auto *editor = data_flow_editor())
        editor->set_navigation_speed(CameraControls::movementSpeed(controls));
}

void MainWindow::bind_static_graph_controls() {
    for(auto it=_panels.begin();it!=_panels.end();++it)if(auto* panel=qobject_cast<GraphPanel*>(it.value()->widget())) {
        panel->set_numeric_plane_count(0,0);
        connect(panel,&GraphPanel::channel_requested,this,[this,key=it.key()](int plane){select_numeric_plane(key,std::size_t(std::max(0,plane)));});
    }
}

std::pair<SchemeEditorWidget *,QAction *> MainWindow::setup_editor_panel(SettingsWidget *settings_widget) {
    auto *editor = new SchemeEditorWidget(_application, this);
    settings_widget->set_execution_settings(editor->execution_settings_widget(settings_widget));
    connect(editor,&SchemeEditorWidget::default_graph_requested,this,&MainWindow::default_graph_requested);
    connect(editor, &SchemeEditorWidget::status_message, this, [this](const QString &text) {
        if (text != ui::text("Data Flow applied"))
            statusBar()->showMessage(text, 5000);
    });
    auto *pipeline_action=setup_pipeline_panel(editor,settings_widget);
    bind_editor_graph_navigation(editor);
    return {editor,pipeline_action};
}

QAction *MainWindow::setup_pipeline_panel(SchemeEditorWidget *editor,SettingsWidget *settings_widget) {
    auto *pipeline_panel = add_panel("dataFlow", "Data Flow", editor, &UIApplicationConfig::show_data_flow);
    setup_graph_dock_controls(pipeline_panel);
    bind_workspace_position_changes(settings_widget);
    auto *pipeline_action = pipeline_panel->toggleViewAction();
    pipeline_action->setObjectName("openPipeline");
    _commands->register_command({"openPipeline","Data Flow",QKeySequence("Alt+D"),presentation::UiCommandContext::Workspace,{},{}},pipeline_action);
    ui::bind(pipeline_action,"toolTip","Show or hide Data Flow");
    auto *pipeline_toggle = ui::tool_button(this);
    pipeline_toggle->setObjectName("pipelineToggle");
    pipeline_toggle->setDefaultAction(pipeline_action);
    editor->set_navigation_speed(CameraControls::movementSpeed(_application.state().file_configs.ui.camera_controls));
    connect(pipeline_action, &QAction::triggered, this, [pipeline_panel](bool visible) {
        if (visible) pipeline_panel->raise();
    });
    return pipeline_action;
}
void MainWindow::bind_workspace_position_changes(SettingsWidget* settings_widget) {
    connect(settings_widget,&SettingsWidget::workspace_position_changed,this,[this] {
        const bool restoring=_restoring_layout; _restoring_layout=true;
        rebuild_graph_areas(nullptr,nullptr,-1);

        _restoring_layout=restoring; refresh_subscriptions(); _workspace_save_timer.start();
    });
}

void MainWindow::bind_editor_graph_navigation(SchemeEditorWidget *editor) {
    connect(editor,&SchemeEditorWidget::graph_view_requested,this,&MainWindow::open_graph_node);
    connect(editor,&SchemeEditorWidget::graph_rename_requested,this,[this](int node) {
        auto* dock=graph_dock(node);
        if(!dock){open_graph_node(node);dock=graph_dock(node);}
        if(dock)request_graph_rename(dock);
    });
    connect(editor,&SchemeEditorWidget::graph_open_requested,this,&MainWindow::add_graph_node);
    editor->set_graph_menu_provider([this](int node,QMenu *menu) { populate_graph_menu(node,menu); });
    editor->set_preset_presentation_provider([this]{return capture_preset_presentation();});
    connect(editor,&SchemeEditorWidget::preset_presentation_requested,this,&MainWindow::restore_preset_presentation);
    if(auto *presets=editor->findChild<QToolButton *>("pipelinePresets")) {
        presets->hide(); // Added to the shared upper navigation bar by the constructor.
    }
}

bool MainWindow::restore_serialized_workspace(const UIApplicationConfig& saved_ui) {
    const auto workspace=saved_ui.workspace;
    if(workspace.value("version").toInt()!=1)return false;
    const auto geometry=QByteArray::fromBase64(workspace.value("geometry").toString().toLatin1());
    if(!geometry.isEmpty())restoreGeometry(geometry);
    const int layout_version=workspace.value("layout_version").toInt();
    if(!restore_workspace_state(workspace,layout_version))return false;
    _application.state().file_configs.ui=saved_ui;
    restore_workspace_dock_visibility(workspace.value("visibility").toObject(),layout_version);
    sync_workspace_config_visibility();
    return true;
}
bool MainWindow::restore_workspace_state(const QJsonObject& workspace,int layout_version) {
    const auto state=QByteArray::fromBase64(workspace.value(workspace.contains("ads_state")?"ads_state":"state").toString().toLatin1());
    if(layout_version<2 || layout_version>5 || state.isEmpty())return false;
    return workspace.contains("ads_state")?restoreState(state,1):importLegacyState(state,1,layout_version==2);
}

void MainWindow::restore_workspace_dock_visibility(const QJsonObject& visibility,int layout_version) {
    for(auto it=_panels.begin();it!=_panels.end();++it)
        if(layout_version>=4 && visibility.value(it.key()).isBool() &&
           it.value()->isClosed()==visibility.value(it.key()).toBool())
            it.value()->toggleView(visibility.value(it.key()).toBool());
    for(auto it=_node_views.begin();it!=_node_views.end();++it) {
        const auto identity="nodeView_"+it.key();
        // ADS restored the current tab already. Opening an already
        // open dock raises it and would overwrite that saved selection.
        if(layout_version>=4 && visibility.value(identity).isBool() &&
           it.value().dock->isClosed()==visibility.value(identity).toBool())
            it.value().dock->toggleView(visibility.value(identity).toBool());
        auto presentation=_application.state().file_configs.ui.graph_views[it.key()].toObject();
        presentation["open"]=!it.value().dock->isClosed();
        _application.state().file_configs.ui.graph_views[it.key()]=presentation;
    }
}

void MainWindow::sync_workspace_config_visibility() {
    // The Qt dock state is authoritative for older workspaces. Legacy
    // flags may describe parent/tab visibility rather than an open dock.
    auto &ui=_application.state().file_configs.ui;
    ui.show_spectrogram=!_panels["spectrogram"]->isHidden();
    ui.show_data_flow=!_panels["dataFlow"]->isHidden();
    ui.show_amplitudes=!_panels["fft"]->isHidden();
    ui.show_test_amplitudes=!_panels["mel"]->isHidden();
    ui.show_raw_samples=!_panels["rawSamples"]->isHidden();
    ui.show_window_signal=!_panels["windowSignal"]->isHidden();
    ui.show_windowed_signal=!_panels["windowedSignal"]->isHidden();
    ui.show_window_function=!_panels["windowFunction"]->isHidden();
}

void MainWindow::setup_workspace_layout() {
    setup_workspace_save_timer();
    const auto saved_ui = _application.state().file_configs.ui;
    reset_workspace();
    restore_node_views();
    const bool restored_workspace=restore_serialized_workspace(saved_ui);
    if (!restored_workspace)restore_default_workspace_visibility(saved_ui);
    _restoring_layout = false;
    QTimer::singleShot(0, this, [this, restored_workspace] {
        recover_offscreen_panels();
        if (!restored_workspace && !_panels["spectrogram"]->isHidden() && !_panels["dataFlow"]->isHidden())
            resizeDocks({_panels["spectrogram"], _panels["dataFlow"]}, {560, 320}, Qt::Vertical);
        refresh_subscriptions();
    });
    connect(qApp, &QGuiApplication::screenRemoved, this, [this] { recover_offscreen_panels(); });
}
namespace {
QJsonObject workspace_save_state(const UIApplicationConfig& ui) {
    auto views=ui.graph_views;
    for(auto it=views.begin();it!=views.end();++it) {
        auto preferences=it.value().toObject();preferences.remove("camera");
        it.value()=preferences;
    }
    return {{"workspace",ui.workspace},{"graph_views",views}};
}
}
void MainWindow::setup_workspace_save_timer() {
    _workspace_save_timer.setSingleShot(true);
    _workspace_save_timer.setInterval(2000);
    connect(&_workspace_save_timer, &QTimer::timeout, this,
        [this,last=workspace_save_state(_application.state().file_configs.ui)]() mutable {
        // A move/resize is not complete while the mouse is held down.
        if(QGuiApplication::mouseButtons()!=Qt::NoButton){_workspace_save_timer.start();return;}
        save_workspace();
        auto current=workspace_save_state(_application.state().file_configs.ui);
        if(current==last)return;
        last=std::move(current);
        emit settings_change_requested({ui::SettingsChangeScope::SessionWorkspace,false});
    });
}

void MainWindow::restore_default_workspace_visibility(const UIApplicationConfig& saved_ui) {
    // A damaged serialized layout can partly mutate docks before failing.
    reset_workspace();
    // Build the default dock topology without replacing the loaded visibility
    // choices (including the first-run demonstration's explicit view set).
    auto& ui=_application.state().file_configs.ui;
    ui.show_spectrogram=saved_ui.show_spectrogram;
    ui.show_data_flow=saved_ui.show_data_flow;
    ui.show_amplitudes=saved_ui.show_amplitudes;
    ui.show_test_amplitudes=saved_ui.show_test_amplitudes;
    ui.show_raw_samples=saved_ui.show_raw_samples;
    ui.show_window_signal=saved_ui.show_window_signal;
    ui.show_windowed_signal=saved_ui.show_windowed_signal;
    ui.show_window_function=saved_ui.show_window_function;
    restore_default_node_visibility(saved_ui);
}

void MainWindow::restore_default_node_visibility(const UIApplicationConfig& saved_ui) {
    const auto choices=saved_ui.workspace.value("visibility").toObject();
    // restore_node_views already filters these docks by the loaded device/graph.
    for(auto it=_node_views.begin();it!=_node_views.end();++it) {
        const auto stored=choices.value("nodeView_"+it.key());
        const auto preferences=saved_ui.graph_views.value(it.key()).toObject();
        const bool open=it->device==_application.device_id() && (stored.isBool()
            ?stored.toBool():preferences.value("open").toBool(false));
        if(it->dock->isClosed()==open)it->dock->toggleView(open);
    }
}

QMenu *MainWindow::setup_menu_bar_status() {
    auto *navigation = new NavigationMenuBar(this);
    setMenuBar(navigation);
    _configuration_save_status = new MenuRowStatus(navigation);
    _configuration_save_status->setObjectName("configurationSaveStatus");
    _configuration_save_status->setAlignment(Qt::AlignCenter);
    _configuration_save_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    navigation->setCenterWidget(_configuration_save_status);
    connect(&ui::Language::instance(), &ui::Language::changed, this, [this] {
        set_configuration_save_state(_configuration_save_state, _configuration_save_path, _configuration_save_error);
    });
    set_configuration_save_state(ConfigurationSaveState::Unsaved);
    auto *file_menu = menuBar()->addMenu(ui::text("File"));
    file_menu->setObjectName("fileMenu");
    ui::bind(file_menu, "title", "File");
    return file_menu;
}

QAction *MainWindow::setup_save_action(QMenu *file_menu) {
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
    return saveAction;
}

QAction *MainWindow::setup_save_as_action(QMenu *file_menu) {
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
    return saveAs;
}

QAction *MainWindow::setup_open_action(QMenu *file_menu) {
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
    return open;
}

MainWindow::FileMenuActions MainWindow::setup_file_menu() {
    auto *file_menu=setup_menu_bar_status();
    return {setup_save_action(file_menu),setup_save_as_action(file_menu),setup_open_action(file_menu)};
}

void MainWindow::setup_view_menu(QMenu *settings_menu,SchemeEditorWidget *editor,QAction *pipeline_action,
                                  const FileMenuActions& file_actions) {
    auto *view_menu = menuBar()->addMenu(ui::text("Display"));
    view_menu->setObjectName("graphsMenu");
    ui::bind(view_menu, "title", "Display");
    view_menu->addAction(pipeline_action);
    view_menu->addSeparator();
    auto *reset=setup_reset_layout_action(view_menu);
    auto *resetCamera=setup_reset_camera_action(view_menu);
    view_menu->addSeparator();
    add_editor_view_actions(view_menu,editor);
    menuBar()->addMenu(settings_menu);
    register_workspace_menu_actions(file_actions,reset,resetCamera);
    register_editor_menu_actions(editor);
}
void MainWindow::add_editor_view_actions(QMenu* view_menu,SchemeEditorWidget* editor) {
    if(auto *arrange=editor->findChild<QAction *>("arrangePipelineShortcut")) view_menu->addAction(arrange);
    if(auto *center=editor->findChild<QAction *>("fitPipelineShortcut")) view_menu->addAction(center);
    if(auto *presets=editor->presets_button()) view_menu->addMenu(presets->menu());
}

QAction *MainWindow::setup_reset_layout_action(QMenu *view_menu) {
    auto *reset = view_menu->addAction(ui::text("Reset layout"));
    ui::bind(reset, "text", "Reset layout");
    reset->setObjectName("resetWorkspace");
    reset->setShortcut(QKeySequence("Alt+L"));
    ui::bind(reset,"toolTip","Restore the default workspace layout");
    connect(reset, &QAction::triggered, this, [this] {
        reset_workspace();
        _workspace_save_timer.start();
    });
    return reset;
}

QAction *MainWindow::setup_reset_camera_action(QMenu *view_menu) {
    auto *resetCamera=view_menu->addAction(ui::text("Reset camera"));
    resetCamera->setObjectName("resetCameraView");ui::bind(resetCamera,"text","Reset camera");
    resetCamera->setShortcut(QKeySequence("Alt+R"));
    connect(resetCamera,&QAction::triggered,this,&MainWindow::reset_active_surface_camera);
    return resetCamera;
}

void MainWindow::reset_active_surface_camera() {
    for(const auto &key:std::as_const(_active_node_views)) {
        auto *surface=key=="spectrogram" ? _spectrogram_view : _node_views.value(key).spectrogram;
        if(surface && surface->isVisible()) {surface->reset_camera();return;}
    }
}

void MainWindow::register_workspace_menu_actions(const FileMenuActions& file_actions,QAction *reset,QAction *resetCamera) {
    for (const auto &entry : std::initializer_list<std::pair<QAction *, const char *>>{
             {file_actions.save,"Save settings"},{file_actions.save_as,"Save settings as…"},{file_actions.open,"Open settings…"},
             {reset,"Reset layout"},{resetCamera,"Reset camera"}}) {
        _commands->register_command({entry.first->objectName(),entry.second,entry.first->shortcut(),presentation::UiCommandContext::Workspace,{},{}},entry.first);
    }
}

void MainWindow::register_editor_menu_actions(SchemeEditorWidget *editor) {
    for (const auto &entry : std::initializer_list<std::pair<const char *,const char *>>{
             {"arrangePipelineShortcut","Arrange Data Flow"},{"fitPipelineShortcut","Center Data Flow"}}) {
        if (auto *command=editor->findChild<QAction *>(entry.first))
            _commands->register_command({command->objectName(),entry.second,command->shortcut(),presentation::UiCommandContext::SchemaEditor,{},{}},command);
    }
}

void MainWindow::setup_ui() {
    const auto file_actions=setup_file_menu();
    setup_static_charts();
    setup_spectrogram();

    auto [settings_widget,settings_menu]=setup_settings_dialog();
    auto [editor,pipeline_action]=setup_editor_panel(settings_widget);
    setup_view_menu(settings_menu,editor,pipeline_action,file_actions);
    setup_workspace_layout();
    // Инициализируем видимость графиков на основе текущего конфига
    update_chart_visibility(_application.state().file_configs.ui);

    qCInfo(core_app_window, "UI setup completed");
}

