#include "visualization/scene_aspect_control.h"
#include "visualization/auto_range_interval_control.h"
#include "visualization/ui_text.h"
#include "core_app/settings_widget.h"
#include "core_app/ui/field_row.h"
#include "core_app/ui/slider_value_control.h"

#include <QVBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <QtCore/QLoggingCategory>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QTabWidget>
#include <QSignalBlocker>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QPushButton>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QPixmap>
#include <QIcon>
#include <QPainter>
#include <QScreen>
#include <QGuiApplication>
#include <QStandardItemModel>
#include <QInputDialog>
#include <QUuid>
#include <QSet>
#include "visualization/surface_settings.h"

#include "core/utils.h"

Q_LOGGING_CATEGORY(core_app_settings, "core_app.settings")

std::vector<SettingsWidget::SectionDescriptor> SettingsWidget::available_sections() {
    return {{General,"General","Alt+,"}, {Processing,"Audio processing","Alt+2"},
            {Graphs,"Graphs","Alt+3"}, {Controls,"Controls","Alt+6"}};
}

SettingsWidget::SettingsWidget(GuiState &context, QWidget *parent) : QWidget(parent), _context(context) {
    _setup_ui();
    _setup_camera_controls();
    auto *sections = new QTabWidget(this);
    sections->setObjectName("settingsSections");
    auto *outer = qobject_cast<QVBoxLayout *>(layout());
    auto *general = findChild<QScrollArea *>();
    outer->removeWidget(general);
    outer->addWidget(sections);
    auto title_for = [](Section section) {
        for (const auto &descriptor : available_sections())
            if (descriptor.section == section) return QString::fromUtf8(descriptor.title_key);
        return QString{};
    };
    ui::tab(sections, general, title_for(General).toUtf8().constData());
    auto add_section = [sections](QWidget *group, const QString &title) {
        auto *scroll = new QScrollArea(sections);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *page = new QWidget(scroll);
        auto *layout = new QVBoxLayout(page);
        layout->addWidget(group);
        layout->addStretch();
        scroll->setWidget(page);
        ui::tab(sections, scroll, title.toUtf8().constData());
    };
    auto *processing = new QGroupBox(this);
    auto *processing_layout = new QVBoxLayout(processing);
    _processing_layout = processing_layout;
    auto* running=ui::make<QLabel>("Data Flow runs continuously; valid changes apply automatically.",processing);
    running->setWordWrap(true);processing_layout->addWidget(running);
    add_section(processing, title_for(Processing));
    add_section(_setup_graphs(), title_for(Graphs));
    _main_layout->removeWidget(_wled_group);
    add_section(_wled_group, "WLED");
    _main_layout->removeWidget(_network_group);
    add_section(_network_group, "Network");
    // Output settings belong to registered graph parameters. Retain section
    // indices for workspace preferences while hiding obsolete global controls.
    sections->setTabVisible(WLED,false);
    sections->setTabVisible(Network,false);
    auto *controls = findChild<QGroupBox *>("cameraControls");
    _main_layout->removeWidget(controls);
    add_section(controls, title_for(Controls));
    for (int i = _main_layout->count() - 1; i >= 0; --i)
        if (_main_layout->itemAt(i)->spacerItem())
            delete _main_layout->takeAt(i);
    _main_layout->addStretch();
    _load_current_config();
    _connect_signals();
}

void SettingsWidget::set_execution_settings(QWidget *settings) {
    if (settings && _processing_layout) _processing_layout->addWidget(settings);
}

void SettingsWidget::_setup_ui() {
    auto *outer_layout = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    _main_layout = new QVBoxLayout(content);
    scroll->setWidget(content);
    outer_layout->addWidget(scroll);
    auto *language = new QComboBox(this);
    language->setObjectName("interfaceLanguage");
    language->addItem(QString::fromUtf8("Русский"), "ru");
    language->addItem("English", "en");
    language->setCurrentIndex(_context.file_configs.ui.language == "en" ? 1 : 0);
    _main_layout->addWidget(new ui::FieldRow("Interface language", language, this));
    connect(language, &QComboBox::currentIndexChanged, this, [this, language](int index) {
        const auto code = language->itemData(index).toString();
        if (ui::Language::instance().set(code)) {
            _context.file_configs.ui.language = code;
            _on_config_change();
        }
    });
    auto *theme = new QComboBox(this);
    theme->setObjectName("interfaceTheme");
    ui::items(theme, {"System theme", "Dark", "Light"});
    const QStringList themes{"system", "dark", "light"};
    theme->setCurrentIndex(qMax(0, themes.indexOf(_context.file_configs.ui.theme)));
    _main_layout->addWidget(new ui::FieldRow("Interface theme", theme, this));
    connect(theme, &QComboBox::currentIndexChanged, this, [this, themes](int index) {
        if (ui::Theme::instance().set(themes[index])) {
            _context.file_configs.ui.theme = themes[index];
            _on_config_change();
        }
    });
    auto *fps = ui::make<QCheckBox>("Show FPS", this);
    fps->setObjectName("showFPS");
    fps->setChecked(_context.file_configs.ui.show_fps);
    _main_layout->addWidget(fps);
    connect(fps, &QCheckBox::toggled, this, [this](bool checked) {
        _context.file_configs.ui.show_fps = checked;
        emit presentation_preferences_changed();
        _on_config_change();
    });
    // Группа управления отображением
    _ui_group = ui::make<QGroupBox>("UI Settings", this);
    _ui_layout = new QVBoxLayout(_ui_group);

    _move_to_primary_screen_on_start_cb = ui::make<QCheckBox>("Move window to primary screen on startup", this);
    _enable_autosave_cb = ui::make<QCheckBox>("Autosave enabled", this);
    _enable_autosave_cb->setObjectName("enableAutosave");
    _show_raw_samples_cb = ui::make<QCheckBox>("Show Raw Samples", this);
    _show_raw_samples_cb->setObjectName("showRawSamples");
    _show_window_signal_cb = ui::make<QCheckBox>("Show window signal", this);
    _show_window_signal_cb->setObjectName("showWindowSignal");
    _show_windowed_signal_cb = ui::make<QCheckBox>("Show windowed signal", this);
    _show_windowed_signal_cb->setObjectName("showWindowedSignal");
    _show_window_function_cb = ui::make<QCheckBox>("Show window function", this);
    _show_window_function_cb->setObjectName("showWindowFunction");
    _show_amplitudes_cb = ui::make<QCheckBox>("Show Amplitudes (FFT)", this);
    _show_amplitudes_cb->setObjectName("showFFT");
    _show_test_amplitudes_cb = ui::make<QCheckBox>("Show Test Amplitudes (Mel)", this);
    _show_test_amplitudes_cb->setObjectName("showMel");
    _show_spectrogram_cb = ui::make<QCheckBox>("Show spectrogram", this);
    _show_spectrogram_cb->setObjectName("showSpectrogram");

    _ui_layout->addWidget(_move_to_primary_screen_on_start_cb);
    _ui_layout->addWidget(_enable_autosave_cb);
    _ui_layout->addWidget(_show_raw_samples_cb);
    _ui_layout->addWidget(_show_window_signal_cb);
    _ui_layout->addWidget(_show_windowed_signal_cb);
    _ui_layout->addWidget(_show_window_function_cb);
    _ui_layout->addWidget(_show_amplitudes_cb);
    _ui_layout->addWidget(_show_test_amplitudes_cb);
    _ui_layout->addWidget(_show_spectrogram_cb);
    _ui_layout->addStretch();

    _main_layout->addWidget(_ui_group);
    _main_layout->addStretch();

    // Группа WLED AR server
    _wled_group = ui::make<QGroupBox>("WLED AR server", this);
    _wled_layout = new QVBoxLayout(_wled_group);

    _enable_wled_server_cb = ui::make<QCheckBox>("Enable WLED AR server", this);
    _enable_wled_server_cb->setObjectName("enableWLED");
    _wled_layout->addWidget(_enable_wled_server_cb);

    // Выбор сетевого адаптера
    _network_group = ui::make<QGroupBox>("Network", this);
    _network_layout = new QVBoxLayout(_network_group);
    _main_layout->addWidget(_network_group);
    auto *adapter_label = ui::make<QLabel>("Network Adapter:", this);
    _network_layout->addWidget(adapter_label);

    _network_adapter_combo = new QComboBox(this);
    _network_layout->addWidget(_network_adapter_combo);

    // Определение интерфейса по умолчанию через тестовое UDP соединение
    const auto defaultInterfaceAddress = get_default_interfaces();

    // Заполнение списка сетевых адаптеров
    int defaultIndex = -1;
    int savedIndex = -1;
    int currentIndex = 0;
    const QString saved_interface = _context.file_configs.wled_ar_server.network_interface_name;

    const auto networkEntries = get_available_network_interfaces();
    for (const auto &[interface_name, human_readable_name, ip_address] : networkEntries) {
        const QString displayText = QString("%1 (%2)").arg(human_readable_name).arg(ip_address.toString());
        _network_adapter_combo->addItem(displayText, interface_name);

        if (not saved_interface.isEmpty() and interface_name == saved_interface) {
            savedIndex = currentIndex;
        }

        if (not defaultInterfaceAddress.isNull() and ip_address == defaultInterfaceAddress) {
            defaultIndex = currentIndex;
        }
        currentIndex++;
    }

    // Установка интерфейса: приоритет у сохраненного, если он доступен
    if (savedIndex >= 0) {
        _network_adapter_combo->setCurrentIndex(savedIndex);
    } else if (defaultIndex >= 0) {
        // Если сохраненный интерфейс недоступен, устанавливаем системный по умолчанию
        _network_adapter_combo->setCurrentIndex(defaultIndex);
        // Обновляем конфигурацию с новым интерфейсом
        _context.file_configs.wled_ar_server.network_interface_name =
            _network_adapter_combo->itemData(defaultIndex).toString();
    }

    // Порт сервера
    auto *port_layout = new QHBoxLayout();
    auto *port_label = ui::make<QLabel>("Server Port:", this);
    _port_spinbox = new QSpinBox(this);
    _port_spinbox->setObjectName("wledPort");
    _port_spinbox->setMinimum(1);
    _port_spinbox->setMaximum(65535);
    _port_spinbox->setValue(_context.file_configs.wled_ar_server.port);
    port_layout->addWidget(port_label);
    port_layout->addWidget(_port_spinbox);
    port_layout->addStretch();
    _network_layout->addLayout(port_layout);

    _wled_layout->addStretch();

    _main_layout->addWidget(_wled_group);
    _main_layout->addStretch();
}

void SettingsWidget::_load_current_config() const {

    _enable_autosave_cb->setChecked(_context.file_configs.ui.autosave_enabled);
    _move_to_primary_screen_on_start_cb->setChecked(_context.file_configs.ui.move_to_primary_screen_on_start);

    _show_raw_samples_cb->setChecked(_context.file_configs.ui.show_raw_samples);
    _show_window_signal_cb->setChecked(_context.file_configs.ui.show_window_signal);
    _show_windowed_signal_cb->setChecked(_context.file_configs.ui.show_windowed_signal);
    _show_window_function_cb->setChecked(_context.file_configs.ui.show_window_function);
    _show_amplitudes_cb->setChecked(_context.file_configs.ui.show_amplitudes);
    _show_test_amplitudes_cb->setChecked(_context.file_configs.ui.show_test_amplitudes);
    _show_spectrogram_cb->setChecked(_context.file_configs.ui.show_spectrogram);

    // Загрузка настроек WLED AR server
    _enable_wled_server_cb->setChecked(_context.file_configs.wled_ar_server.enabled);
    _port_spinbox->setValue(_context.file_configs.wled_ar_server.port);
}

void SettingsWidget::_connect_signals() {
    // Сигналы управления отображением

    connect(_move_to_primary_screen_on_start_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.move_to_primary_screen_on_start = checked;
        _on_config_change();
    });

    connect(_enable_autosave_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.autosave_enabled = checked;
        // Disabling autosave stops writes immediately; manual Save persists it.
        // Enabling autosave persists the accumulated UI preferences.
        _on_config_change(checked);
    });

    connect(_show_raw_samples_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_raw_samples = checked;
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
        emit on_rebuild_charts();
    });

    connect(_show_window_signal_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_window_signal = checked;
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
        emit on_rebuild_charts();
    });

    connect(_show_windowed_signal_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_windowed_signal = checked;
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
        emit on_rebuild_charts();
    });

    connect(_show_window_function_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_window_function = checked;
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
        emit on_rebuild_charts();
    });

    connect(_show_amplitudes_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_amplitudes = checked;
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
        emit on_rebuild_charts();
    });

    connect(_show_test_amplitudes_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_test_amplitudes = checked;
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
        emit on_rebuild_charts();
    });

    connect(_show_spectrogram_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_spectrogram = checked;
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
        emit on_rebuild_charts();
    });

    // Сигналы WLED AR server
    connect(_enable_wled_server_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.wled_ar_server.enabled = checked;
        if (checked) {
            emit start_wled_server_requested();
        } else {
            emit stop_wled_server_requested();
        }
        _on_config_change(ui::SettingsChangeScope::NodeAdapters);
    });

    connect(_network_adapter_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](const int index) {
        if (index >= 0) {
            _context.file_configs.wled_ar_server.network_interface_name =
                _network_adapter_combo->itemData(index).toString();
            emit wled_server_config_changed();
            _on_config_change(ui::SettingsChangeScope::NodeAdapters);
        }
    });

    connect(_port_spinbox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](const int value) {
        _context.file_configs.wled_ar_server.port = static_cast<quint16>(value);
        emit wled_server_config_changed();
        _on_config_change(ui::SettingsChangeScope::NodeAdapters);
    });
}

void SettingsWidget::_on_config_change(const bool force_save) {
    _on_config_change(ui::SettingsChangeScope::ApplicationPreferences, force_save);
}

void SettingsWidget::_on_config_change(ui::SettingsChangeScope scope, const bool force_save) {
    if (_context.file_configs.ui.autosave_enabled or force_save) {
        emit settings_change_requested({scope, force_save});
    }
}

void SettingsWidget::select_section(int index) {
    const auto descriptors = available_sections();
    if (std::none_of(descriptors.begin(), descriptors.end(), [index](const auto &entry) {
        return entry.section == index;
    })) return;
    if (index == Graphs && _refresh_graph_targets)
        _refresh_graph_targets();
    if (auto *tabs = findChild<QTabWidget *>("settingsSections"); tabs && tabs->isTabVisible(index))
        tabs->setCurrentIndex(index);
}

void SettingsWidget::refresh_graph_preferences() {
    if(_refresh_graph_targets)_refresh_graph_targets();
    if(_refresh_screen_rate)_refresh_screen_rate();
    if(_refresh_dimensions)_refresh_dimensions();
}
void SettingsWidget::set_graph_actions(const QList<QAction *> &actions) {
    QList<QPointer<QAction>> canonical;
    QSet<QAction *> seen;
    for (auto *action : actions)
        if (action && action->isCheckable() && !seen.contains(action)) {
            seen.insert(action);
            canonical.append(action);
        }
    if (!_graph_views_layout || canonical == _graph_actions)
        return;
    _graph_actions = canonical;
    // Row widgets own every connection; replacing rows disconnects the old callbacks.
    while (auto *item = _graph_views_layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    _empty_graph_views = ui::make<QLabel>("No graph views available", _graph_views_layout->parentWidget());
    _empty_graph_views->setObjectName("emptyGraphViews");
    _graph_views_layout->addWidget(_empty_graph_views);
    auto refreshEmpty = [this] {
        const bool any = std::any_of(_graph_actions.cbegin(), _graph_actions.cend(),
                                     [](const auto &action) { return action && action->isVisible(); });
        _empty_graph_views->setVisible(!any);
    };
    int index = 0;
    for (const auto &action : _graph_actions) {
        auto *box = new QCheckBox(_graph_views_layout->parentWidget());
        box->setObjectName("graphViewToggle" + QString::number(index++));
        _graph_views_layout->addWidget(box);
        auto update = [action, box, refreshEmpty] {
            if (!action)
                return;
            const QSignalBlocker block(box);
            box->setText(action->text());
            box->setToolTip(action->toolTip());
            box->setChecked(action->isChecked());
            box->setEnabled(action->isEnabled());
            box->setVisible(action->isVisible());
            refreshEmpty();
        };
        connect(action, &QAction::changed, box, update);
        connect(action, &QAction::destroyed, box, [this, box, refreshEmpty] {
            _graph_views_layout->removeWidget(box);
            box->hide();
            box->setEnabled(false);
            box->deleteLater();
            refreshEmpty();
        });
        connect(box, &QCheckBox::toggled, box, [action](bool checked) {
            if (action && action->isEnabled() && action->isCheckable() && action->isChecked() != checked)
                action->trigger();
        });
        update();
    }
    refreshEmpty();
}

void SettingsWidget::set_display_screen(QScreen *screen) {
    QObject::disconnect(_screen_rate_connection);
    _display_screen = screen;
    if (screen)
        _screen_rate_connection = connect(screen, &QScreen::refreshRateChanged, this, [this] {
            if (_refresh_screen_rate)
                _refresh_screen_rate();
        });
    if (_refresh_screen_rate)
        _refresh_screen_rate();
}
void SettingsWidget::set_graph_dimensions(const QString &identity, size_t frames, size_t bands) {
    const auto dimensions = qMakePair(frames, bands);
    if (_graph_dimensions.value(identity) == dimensions)
        return;
    _graph_dimensions[identity] = dimensions;
    if (_refresh_dimensions)
        _refresh_dimensions();
}

class GradientPreview final : public QWidget {
  public:
    explicit GradientPreview(QWidget *parent) : QWidget(parent) {
        setMinimumHeight(36);
    }
    void setStops(const QGradientStops &stops) {
        _stops = stops;
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const auto area = rect().adjusted(1, 1, -1, -1);
        QLinearGradient gradient(area.topLeft(), area.topRight());
        gradient.setStops(_stops);
        painter.fillRect(area, gradient);
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(area);
    }

  private:
    QGradientStops _stops;
};

static bool editGradient(QWidget *parent, const QJsonObject &settings, QJsonObject &output, QJsonArray &colorPresets) {
    QDialog dialog(parent);
    dialog.setObjectName("gradientEditor");
    ui::bind(&dialog, "windowTitle", "Edit color scale");
    dialog.resize(500, 440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *preview = new GradientPreview(&dialog);
    preview->setObjectName("gradientPreview");
    auto *options = new QFormLayout;
    auto *preset = new QComboBox(&dialog);
    preset->setObjectName("gradientPreset");
    auto savedPresets = colorPresets;
    const QStringList presetCodes{"green_red", "black_yellow", "viridis", "ice_fire", "rainbow"};
    const QStringList presetNames{"Green / Red", "Black / Yellow", "Viridis", "Ice / Fire", "Rainbow"};
    auto rebuildPresets = [&](const QString &selected) {
        const QSignalBlocker block(preset);
        preset->clear();
        for (int i = 0; i < presetCodes.size(); ++i)
            preset->addItem(ui::text(presetNames[i].toUtf8().constData()), presetCodes[i]);
        for (const auto &value : savedPresets) {
            const auto entry = value.toObject();
            if (entry["id"].isString() && !entry["id"].toString().isEmpty() && entry["name"].isString() &&
                !entry["name"].toString().trimmed().isEmpty())
                preset->addItem(entry["name"].toString(), "saved:" + entry["id"].toString());
        }
        preset->setCurrentIndex(preset->findData(selected));
    };
    rebuildPresets(settings["gradient_stops"].toArray().isEmpty() ? settings["gradient"].toString() : QString{});
    QObject::connect(&ui::Language::instance(), &ui::Language::changed, &dialog,
                     [&] { rebuildPresets(preset->currentData().toString()); });
    preset->setPlaceholderText(ui::text("Choose a source preset"));
    bool editableScale = !SurfaceSettings::normalizedStops(settings["gradient_stops"].toArray()).isEmpty();
    QString editingPresetId;
    auto *copyPreset = ui::make<QPushButton>("Create editable copy", &dialog);
    copyPreset->setObjectName("copyColorPreset");
    auto *sourceHelp =
        ui::make<QLabel>("Built-in presets are read-only. Create a copy to change colors or positions.", &dialog);
    sourceHelp->setWordWrap(true);
    auto *draftTitle = new QLabel(&dialog);
    draftTitle->setObjectName("colorScaleEditingState");
    auto *presetTools = new QHBoxLayout;
    presetTools->addWidget(copyPreset);
    auto *savePreset = ui::make<QPushButton>("Save color preset", &dialog);
    savePreset->setObjectName("saveColorPreset");
    auto *deletePreset = ui::make<QPushButton>("Delete color preset", &dialog);
    deletePreset->setObjectName("deleteColorPreset");
    presetTools->addWidget(savePreset);
    presetTools->addWidget(deletePreset);
    auto *spacing = new QComboBox(&dialog);
    spacing->setObjectName("gradientSpacing");
    ui::items(spacing, {"Preset transitions", "Linear transitions", "Logarithmic transitions"});
    const QStringList spacingCodes{"preset", "linear", "log"};
    for (int i = 0; i < spacingCodes.size(); ++i)
        spacing->setItemData(i, spacingCodes[i]);
    options->addRow(ui::make<QLabel>("Source preset", &dialog), preset);
    layout->addLayout(options);
    layout->addLayout(presetTools);
    layout->addWidget(sourceHelp);
    layout->addWidget(draftTitle);
    layout->addWidget(preview);
    auto *editOptions = new QFormLayout;
    editOptions->addRow(ui::make<QLabel>("Transition positions", &dialog), spacing);
    layout->addLayout(editOptions);
    auto *table = new QTableWidget(0, 2, &dialog);
    table->setObjectName("gradientStops");
    table->setHorizontalHeaderLabels({ui::text("Position (%)"), ui::text("Color")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table);
    auto *error = new QLabel(&dialog);
    error->setWordWrap(true);
    layout->addWidget(error);
    auto *tools = new QHBoxLayout;
    auto *add = ui::make<QPushButton>("Add stop", &dialog);
    add->setObjectName("addGradientStop");
    auto *remove = ui::make<QPushButton>("Remove stop", &dialog);
    remove->setObjectName("removeGradientStop");
    auto *reset = ui::make<QPushButton>("Reset", &dialog);
    reset->setObjectName("resetGradientStops");
    tools->addWidget(add);
    tools->addWidget(remove);
    tools->addStretch();
    tools->addWidget(reset);
    layout->addLayout(tools);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    ui::bind(buttons->button(QDialogButtonBox::Save), "text", "Save");
    ui::bind(buttons->button(QDialogButtonBox::Cancel), "text", "Cancel");
    auto current = SurfaceSettings::normalizedStops(settings.value("gradient_stops").toArray());
    preset->setCurrentIndex(preset->findData(current.isEmpty() ? settings["gradient"].toString() : QString{}));
    spacing->setCurrentIndex(std::max(0, int(spacingCodes.indexOf(settings["gradient_layout"].toString("preset")))));
    bool populating = false;
    auto read = [table] {
        QJsonArray values;
        for (int row = 0; row < table->rowCount(); ++row) {
            auto stop = table->item(row, 0)->data(Qt::UserRole).toJsonObject();
            const auto displayed = qobject_cast<QDoubleSpinBox *>(table->cellWidget(row, 0))->value();
            if (qRound(stop["position"].toDouble() * 10000) != qRound(displayed * 100))
                stop["position"] = displayed / 100.0;
            stop["color"] = table->cellWidget(row, 1)->property("stopColor").toString();
            values.append(stop);
        }
        return values;
    };
    auto refresh = [&] {
        QGradientStops stops;
        auto raw = read();
        // Preview stays live while editing, including a temporarily unsorted table.
        std::map<double, QColor> sorted;
        for (const auto &value : raw) {
            auto stop = value.toObject();
            sorted[stop["position"].toDouble()] = QColor(stop["color"].toString());
        }
        for (const auto &[position, color] : sorted)
            stops.append({position, color});
        preview->setStops(stops);
        table->setEnabled(editableScale);
        spacing->setEnabled(editableScale);
        reset->setEnabled(editableScale);
        copyPreset->setEnabled(preset->currentIndex() >= 0);
        draftTitle->setText(ui::text(editableScale ? "Editable color scale" : "Read-only preset preview"));
        add->setEnabled(editableScale && table->rowCount() < 16);
        remove->setEnabled(editableScale && table->currentRow() > 0 && table->currentRow() < table->rowCount() - 1 &&
                           table->rowCount() > 2);
        deletePreset->setEnabled(preset->currentData().toString().startsWith("saved:"));
    };
    auto custom = [&] {
        if (populating || !editableScale)
            return;
        error->clear();
        refresh();
    };
    auto populate = [&](const QJsonArray &values) {
        populating = true;
        table->setRowCount(0);
        for (int row = 0; row < values.size(); ++row) {
            const auto stop = values[row].toObject();
            table->insertRow(row);
            auto *item = new QTableWidgetItem;
            item->setData(Qt::UserRole, stop);
            table->setItem(row, 0, item);
            auto *position = new QDoubleSpinBox(table);
            position->setRange(0, 100);
            position->setDecimals(2);
            position->setSingleStep(1);
            position->setValue(stop["position"].toDouble() * 100);
            position->setEnabled(row > 0 && row < values.size() - 1);
            table->setCellWidget(row, 0, position);
            auto *color = new QPushButton(table);
            color->setProperty("stopColor", stop["color"].toString());
            auto paint = [color] {
                const QColor c(color->property("stopColor").toString());
                QPixmap swatch(18, 18);
                swatch.fill(c);
                color->setIcon(QIcon(swatch));
                color->setText(c.name());
            };
            paint();
            table->setCellWidget(row, 1, color);
            QObject::connect(position, &QDoubleSpinBox::valueChanged, &dialog, custom);
            QObject::connect(color, &QPushButton::clicked, &dialog, [&, color, paint, row] {
                table->setCurrentCell(row, 1);
                const auto selected = ui::select_color(QColor(color->property("stopColor").toString()), &dialog,
                                                       ui::text("Color scale stop color"));
                if (selected.isValid()) {
                    color->setProperty("stopColor", selected.name());
                    paint();
                    custom();
                }
            });
        }
        populating = false;
        refresh();
    };
    auto presetValues = [&] {
        auto value = settings;
        const auto code = preset->currentData().toString();
        value["gradient_stops"] = QJsonArray{};
        if (code.startsWith("saved:")) {
            for (const auto &entry : savedPresets)
                if (entry.toObject()["id"].toString() == code.mid(6)) {
                    value = entry.toObject()["values"].toObject();
                    break;
                }
            const auto custom = SurfaceSettings::normalizedStops(value["gradient_stops"].toArray());
            if (!custom.isEmpty())
                return custom;
        } else
            value["gradient"] = code.isEmpty() ? settings["gradient"] : QJsonValue(code);
        value["gradient_layout"] = spacing->currentData().toString();
        QJsonArray values;
        for (const auto &stop : SurfaceSettings::gradientStops(value))
            values.append(QJsonObject{{"position", stop.first}, {"color", stop.second.name()}});
        return values;
    };
    if (current.isEmpty())
        current = presetValues();
    populate(current);
    QObject::connect(table, &QTableWidget::itemSelectionChanged, &dialog, [&] {
        if (!populating)
            refresh();
    });
    QObject::connect(preset, &QComboBox::currentIndexChanged, &dialog, [&](int index) {
        if (index >= 0) {
            const auto code = preset->currentData().toString();
            editableScale = code.startsWith("saved:");
            editingPresetId = editableScale ? code.mid(6) : QString{};
            if (code.startsWith("saved:"))
                for (const auto &entry : savedPresets)
                    if (entry.toObject()["id"].toString() == code.mid(6)) {
                        const QSignalBlocker block(spacing);
                        spacing->setCurrentIndex(std::max(
                            0, spacing->findData(
                                   entry.toObject()["values"].toObject()["gradient_layout"].toString("preset"))));
                        break;
                    }
            populate(presetValues());
            error->clear();
        }
    });
    QObject::connect(copyPreset, &QPushButton::clicked, &dialog, [&] {
        editableScale = true;
        editingPresetId.clear();
        refresh();
        error->clear();
    });
    QObject::connect(&ui::Language::instance(), &ui::Language::changed, &dialog, [&] {
        refresh();
        preset->setPlaceholderText(ui::text("Choose a source preset"));
    });
    QObject::connect(spacing, &QComboBox::currentIndexChanged, &dialog, [&] {
        if (!editableScale)
            return;
        auto values = read();
        if (spacing->currentData().toString() != "preset")
            for (int i = 0; i < values.size(); ++i) {
                auto stop = values[i].toObject();
                const double p = double(i) / (values.size() - 1);
                stop["position"] = spacing->currentData().toString() == "log" ? std::log10(1 + 9 * p) : p;
                values[i] = stop;
            }
        populate(values);
        error->clear();
    });
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        auto values = SurfaceSettings::normalizedStops(read());
        if (values.isEmpty() || values.size() >= 16)
            return;
        int gap = 0;
        double largest = -1;
        for (int i = 0; i < values.size() - 1; ++i) {
            const auto d =
                values[i + 1].toObject()["position"].toDouble() - values[i].toObject()["position"].toDouble();
            if (d > largest) {
                largest = d;
                gap = i;
            }
        }
        const auto left = values[gap].toObject(), right = values[gap + 1].toObject();
        const QColor a(left["color"].toString()), b(right["color"].toString());
        const auto color =
            QColor::fromRgbF((a.redF() + b.redF()) / 2, (a.greenF() + b.greenF()) / 2, (a.blueF() + b.blueF()) / 2);
        values.insert(gap + 1,
                      QJsonObject{{"position", (left["position"].toDouble() + right["position"].toDouble()) / 2},
                                  {"color", color.name()}});
        populate(values);
        table->setCurrentCell(gap + 1, 0);
        custom();
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int row = table->currentRow();
        if (row > 0 && row < table->rowCount() - 1) {
            auto values = read();
            values.removeAt(row);
            populate(values);
            custom();
        }
    });
    QObject::connect(savePreset, &QPushButton::clicked, &dialog, [&] {
        const auto raw = read(), valid = SurfaceSettings::normalizedStops(raw);
        if (valid.isEmpty() || raw.size() != valid.size()) {
            error->setText(ui::text("Use distinct positions from 0% to 100%, including both ends."));
            return;
        }
        const auto code = preset->currentData().toString();
        bool accepted = false;
        const auto name = QInputDialog::getText(
            &dialog, ui::text("Save color preset"), ui::text("Color preset name"), QLineEdit::Normal,
            !editingPresetId.isEmpty() ? preset->currentText() : QString{}, &accepted);
        if (!accepted)
            return;
        if (name.trimmed().isEmpty()) {
            error->setText(ui::text("Enter a color preset name."));
            return;
        }
        const auto id =
            !editingPresetId.isEmpty() ? editingPresetId : QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject colors{{"gradient", settings["gradient"]},
                                 {"gradient_layout", spacing->currentData().toString()},
                                 {"gradient_stops", valid}};
        savedPresets = SurfaceSettings::saveColorPreset(savedPresets, id, name, colors);
        rebuildPresets("saved:" + id);
        editingPresetId = id;
        editableScale = true;
        refresh();
        error->clear();
    });
    QObject::connect(deletePreset, &QPushButton::clicked, &dialog, [&] {
        const auto code = preset->currentData().toString();
        if (!code.startsWith("saved:"))
            return;
        savedPresets = SurfaceSettings::removeColorPreset(savedPresets, code.mid(6));
        rebuildPresets(QString{});
        editingPresetId.clear();
        editableScale = true;
        refresh();
    });
    QObject::connect(reset, &QPushButton::clicked, &dialog, [&] {
        const QSignalBlocker block(spacing);
        if (preset->currentIndex() < 0) {
            spacing->setCurrentIndex(std::max(0, spacing->findData(settings["gradient_layout"].toString("preset"))));
            const auto original = SurfaceSettings::normalizedStops(settings["gradient_stops"].toArray());
            populate(original.isEmpty() ? presetValues() : original);
        } else
            populate(presetValues());
        error->clear();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto raw = read(), valid = SurfaceSettings::normalizedStops(raw);
        if (valid.isEmpty() || valid.size() != raw.size()) {
            error->setText(ui::text("Use distinct positions from 0% to 100%, including both ends."));
            return;
        }
        colorPresets = savedPresets;
        output = settings;
        output["gradient_layout"] = spacing->currentData().toString();
        if (editableScale)
            output["gradient_stops"] = valid;
        else {
            output["gradient"] = preset->currentData().toString();
            output["gradient_stops"] = QJsonArray{};
        }
        dialog.accept();
    });
    return dialog.exec() == QDialog::Accepted;
}

void SettingsWidget::edit_color_scale(const QString &identity) {
    if (identity.isEmpty())
        return;
    auto &ui = _context.file_configs.ui;
    const auto originalView = ui.graph_views.value(identity).toObject();
    auto presets = ui.graph_rendering.value("color_presets").toArray();
    QJsonObject result;
    if (!editGradient(this, SurfaceSettings::effective(ui.graph_rendering, originalView), result, presets))
        return;
    // Merge into the latest view after modal interaction so presentation changes survive.
    auto view = ui.graph_views.value(identity).toObject();
    auto override = view["surface_override"].toObject();
    // Keep the override sparse: changing a color scale must not pin unrelated
    // lighting/background defaults to their current global values.
    auto values = override["values"].toObject();
    for (const auto *key : {"gradient", "gradient_layout", "gradient_stops"})
        values[key] = result[key];
    override["enabled"] = true;
    override["values"] = values;
    view["surface_override"] = override;
    ui.graph_views[identity] = view;
    ui.graph_rendering["color_presets"] = presets;
    emit surface_settings_changed();
    _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
}

QWidget *SettingsWidget::_setup_graphs() {
    auto *page = new QWidget(this);
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    auto *group = ui::make<QGroupBox>("Default 3D appearance", page);
    group->setObjectName("surfaceGraphSettings");
    pageLayout->addWidget(group);
    auto *form = new QFormLayout(group);
    auto *cadence = ui::make<QGroupBox>("Display refresh rate", page);
    auto *cadenceForm = new QFormLayout(cadence);
    auto *fpsMode = new QComboBox(cadence);
    fpsMode->setObjectName("guiFpsMode");
    ui::items(fpsMode, {"Screen refresh rate", "30 FPS", "60 FPS", "90 FPS", "100 FPS", "120 FPS", "Custom"});
    const QList<int> rates{0, 30, 60, 90, 100, 120, -1};
    for (int i = 0; i < rates.size(); ++i)
        fpsMode->setItemData(i, rates[i]);
    auto *fpsValue = new QSpinBox(cadence);
    fpsValue->setObjectName("guiTargetFps");
    fpsValue->setRange(20, 240);
    fpsValue->setSuffix(" FPS");
    auto *screenRate = new QLabel(cadence);
    screenRate->setObjectName("guiScreenRefreshRate");
    ui::bind(cadence, "toolTip", "Display refresh rate is independent of the analysis rate.");
    cadenceForm->addRow(fpsMode, fpsValue);
    cadenceForm->addRow(screenRate);
    if (auto *showFPS = findChild<QCheckBox *>("showFPS")) {
        _main_layout->removeWidget(showFPS);
        cadenceForm->addRow(showFPS);
    }
    auto *views = ui::make<QGroupBox>("Graph views", page);
    views->setObjectName("graphViewsSettings");
    _graph_views_layout = new QVBoxLayout(views);
    _empty_graph_views = ui::make<QLabel>("No graph views available", views);
    _empty_graph_views->setObjectName("emptyGraphViews");
    _graph_views_layout->addWidget(_empty_graph_views);
    pageLayout->insertWidget(0, views);
    pageLayout->insertWidget(0, cadence);
    auto *workspace=ui::make<QGroupBox>("Workspace",page);
    auto *workspaceForm=new QFormLayout(workspace);
    auto *position=new QComboBox(workspace); position->setObjectName("dataFlowPosition");
    ui::items(position,{"Bottom","Top","Left side","Right side"});
    const QStringList positions{"bottom","top","left","right"};
    for(int i=0;i<positions.size();++i) position->setItemData(i,positions[i]);
    position->setCurrentIndex(std::max(0,int(positions.indexOf(_context.file_configs.ui.workspace.value("data_flow_position").toString("bottom")))));
    workspaceForm->addRow(new ui::FieldRow("Data Flow position",position,workspace));
    pageLayout->insertWidget(0,workspace);
    connect(position,&QComboBox::currentIndexChanged,this,[this,position] {
        _context.file_configs.ui.workspace["data_flow_position"]=position->currentData().toString();
        emit workspace_position_changed(); _on_config_change(ui::SettingsChangeScope::SessionWorkspace);
    });
    const auto initialCadence = SurfaceSettings::normalizedCadence(_context.file_configs.ui.graph_rendering);
    const int initialFps = initialCadence["gui_target_fps"].toInt();
    fpsMode->setCurrentIndex(initialCadence["gui_fps_mode"] == "screen" ? 0
                                                                        : std::max(0, fpsMode->findData(initialFps)));
    if (initialCadence["gui_fps_mode"] != "screen" && fpsMode->findData(initialFps) < 0)
        fpsMode->setCurrentIndex(6);
    fpsValue->setValue(initialFps);
    fpsValue->setVisible(fpsMode->currentIndex() == 6);
    auto refreshScreenRate = [this, screenRate] {
        const auto *display = _display_screen.data();
        const double rate = display && std::isfinite(display->refreshRate()) && display->refreshRate() > 0
                                ? display->refreshRate()
                                : 60;
        screenRate->setText(
            ui::text("Screen: %1 Hz").arg(QString::number(rate, 'f', rate == std::floor(rate) ? 0 : 1)));
    };
    _refresh_screen_rate = refreshScreenRate;
    refreshScreenRate();
    connect(&ui::Language::instance(), &ui::Language::changed, this, refreshScreenRate);
    auto saveCadence = [this, fpsMode, fpsValue] {
        auto schema = _context.file_configs.ui.graph_rendering;
        schema["gui_fps_mode"] = fpsMode->currentIndex() == 0 ? "screen" : "fixed";
        schema["gui_target_fps"] = fpsValue->value();
        _context.file_configs.ui.graph_rendering = SurfaceSettings::normalizedCadence(schema);
        emit surface_settings_changed();
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
    };
    connect(fpsMode, &QComboBox::currentIndexChanged, this, [fpsMode, fpsValue, saveCadence](int index) {
        const QSignalBlocker block(fpsValue);
        fpsValue->setVisible(index == 6);
        const int fixed = fpsMode->currentData().toInt();
        if (fixed > 0)
            fpsValue->setValue(fixed);
        saveCadence();
    });
    connect(fpsValue, &QSpinBox::valueChanged, this, saveCadence);
    auto *precisionRow = new ui::SliderValueControl(group);
    auto *precision = precisionRow->slider();
    precision->setObjectName("surface_axis_precision");
    precision->setRange(0, 8);
    auto *precisionValue = precisionRow->valueLabel();
    precision->setValue(SurfaceSettings::effective(_context.file_configs.ui.graph_rendering)["axis_precision"].toInt());
    precisionValue->setText(QString::number(precision->value()));
    connect(precision, &QSlider::valueChanged, this, [this, precisionValue](int value) {
        precisionValue->setText(QString::number(value));
        auto &schema = _context.file_configs.ui.graph_rendering;
        auto defaults = schema["defaults"].toObject();
        defaults["axis_precision"] = value;
        schema["defaults"] = defaults;
        emit surface_settings_changed();
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
    });
    form->addRow(new ui::FieldRow("Axis decimal places", precisionRow, group));
    auto* sceneAspect=new ui::SceneAspectControl(group);form->addRow(sceneAspect);
    auto* rangeInterval=new ui::AutoRangeIntervalControl(group);form->addRow(rangeInterval);
    rangeInterval->box->setObjectName("surface_auto_range_interval_seconds");
    auto checks = std::make_shared<QMap<QString, QCheckBox *>>();
    const std::pair<const char *, const char *> flags[] = {
        {"mesh_grid", "Surface mesh"}, {"space_grid", "Space grid"}, {"shadows", "Shadows"}, {"light", "Lighting"}};
    for (const auto &[key, title] : flags) {
        auto *box = ui::make<QCheckBox>(title, group);
        box->setObjectName("surface_" + QString(key));
        (*checks)[key] = box;
        if (QString(key) != "mesh_grid")
            form->addRow(box);
    }
    auto numbers = std::make_shared<QMap<QString, QSlider *>>();
    auto numberLabels = std::make_shared<QMap<QString, QLabel *>>();
    const std::pair<const char *, const char *> scalars[] = {
        {"brightness", "Brightness"}, {"light_strength", "Light strength"}, {"ambient_strength", "Ambient light"}};
    for (const auto &[key, title] : scalars) {
        auto *row = new ui::SliderValueControl(group);
        auto *slider = row->slider();
        slider->setObjectName("surface_" + QString(key));
        slider->setRange(QString(key) == "brightness" ? 10 : 0, 200);
        slider->setSingleStep(1);
        slider->setPageStep(10);
        slider->setTickInterval(50);
        slider->setTickPosition(QSlider::NoTicks);
        row->setFormatter([](int n) { return QString::number(n) + "%"; });
        row->setDefaultValue(100);
        auto *value = row->valueLabel();
        value->setObjectName("surface_" + QString(key) + "_value");
        row->resetButton()->setObjectName("surface_" + QString(key) + "_default");
        (*numbers)[key] = slider;
        (*numberLabels)[key] = value;
        form->addRow(new ui::FieldRow(title, row, group));
    }
    auto *meshGroup = ui::make<QGroupBox>("Surface mesh", group);
    meshGroup->setObjectName("surfaceMeshSettings");
    auto *meshForm = new QFormLayout(meshGroup);
    meshForm->addRow((*checks)["mesh_grid"]);
    auto *widthRow = new ui::SliderValueControl(meshGroup);
    auto *meshWidth = widthRow->slider();
    meshWidth->setObjectName("surface_mesh_width");
    meshWidth->setRange(50, 300);
    meshWidth->setSingleStep(5);
    widthRow->setFormatter([](int n) { return QString::number(n / 100.0, 'f', 2); });
    meshForm->addRow(new ui::FieldRow("Mesh thickness", widthRow, meshGroup));
    auto *contrastRow = new ui::SliderValueControl(meshGroup);
    auto *meshContrast = contrastRow->slider();
    meshContrast->setObjectName("surface_mesh_contrast");
    meshContrast->setRange(0, 100);
    contrastRow->setFormatter([](int n) { return QString::number(n) + "%"; });
    meshForm->addRow(new ui::FieldRow("Mesh contrast", contrastRow, meshGroup));
    auto *meshColor = ui::make<QPushButton>("Mesh color", meshGroup);
    meshColor->setObjectName("surface_mesh_color");
    auto *meshColorRow = new QWidget(meshGroup);
    auto *meshColorLayout = new QHBoxLayout(meshColorRow);
    meshColorLayout->setContentsMargins(0, 0, 0, 0);
    meshColorLayout->addWidget(meshColor, 1);
    meshForm->addRow(meshColorRow);
    form->addRow(meshGroup);
    auto *color = ui::make<QPushButton>("Choose background color", group);
    color->setObjectName("surface_background_color");
    auto *backgroundRow = new QWidget(group);
    auto *backgroundLayout = new QHBoxLayout(backgroundRow);
    backgroundLayout->setContentsMargins(0, 0, 0, 0);
    backgroundLayout->addWidget(color, 1);
    form->addRow(ui::make<QLabel>("Background", group), backgroundRow);
    auto *scope = ui::make<QLabel>("Axis ranges and color scales are configured beside each graph.", group);
    scope->setWordWrap(true);
    form->insertRow(0, scope);
    auto load = [this, checks, numbers, numberLabels, color, meshWidth, meshContrast, meshColor, refreshScreenRate, sceneAspect, rangeInterval] {
        const auto settings = SurfaceSettings::effective(_context.file_configs.ui.graph_rendering);
        sceneAspect->set_options(settings);
        rangeInterval->set_seconds(settings["auto_range_interval_seconds"].toDouble(10));
        for (auto it = checks->begin(); it != checks->end(); ++it) {
            const QSignalBlocker block(it.value());
            if (it.key() == "space_grid") {
                const bool axis = settings["axis_grid"].toBool(), floor = settings["floor_grid"].toBool();
                it.value()->setTristate(axis != floor);
                it.value()->setCheckState(axis != floor ? Qt::PartiallyChecked : axis ? Qt::Checked : Qt::Unchecked);
            } else
                it.value()->setChecked(settings[it.key()].toBool());
        }
        for (auto it = numbers->begin(); it != numbers->end(); ++it) {
            const QSignalBlocker block(it.value());
            it.value()->setValue(qRound(settings[it.key()].toDouble() * 100));
            (*numberLabels)[it.key()]->setText(QString::number(it.value()->value()) + "%");
        }
        {
            const QSignalBlocker block(meshWidth);
            meshWidth->setValue(qRound(settings["mesh_width"].toDouble() * 100));
        }
        {
            const QSignalBlocker block(meshContrast);
            meshContrast->setValue(qRound(settings["mesh_contrast"].toDouble() * 100));
        }
        // Blocked valueChanged still requires updating the adjacent readouts.
        auto *widthValue = meshWidth->parentWidget()->findChild<QLabel *>();
        if (widthValue)
            widthValue->setText(QString::number(meshWidth->value() / 100.0, 'f', 2));
        auto *contrastValue = meshContrast->parentWidget()->findChild<QLabel *>();
        if (contrastValue)
            contrastValue->setText(QString::number(meshContrast->value()) + "%");
        color->setProperty("backgroundMode", settings["background_mode"]);
        color->setProperty("selectedColor", settings["background_color"]);
        meshColor->setProperty("selectedColor", settings["mesh_color"].toString());
        auto swatch = [](QPushButton *button, const QColor &value) {
            QPixmap icon(18, 18);
            icon.fill(value);
            button->setIcon(QIcon(icon));
        };
        swatch(color, settings["background_mode"] == "custom"
                          ? QColor(settings["background_color"].toString())
                          : ui::Theme::instance().palette().color(QPalette::Window));
        swatch(meshColor, QColor(settings["mesh_color"] == "auto" ? "#08171c" : settings["mesh_color"].toString()));
        meshColor->setToolTip(settings["mesh_color"] == "auto" ? ui::text("Default mesh color")
                                                               : settings["mesh_color"].toString());
        refreshScreenRate();
    };
    auto save = [this, checks, numbers, color, meshWidth, meshContrast, meshColor, load, sceneAspect, rangeInterval] {
        auto &ui = _context.file_configs.ui;
        auto values = ui.graph_rendering["defaults"].toObject();
        const auto previous = SurfaceSettings::effective(ui.graph_rendering);
        for (auto it = checks->begin(); it != checks->end(); ++it) {
            if (it.key() == "space_grid") {
                if (it.value()->checkState() != Qt::PartiallyChecked) {
                    values["axis_grid"] = it.value()->isChecked();
                    values["floor_grid"] = it.value()->isChecked();
                }
            } else
                values[it.key()] = it.value()->isChecked();
        }
        for (auto it = numbers->begin(); it != numbers->end(); ++it) {
            const auto old = previous[it.key()].toDouble();
            values[it.key()] = qRound(old * 100) == it.value()->value() ? old : it.value()->value() / 100.0;
        }
        values["background_mode"] = color->property("backgroundMode").toString();
        values["background_color"] = color->property("selectedColor").toString();
        const auto width = previous["mesh_width"].toDouble();
        values["mesh_width"] = qRound(width * 100) == meshWidth->value() ? width : meshWidth->value() / 100.0;
        const auto contrast = previous["mesh_contrast"].toDouble();
        values["mesh_contrast"] =
            qRound(contrast * 100) == meshContrast->value() ? contrast : meshContrast->value() / 100.0;
        values["mesh_color"] = meshColor->property("selectedColor").toString();
        ui.graph_rendering["version"] = 1;
        values["auto_range_interval_seconds"]=rangeInterval->seconds();
        ui.graph_rendering["defaults"] = sceneAspect->apply(values);
        emit surface_settings_changed();
        _on_config_change(ui::SettingsChangeScope::ProjectPresentation);
        load();
    };
    sceneAspect->changed=save;
    connect(rangeInterval->box,&QDoubleSpinBox::valueChanged,this,save);
    for (auto it = checks->begin(); it != checks->end(); ++it) {
        if (it.key() == "space_grid")
            connect(it.value(), &QCheckBox::checkStateChanged, this, save);
        else
            connect(it.value(), &QCheckBox::toggled, this, save);
    }
    for (auto *slider : *numbers)
        connect(slider, &QSlider::valueChanged, this, save);
    connect(meshWidth, &QSlider::valueChanged, this, save);
    connect(meshContrast, &QSlider::valueChanged, this, save);
    connect(meshColor, &QPushButton::clicked, this, [this, meshColor, save] {
        bool reset = false;
        const auto c = ui::select_color(QColor(meshColor->property("selectedColor").toString() == "auto"
                                                   ? "#08171c"
                                                   : meshColor->property("selectedColor").toString()),
                                        this, ui::text("Mesh color"), &reset, QColor("#08171c"));
        if (c.isValid()) {
            meshColor->setProperty("selectedColor", reset ? QString("auto") : c.name());
            save();
        }
    });
    connect(color, &QPushButton::clicked, this, [this, color, save] {
        bool reset = false;
        const auto initial = color->property("backgroundMode") == "auto"
                                 ? ui::Theme::instance().palette().color(QPalette::Window)
                                 : QColor(color->property("selectedColor").toString());
        const auto selected = ui::select_color(initial, this, ui::text("Background"), &reset,
                                               ui::Theme::instance().palette().color(QPalette::Window));
        if (selected.isValid()) {
            if (!reset)
                color->setProperty("selectedColor", selected.name());
            color->setProperty("backgroundMode", reset ? "auto" : "custom");
            save();
        }
    });
    connect(&ui::Theme::instance(), &ui::Theme::changed, this, load);
    connect(&ui::Language::instance(), &ui::Language::changed, this, load);
    _refresh_graph_targets = load;
    load();
    return page;
}
