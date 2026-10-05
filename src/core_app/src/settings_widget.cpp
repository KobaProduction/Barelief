#include "visualization/ui_text.h"
#include "core_app/settings_widget.h"

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

#include "core/utils.h"

Q_LOGGING_CATEGORY(core_app_settings, "core_app.settings")

SettingsWidget::SettingsWidget(GuiState &context, QWidget *parent)
    : QWidget(parent), _context(context) {
    _setup_ui();
    _setup_camera_controls();
    auto *sections = new QTabWidget(this);
    sections->setObjectName("settingsSections");
    auto *outer = qobject_cast<QVBoxLayout *>(layout());
    auto *general = findChild<QScrollArea *>();
    outer->removeWidget(general);
    outer->addWidget(sections);
    ui::tab(sections,general,"General");
    auto add_section = [sections](QWidget *group, const QString &title) {
        auto *scroll = new QScrollArea(sections);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *page = new QWidget(scroll);
        auto *layout = new QVBoxLayout(page);
        layout->addWidget(group);
        layout->addStretch();
        scroll->setWidget(page);
        ui::tab(sections,scroll,title.toUtf8().constData());
    };
    auto *processing=new QGroupBox(this); auto *processing_layout=new QVBoxLayout(processing);
    auto *automatic=ui::make<QCheckBox>("Auto apply pipeline",processing); automatic->setObjectName("pipelineAutoApply");
    automatic->setChecked(_context.file_configs.ui.pipeline_auto_apply); processing_layout->addWidget(automatic);
    connect(automatic,&QCheckBox::toggled,this,[this](bool checked) {
        _context.file_configs.ui.pipeline_auto_apply=checked; emit presentation_preferences_changed(); _on_config_change();
    });
    add_section(processing,"Audio processing");
    _main_layout->removeWidget(_wled_group); add_section(_wled_group,"WLED");
    _main_layout->removeWidget(_network_group); add_section(_network_group,"Network");
    auto *controls=findChild<QGroupBox *>("cameraControls");
    _main_layout->removeWidget(controls); add_section(controls,"Controls");
    for (int i = _main_layout->count()-1; i >= 0; --i)
        if (_main_layout->itemAt(i)->spacerItem()) delete _main_layout->takeAt(i);
    _main_layout->addStretch();
    _load_current_config();
    _connect_signals();
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
    language->addItem(QString::fromUtf8("Русский"), "ru"); language->addItem("English", "en");
    language->setCurrentIndex(_context.file_configs.ui.language=="en" ? 1 : 0);
    _main_layout->addWidget(ui::make<QLabel>("Interface language", this));
    _main_layout->addWidget(language);
    connect(language,&QComboBox::currentIndexChanged,this,[this,language](int index) {
        const auto code=language->itemData(index).toString();
        if (ui::Language::instance().set(code)) { _context.file_configs.ui.language=code; _on_config_change(); }
    });
    auto *theme=new QComboBox(this); theme->setObjectName("interfaceTheme");
    ui::items(theme,{"System theme","Dark","Light"});
    const QStringList themes{"system","dark","light"};
    theme->setCurrentIndex(qMax(0,themes.indexOf(_context.file_configs.ui.theme)));
    _main_layout->addWidget(ui::make<QLabel>("Interface theme",this));
    _main_layout->addWidget(theme);
    connect(theme,&QComboBox::currentIndexChanged,this,[this,themes](int index) {
        if (ui::Theme::instance().set(themes[index])) { _context.file_configs.ui.theme=themes[index]; _on_config_change(); }
    });
    auto *fps=ui::make<QCheckBox>("Show FPS",this); fps->setObjectName("showFPS");
    fps->setChecked(_context.file_configs.ui.show_fps); _main_layout->addWidget(fps);
    connect(fps,&QCheckBox::toggled,this,[this](bool checked) {
        _context.file_configs.ui.show_fps=checked; emit presentation_preferences_changed(); _on_config_change();
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
    auto* adapter_label = ui::make<QLabel>("Network Adapter:", this);
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
    for (const auto&[interface_name, human_readable_name, ip_address] : networkEntries) {
        const QString displayText = QString("%1 (%2)") .arg(human_readable_name) .arg(ip_address.toString());
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
        _context.file_configs.wled_ar_server.network_interface_name = _network_adapter_combo->itemData(defaultIndex).toString();
    }

    // Порт сервера
    auto* port_layout = new QHBoxLayout();
    auto* port_label = ui::make<QLabel>("Server Port:", this);
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
        _on_config_change(true);
    });

    connect(_show_raw_samples_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_raw_samples = checked;
        _on_config_change();
        emit on_rebuild_charts();
    });

    connect(_show_window_signal_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_window_signal = checked;
        _on_config_change();
        emit on_rebuild_charts();
    });

    connect(_show_windowed_signal_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_windowed_signal = checked;
        _on_config_change();
        emit on_rebuild_charts();
    });

    connect(_show_window_function_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_window_function = checked;
        _on_config_change();
        emit on_rebuild_charts();
    });

    connect(_show_amplitudes_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_amplitudes = checked;
        _on_config_change();
        emit on_rebuild_charts();
    });

    connect(_show_test_amplitudes_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_test_amplitudes = checked;
        _on_config_change();
        emit on_rebuild_charts();
    });

    connect(_show_spectrogram_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.show_spectrogram = checked;
        _on_config_change();
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
        _on_config_change();
    });

    connect(_network_adapter_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](const int index) {
        if (index >= 0) {
            _context.file_configs.wled_ar_server.network_interface_name = _network_adapter_combo->itemData(index).toString();
            emit wled_server_config_changed();
            _on_config_change();
        }
    });

    connect(_port_spinbox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](const int value) {
        _context.file_configs.wled_ar_server.port = static_cast<quint16>(value);
        emit wled_server_config_changed();
        _on_config_change();
    });
}

void SettingsWidget::_on_config_change(const bool force_save) {
    if (_context.file_configs.ui.autosave_enabled or force_save) {
        emit on_config_changed();
    }
}

void SettingsWidget::select_section(int index) {
    if (auto *tabs=findChild<QTabWidget *>("settingsSections")) tabs->setCurrentIndex(index);
}
