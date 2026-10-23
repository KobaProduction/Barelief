#include "barelief/scene_aspect_control.h"
#include "barelief/auto_range_interval_control.h"
#include "barelief/ui_text.h"
#include "core_app/settings_widget.h"
#include "core_app/ui/field_row.h"
#include "core_app/ui/slider_value_control.h"

#include <QVBoxLayout>
#include <utility>
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
#include "barelief/surface_settings.h"

#include "core/utils.h"

Q_LOGGING_CATEGORY(core_app_settings, "core_app.settings")

namespace {
QCheckBox* settings_checkbox(QWidget* parent,const char* title,const char* name) {
    auto* control=ui::make<QCheckBox>(title,parent);control->setObjectName(name);return control;
}
}
std::vector<SettingsWidget::SectionDescriptor> SettingsWidget::available_sections() {
    return {{General,"General","Alt+,"}, {Processing,"Audio processing","Alt+2"},
            {Graphs,"Graphs","Alt+3"}, {Controls,"Controls","Alt+6"}};
}

namespace {
QString section_title(SettingsWidget::Section section) {
    for(const auto& descriptor:SettingsWidget::available_sections())if(descriptor.section==section)return QString::fromUtf8(descriptor.title_key);
    return {};
}
void add_settings_section(QTabWidget* sections,QWidget* group,const QString& title) {
    auto* scroll=new QScrollArea(sections);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    auto* page=new QWidget(scroll);auto* layout=new QVBoxLayout(page);layout->addWidget(group);layout->addStretch();scroll->setWidget(page);
    ui::tab(sections,scroll,title.toUtf8().constData());
}
}
SettingsWidget::SettingsWidget(GuiState& context,QWidget* parent):QWidget(parent),_context(context) {
    _setup_ui();_setup_camera_controls();setup_sections();_load_current_config();_connect_signals();
}
void SettingsWidget::setup_sections() {
    auto *sections = new QTabWidget(this);
    sections->setObjectName("settingsSections");
    auto *outer = qobject_cast<QVBoxLayout *>(layout());
    auto *general = findChild<QScrollArea *>();
    outer->removeWidget(general);
    outer->addWidget(sections);
    ui::tab(sections, general, section_title(General).toUtf8().constData());
    auto *processing = new QGroupBox(this);
    auto *processing_layout = new QVBoxLayout(processing);
    _processing_layout = processing_layout;
    auto* running=ui::make<QLabel>("Data Flow runs continuously; valid changes apply automatically.",processing);
    running->setWordWrap(true);processing_layout->addWidget(running);
    add_settings_section(sections,processing, section_title(Processing));
    add_settings_section(sections,_setup_graphs(), section_title(Graphs));
    setup_adapter_sections(sections);setup_control_section(sections);
}
void SettingsWidget::setup_adapter_sections(QTabWidget* sections) {
    _main_layout->removeWidget(_wled_group);
    add_settings_section(sections,_wled_group, "WLED");
    _main_layout->removeWidget(_network_group);
    add_settings_section(sections,_network_group, "Network");
    // Output settings belong to registered graph parameters. Retain section
    // indices for workspace preferences while hiding obsolete global controls.
    sections->setTabVisible(WLED,false);
    sections->setTabVisible(Network,false);
}
void SettingsWidget::setup_control_section(QTabWidget* sections) {
    auto *controls = findChild<QGroupBox *>("cameraControls");
    _main_layout->removeWidget(controls);
    add_settings_section(sections,controls, section_title(Controls));
    for (int i = _main_layout->count() - 1; i >= 0; --i)
        if (_main_layout->itemAt(i)->spacerItem())
            delete _main_layout->takeAt(i);
    _main_layout->addStretch();
}

void SettingsWidget::set_execution_settings(QWidget *settings) {
    if (settings && _processing_layout) _processing_layout->addWidget(settings);
}

void SettingsWidget::_setup_ui() {
    setup_scroll_content();
    setup_language();
    setup_theme();
    setup_fps();
    setup_visibility_controls();
    setup_wled_controls();
    setup_network_controls();
    populate_network_interfaces();
    setup_network_port();
    _wled_layout->addStretch();

    _main_layout->addWidget(_wled_group);
    _main_layout->addStretch();
}

void SettingsWidget::setup_scroll_content() {
    auto *outer_layout = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    _main_layout = new QVBoxLayout(content);
    scroll->setWidget(content);
    outer_layout->addWidget(scroll);
}

void SettingsWidget::setup_language() {
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
}

void SettingsWidget::setup_theme() {
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
}

void SettingsWidget::setup_fps() {
    auto *fps = ui::make<QCheckBox>("Show FPS", this);
    fps->setObjectName("showFPS");
    fps->setChecked(_context.file_configs.ui.show_fps);
    _main_layout->addWidget(fps);
    connect(fps, &QCheckBox::toggled, this, [this](bool checked) {
        _context.file_configs.ui.show_fps = checked;
        emit presentation_preferences_changed();
        _on_config_change();
    });
}

void SettingsWidget::setup_visibility_controls() {
    _ui_group=ui::make<QGroupBox>("UI Settings",this);_ui_layout=new QVBoxLayout(_ui_group);
    _move_to_primary_screen_on_start_cb=ui::make<QCheckBox>("Move window to primary screen on startup",this);
    _enable_autosave_cb=settings_checkbox(this,"Autosave enabled","enableAutosave");
    _ui_layout->addWidget(_move_to_primary_screen_on_start_cb);
    _ui_layout->addWidget(_enable_autosave_cb);
    _ui_layout->addStretch();_main_layout->addWidget(_ui_group);_main_layout->addStretch();
}

void SettingsWidget::setup_wled_controls() {
    // Группа WLED AR server
    _wled_group = ui::make<QGroupBox>("WLED AR server", this);
    _wled_layout = new QVBoxLayout(_wled_group);

    _enable_wled_server_cb = ui::make<QCheckBox>("Enable WLED AR server", this);
    _enable_wled_server_cb->setObjectName("enableWLED");
    _wled_layout->addWidget(_enable_wled_server_cb);

}

void SettingsWidget::setup_network_controls() {
    // Выбор сетевого адаптера
    _network_group = ui::make<QGroupBox>("Network", this);
    _network_layout = new QVBoxLayout(_network_group);
    _main_layout->addWidget(_network_group);
    auto *adapter_label = ui::make<QLabel>("Network Adapter:", this);
    _network_layout->addWidget(adapter_label);

    _network_adapter_combo = new QComboBox(this);
    _network_layout->addWidget(_network_adapter_combo);

}

void SettingsWidget::populate_network_interfaces() {
    // Определение интерфейса по умолчанию через тестовое UDP соединение
    const auto defaultInterfaceAddress = get_default_interfaces();

    // Заполнение списка сетевых адаптеров
    int defaultIndex = -1;
    int savedIndex = -1;
    int currentIndex = 0;
    const QString saved_interface = _context.file_configs.wled_ar_server.network_interface_name;

    const auto networkEntries = get_available_network_interfaces();
    for (const auto &[interface_name, human_readable_name, ip_address] : networkEntries) {
        const QString displayText = QString("%1 (%2)").arg(human_readable_name,ip_address.toString());
        _network_adapter_combo->addItem(displayText, interface_name);

        if (not saved_interface.isEmpty() and interface_name == saved_interface) {
            savedIndex = currentIndex;
        }

        if (not defaultInterfaceAddress.isNull() and ip_address == defaultInterfaceAddress) {
            defaultIndex = currentIndex;
        }
        currentIndex++;
    }

    select_network_interface(savedIndex,defaultIndex);
}
void SettingsWidget::select_network_interface(int savedIndex,int defaultIndex) {
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

}

void SettingsWidget::setup_network_port() {
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

}

void SettingsWidget::_load_current_config() const {

    _enable_autosave_cb->setChecked(_context.file_configs.ui.autosave_enabled);
    _move_to_primary_screen_on_start_cb->setChecked(_context.file_configs.ui.move_to_primary_screen_on_start);

    // Загрузка настроек WLED AR server
    _enable_wled_server_cb->setChecked(_context.file_configs.wled_ar_server.enabled);
    _port_spinbox->setValue(_context.file_configs.wled_ar_server.port);
}

void SettingsWidget::connect_preference_signals() {
    // Сигналы управления отображением

    connect(_move_to_primary_screen_on_start_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.move_to_primary_screen_on_start = checked;
        _on_config_change();
    });

    connect(_enable_autosave_cb, &QCheckBox::toggled, this, [this](const bool checked) {
        _context.file_configs.ui.autosave_enabled = checked;
        // Disabling autosave stops writes immediately; manual Save persists it.
        // Enabling autosave persists the accumulated UI preferences.
        _on_config_change();
    });
}
void SettingsWidget::connect_wled_signals() {
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
void SettingsWidget::_connect_signals() {
    connect_preference_signals();
    connect_wled_signals();
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
    if(_refresh_dimensions)_refresh_dimensions();
}
void SettingsWidget::refresh_display_cadence() {
    if(_refresh_screen_rate)_refresh_screen_rate();
}
void SettingsWidget::invalidate_display_catalog() {
    _display_rates_dirty=true;
}
void SettingsWidget::refresh_screen_catalog(bool geometryChanged) {
    _display_rates_dirty=true;
    if(_refresh_screen_rate)_refresh_screen_rate();
    if(geometryChanged){emit cadence_changed();emit surface_settings_changed();}
}
void SettingsWidget::set_display_screen(QScreen *screen) {
    if (_display_screen == screen)
        return;
    QObject::disconnect(_screen_rate_connection);
    QObject::disconnect(_screen_geometry_connection);
    _display_screen = screen;
    _display_rates_dirty=true;
    if(screen)_screen_rate_connection=connect(screen,&QScreen::refreshRateChanged,this,[this]{refresh_screen_catalog(false);});
    if(screen)_screen_geometry_connection=connect(screen,&QScreen::geometryChanged,this,[this]{refresh_screen_catalog(true);});
    if (_refresh_screen_rate)
        _refresh_screen_rate();
}
void SettingsWidget::set_display_rate_provider(DisplayRateProvider provider) {
    _display_rate_provider=std::move(provider);
    _display_rates_dirty=true;
    if(_refresh_screen_rate)_refresh_screen_rate();
    emit surface_settings_changed();
}
void SettingsWidget::set_display_current_rate_provider(DisplayCurrentRateProvider provider) {
    _display_current_rate_provider=std::move(provider);
}
void SettingsWidget::set_display_mode_requester(DisplayModeRequester requester) {
    _display_mode_requester=std::move(requester);
}
double SettingsWidget::display_refresh_rate() const {
    const double current=_display_current_rate_provider?_display_current_rate_provider(_display_screen.data()):_display_rates.current;
    if(std::isfinite(current) && current>0)return current;
    const auto* screen=_display_screen.data();const double rate=screen?screen->refreshRate():60;
    return std::isfinite(rate) && rate>0?rate:60;
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

class GradientEditor final : public QDialog {
    const QJsonObject settings;
    QJsonObject& output;
    QJsonArray& colorPresets;
    QJsonArray savedPresets;
    const QStringList presetCodes{"green_red","black_yellow","viridis","ice_fire","rainbow"};
    const QStringList presetNames{"Green / Red","Black / Yellow","Viridis","Ice / Fire","Rainbow"};
    const QStringList spacingCodes{"preset","linear","log"};
    bool editableScale=false,populating=false;
    QString editingPresetId;
    QVBoxLayout* layout=nullptr;
    GradientPreview* preview=nullptr;
    QComboBox* preset=nullptr;
    QComboBox* spacing=nullptr;
    QPushButton* copyPreset=nullptr;
    QPushButton* savePreset=nullptr;
    QPushButton* deletePreset=nullptr;
    QPushButton* add=nullptr;
    QPushButton* remove=nullptr;
    QPushButton* reset=nullptr;
    QLabel* draftTitle=nullptr;
    QLabel* error=nullptr;
    QTableWidget* table=nullptr;
    QDialogButtonBox* buttons=nullptr;
    void setup_dialog(){
        setObjectName("gradientEditor");ui::bind(this,"windowTitle","Edit color scale");resize(500,440);
        layout=new QVBoxLayout(this);preview=new GradientPreview(this);preview->setObjectName("gradientPreview");
    }
    void rebuild_presets(const QString& selected){
        const QSignalBlocker block(preset);preset->clear();
        for(int i=0;i<presetCodes.size();++i)preset->addItem(ui::text(presetNames[i].toUtf8().constData()),presetCodes[i]);
        for(const auto& value:std::as_const(savedPresets)){
            const auto entry=value.toObject();
            if(entry["id"].isString() && !entry["id"].toString().isEmpty() && entry["name"].isString() && !entry["name"].toString().trimmed().isEmpty())
                preset->addItem(entry["name"].toString(),"saved:"+entry["id"].toString());
        }
        preset->setCurrentIndex(preset->findData(selected));
    }
    void setup_source(){
        auto* options=new QFormLayout;preset=new QComboBox(this);preset->setObjectName("gradientPreset");
        rebuild_presets(settings["gradient_stops"].toArray().isEmpty()?settings["gradient"].toString():QString{});
        preset->setPlaceholderText(ui::text("Choose a source preset"));options->addRow(ui::make<QLabel>("Source preset",this),preset);layout->addLayout(options);
    }
    void setup_preset_tools(){
        auto* tools=new QHBoxLayout;copyPreset=ui::make<QPushButton>("Create editable copy",this);copyPreset->setObjectName("copyColorPreset");tools->addWidget(copyPreset);
        savePreset=ui::make<QPushButton>("Save color preset",this);savePreset->setObjectName("saveColorPreset");tools->addWidget(savePreset);
        deletePreset=ui::make<QPushButton>("Delete color preset",this);deletePreset->setObjectName("deleteColorPreset");tools->addWidget(deletePreset);layout->addLayout(tools);
        auto* help=ui::make<QLabel>("Built-in presets are read-only. Create a copy to change colors or positions.",this);help->setWordWrap(true);layout->addWidget(help);
        draftTitle=new QLabel(this);draftTitle->setObjectName("colorScaleEditingState");layout->addWidget(draftTitle);layout->addWidget(preview);
    }
    void setup_transition(){
        spacing=new QComboBox(this);spacing->setObjectName("gradientSpacing");ui::items(spacing,{"Preset transitions","Linear transitions","Logarithmic transitions"});
        for(int i=0;i<spacingCodes.size();++i)spacing->setItemData(i,spacingCodes[i]);
        auto* options=new QFormLayout;options->addRow(ui::make<QLabel>("Transition positions",this),spacing);layout->addLayout(options);
    }
    void setup_table(){
        table=new QTableWidget(0,2,this);table->setObjectName("gradientStops");table->setHorizontalHeaderLabels({ui::text("Position (%)"),ui::text("Color")});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);table->setSelectionBehavior(QAbstractItemView::SelectRows);layout->addWidget(table);
        error=new QLabel(this);error->setWordWrap(true);layout->addWidget(error);
    }
    void setup_stop_tools(){
        auto* tools=new QHBoxLayout;add=ui::make<QPushButton>("Add stop",this);add->setObjectName("addGradientStop");
        remove=ui::make<QPushButton>("Remove stop",this);remove->setObjectName("removeGradientStop");reset=ui::make<QPushButton>("Reset",this);reset->setObjectName("resetGradientStops");
        tools->addWidget(add);tools->addWidget(remove);tools->addStretch();tools->addWidget(reset);layout->addLayout(tools);
    }
    void setup_buttons(){
        buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this);layout->addWidget(buttons);
        ui::bind(buttons->button(QDialogButtonBox::Save),"text","Save");ui::bind(buttons->button(QDialogButtonBox::Cancel),"text","Cancel");
    }
    QJsonArray read()const{
        QJsonArray values;
        for(int row=0;row<table->rowCount();++row){
            auto stop=table->item(row,0)->data(Qt::UserRole).toJsonObject();const auto displayed=qobject_cast<QDoubleSpinBox*>(table->cellWidget(row,0))->value();
            if(qRound(stop["position"].toDouble()*10000)!=qRound(displayed*100))stop["position"]=displayed/100.0;
            stop["color"]=table->cellWidget(row,1)->property("stopColor").toString();values.append(stop);
        }
        return values;
    }
    void refresh_preview(){
        QGradientStops stops;std::map<double,QColor> sorted;
        const auto values=read();
        for(const auto& value:std::as_const(values)){const auto stop=value.toObject();sorted[stop.value("position").toDouble()]=QColor(stop.value("color").toString());}
        for(const auto& [position,color]:sorted)stops.append({position,color});preview->setStops(stops);
    }
    void refresh(){
        refresh_preview();table->setEnabled(editableScale);spacing->setEnabled(editableScale);reset->setEnabled(editableScale);copyPreset->setEnabled(preset->currentIndex()>=0);
        draftTitle->setText(ui::text(editableScale?"Editable color scale":"Read-only preset preview"));add->setEnabled(editableScale && table->rowCount()<16);
        remove->setEnabled(editableScale && table->currentRow()>0 && table->currentRow()<table->rowCount()-1 && table->rowCount()>2);
        deletePreset->setEnabled(preset->currentData().toString().startsWith("saved:"));
    }
    void custom(){if(populating || !editableScale)return;error->clear();refresh();}
    static void paint_color(QPushButton* color){
        const QColor value(color->property("stopColor").toString());QPixmap swatch(18,18);swatch.fill(value);color->setIcon(QIcon(swatch));color->setText(value.name());
    }
    void select_color(QPushButton* color,int row){
        table->setCurrentCell(row,1);const auto selected=ui::select_color(QColor(color->property("stopColor").toString()),this,ui::text("Color scale stop color"));
        if(selected.isValid()){color->setProperty("stopColor",selected.name());paint_color(color);custom();}
    }
    void append_color(int row,const QJsonObject& stop){
        auto* color=new QPushButton(table);color->setProperty("stopColor",stop["color"].toString());paint_color(color);table->setCellWidget(row,1,color);
        connect(color,&QPushButton::clicked,this,[this,color,row]{select_color(color,row);});
    }
    void append_stop(int row,const QJsonObject& stop,int count){
        table->insertRow(row);auto* item=new QTableWidgetItem;item->setData(Qt::UserRole,stop);table->setItem(row,0,item);
        auto* position=new QDoubleSpinBox(table);position->setRange(0,100);position->setDecimals(2);position->setSingleStep(1);position->setValue(stop["position"].toDouble()*100);
        position->setEnabled(row>0 && row<count-1);table->setCellWidget(row,0,position);append_color(row,stop);
        connect(position,&QDoubleSpinBox::valueChanged,this,[this]{custom();});
    }
    void populate(const QJsonArray& values){
        populating=true;table->setRowCount(0);for(int row=0;row<values.size();++row)append_stop(row,values[row].toObject(),values.size());populating=false;refresh();
    }
    QJsonObject selected_preset_values()const{
        auto value=settings;const auto code=preset->currentData().toString();value["gradient_stops"]=QJsonArray{};
        if(code.startsWith("saved:")){
            for(const auto& entry:std::as_const(savedPresets)){
                const auto candidate=entry.toObject();
                if(candidate.value("id").toString()!=code.mid(6))continue;
                value=candidate.value("values").toObject();break;
            }
        }else value["gradient"]=code.isEmpty()?settings["gradient"]:QJsonValue(code);
        return value;
    }
    QJsonArray preset_values()const{
        auto value=selected_preset_values();const auto custom=SurfaceSettings::normalizedStops(value["gradient_stops"].toArray());
        if(preset->currentData().toString().startsWith("saved:") && !custom.isEmpty())return custom;
        value["gradient_layout"]=spacing->currentData().toString();QJsonArray values;
        const auto stops=SurfaceSettings::gradientStops(value);
        for(const auto& stop:std::as_const(stops))values.append(QJsonObject{{"position",stop.first},{"color",stop.second.name()}});
        return values;
    }
    void load_preset_spacing(const QString& code){
        if(!code.startsWith("saved:"))return;
        for(const auto& entry:std::as_const(savedPresets)){
            const auto candidate=entry.toObject();
            if(candidate.value("id").toString()!=code.mid(6))continue;
            const auto values=candidate.value("values").toObject();
            const QSignalBlocker block(spacing);spacing->setCurrentIndex(std::max(0,spacing->findData(values.value("gradient_layout").toString("preset"))));break;
        }
    }
    void select_preset(int index){
        if(index<0)return;const auto code=preset->currentData().toString();editableScale=code.startsWith("saved:");editingPresetId=editableScale?code.mid(6):QString{};
        load_preset_spacing(code);populate(preset_values());error->clear();
    }
    void copy_preset(){editableScale=true;editingPresetId.clear();refresh();error->clear();}
    void retranslate(){rebuild_presets(preset->currentData().toString());refresh();preset->setPlaceholderText(ui::text("Choose a source preset"));}
    void change_spacing(){
        if(!editableScale)return;auto values=read();const auto mode=spacing->currentData().toString();
        if(mode!="preset")for(int i=0;i<values.size();++i){auto stop=values[i].toObject();const double p=double(i)/(values.size()-1);stop["position"]=mode=="log"?std::log10(1+9*p):p;values[i]=stop;}
        populate(values);error->clear();
    }
    static int largest_gap(const QJsonArray& values){
        int gap=0;double largest=-1;
        for(int i=0;i<values.size()-1;++i){const auto distance=values[i+1].toObject().value("position").toDouble()-values[i].toObject().value("position").toDouble();if(distance>largest){largest=distance;gap=i;}}
        return gap;
    }
    static QJsonObject midpoint_stop(const QJsonObject& left,const QJsonObject& right){
        const QColor a(left["color"].toString()),b(right["color"].toString());const auto color=QColor::fromRgbF((a.redF()+b.redF())/2,(a.greenF()+b.greenF())/2,(a.blueF()+b.blueF())/2);
        return QJsonObject{{"position",(left["position"].toDouble()+right["position"].toDouble())/2},{"color",color.name()}};
    }
    void add_stop(){
        auto values=SurfaceSettings::normalizedStops(read());if(values.isEmpty() || values.size()>=16)return;const auto gap=largest_gap(values);
        values.insert(gap+1,midpoint_stop(values[gap].toObject(),values[gap+1].toObject()));populate(values);table->setCurrentCell(gap+1,0);custom();
    }
    void remove_stop(){
        const int row=table->currentRow();if(row<=0 || row>=table->rowCount()-1)return;auto values=read();values.removeAt(row);populate(values);custom();
    }
    bool valid_stops(QJsonArray& valid){
        const auto raw=read();valid=SurfaceSettings::normalizedStops(raw);
        if(!valid.isEmpty() && raw.size()==valid.size())return true;error->setText(ui::text("Use distinct positions from 0% to 100%, including both ends."));return false;
    }
    void store_preset(const QString& name,const QJsonArray& valid){
        const auto id=!editingPresetId.isEmpty()?editingPresetId:QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject colors{{"gradient",settings["gradient"]},{"gradient_layout",spacing->currentData().toString()},{"gradient_stops",valid}};
        savedPresets=SurfaceSettings::saveColorPreset(savedPresets,id,name,colors);rebuild_presets("saved:"+id);editingPresetId=id;editableScale=true;refresh();error->clear();
    }
    void save_preset(){
        QJsonArray valid;if(!valid_stops(valid))return;bool accepted=false;
        const auto name=QInputDialog::getText(this,ui::text("Save color preset"),ui::text("Color preset name"),QLineEdit::Normal,!editingPresetId.isEmpty()?preset->currentText():QString{},&accepted);
        if(!accepted)return;if(name.trimmed().isEmpty()){error->setText(ui::text("Enter a color preset name."));return;}store_preset(name,valid);
    }
    void delete_preset(){
        const auto code=preset->currentData().toString();if(!code.startsWith("saved:"))return;savedPresets=SurfaceSettings::removeColorPreset(savedPresets,code.mid(6));
        rebuild_presets(QString{});editingPresetId.clear();editableScale=true;refresh();
    }
    void reset_stops(){
        const QSignalBlocker block(spacing);
        if(preset->currentIndex()<0){spacing->setCurrentIndex(std::max(0,spacing->findData(settings["gradient_layout"].toString("preset"))));const auto original=SurfaceSettings::normalizedStops(settings["gradient_stops"].toArray());populate(original.isEmpty()?preset_values():original);}
        else populate(preset_values());error->clear();
    }
    void save_result(){
        QJsonArray valid;if(!valid_stops(valid))return;colorPresets=savedPresets;output=settings;output["gradient_layout"]=spacing->currentData().toString();
        if(editableScale)output["gradient_stops"]=valid;else{output["gradient"]=preset->currentData().toString();output["gradient_stops"]=QJsonArray{};}accept();
    }
    void connect_source(){
        connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{retranslate();});connect(preset,&QComboBox::currentIndexChanged,this,[this](int index){select_preset(index);});
        connect(copyPreset,&QPushButton::clicked,this,[this]{copy_preset();});connect(spacing,&QComboBox::currentIndexChanged,this,[this]{change_spacing();});
        connect(savePreset,&QPushButton::clicked,this,[this]{save_preset();});connect(deletePreset,&QPushButton::clicked,this,[this]{delete_preset();});
    }
    void connect_stops(){
        connect(table,&QTableWidget::itemSelectionChanged,this,[this]{if(!populating)refresh();});connect(add,&QPushButton::clicked,this,[this]{add_stop();});
        connect(remove,&QPushButton::clicked,this,[this]{remove_stop();});connect(reset,&QPushButton::clicked,this,[this]{reset_stops();});
        connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(buttons,&QDialogButtonBox::accepted,this,[this]{save_result();});
    }
    void initialize(){
        auto current=SurfaceSettings::normalizedStops(settings.value("gradient_stops").toArray());editableScale=!current.isEmpty();
        preset->setCurrentIndex(preset->findData(current.isEmpty()?settings["gradient"].toString():QString{}));spacing->setCurrentIndex(std::max(0,int(spacingCodes.indexOf(settings["gradient_layout"].toString("preset")))));
        if(current.isEmpty())current=preset_values();populate(current);
    }
public:
    GradientEditor(QWidget* parent,const QJsonObject& initial,QJsonObject& result,QJsonArray& presets):QDialog(parent),settings(initial),output(result),colorPresets(presets),savedPresets(presets){
        setup_dialog();setup_source();setup_preset_tools();setup_transition();setup_table();setup_stop_tools();setup_buttons();initialize();connect_source();connect_stops();
    }
};
static bool editGradient(QWidget* parent,const QJsonObject& settings,QJsonObject& output,QJsonArray& colorPresets){
    GradientEditor editor(parent,settings,output,colorPresets);return editor.exec()==QDialog::Accepted;
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
    apply_color_scale(identity,result,presets);
}
void SettingsWidget::apply_color_scale(const QString& identity,const QJsonObject& result,const QJsonArray& presets) {
    auto& ui=_context.file_configs.ui;
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

class GraphSettingsPage final : public QWidget {
    SettingsWidget& owner;
    QVBoxLayout* pageLayout=nullptr;
    QGroupBox* group=nullptr;
    QFormLayout* form=nullptr;
    QComboBox* fpsMode=nullptr;
    QSpinBox* fpsValue=nullptr;
    QWidget* fpsInput=nullptr;
    QLabel* screenRate=nullptr;
    ui::SceneAspectControl* sceneAspect=nullptr;
    ui::AutoRangeIntervalControl* rangeInterval=nullptr;
    QMap<QString,QCheckBox*> checks;
    QMap<QString,QSlider*> numbers;
    QMap<QString,QLabel*> numberLabels;
    QSlider* meshWidth=nullptr;
    QSlider* meshContrast=nullptr;
    QPushButton* meshColor=nullptr;
    QPushButton* color=nullptr;
    QJsonObject& rendering(){return owner._context.file_configs.ui.graph_rendering;}
    void publish(){emit owner.surface_settings_changed();owner._on_config_change(ui::SettingsChangeScope::ProjectPresentation);}
    void setup_appearance(){
        pageLayout=new QVBoxLayout(this);pageLayout->setContentsMargins(0,0,0,0);
        group=ui::make<QGroupBox>("Default 3D appearance",this);group->setObjectName("surfaceGraphSettings");
        pageLayout->addWidget(group);form=new QFormLayout(group);
        auto* scope=ui::make<QLabel>("Axis ranges and color scales are configured beside each graph.",group);
        scope->setWordWrap(true);form->insertRow(0,scope);
    }
    void setup_cadence_inputs(QGroupBox* cadence,QFormLayout* layout){
        fpsMode=new QComboBox(cadence);fpsMode->setObjectName("guiFpsMode");
        fpsValue=new QSpinBox(cadence);fpsValue->setObjectName("guiTargetFps");fpsValue->setRange(1,1000);
        fpsInput=new QWidget(cadence);auto* inputLayout=new QHBoxLayout(fpsInput);inputLayout->setContentsMargins(0,0,0,0);
        inputLayout->addWidget(fpsValue);inputLayout->addWidget(ui::make<QLabel>("Hz",fpsInput));
        screenRate=new QLabel(cadence);screenRate->setObjectName("guiScreenRefreshRate");
        layout->addRow(fpsMode,fpsInput);layout->addRow(screenRate);
    }
    void setup_cadence(){
        auto* cadence=ui::make<QGroupBox>("Display refresh rate",this);auto* layout=new QFormLayout(cadence);
        setup_cadence_inputs(cadence,layout);
        ui::bind(cadence,"toolTip","Supported frequencies change the monitor mode for this session. Maximum follows the current monitor. Custom only limits drawing.");
        if(auto* showFPS=owner.findChild<QCheckBox*>("showFPS")){owner._main_layout->removeWidget(showFPS);layout->addRow(showFPS);}
        pageLayout->insertWidget(0,cadence);
        owner._refresh_screen_rate=[this]{refresh_screen_rate();};refresh_screen_rate();
        connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{refresh_screen_rate();});
        connect(fpsMode,&QComboBox::currentIndexChanged,this,[this]{select_cadence();});
        connect(fpsValue,&QSpinBox::valueChanged,this,[this]{save_cadence();});
    }
    void setup_workspace(){
        auto* workspace=ui::make<QGroupBox>("Workspace",this);auto* layout=new QFormLayout(workspace);
        auto* position=new QComboBox(workspace);position->setObjectName("dataFlowPosition");
        ui::items(position,{"Bottom","Top","Left side","Right side"});const QStringList positions{"bottom","top","left","right"};
        for(int i=0;i<positions.size();++i)position->setItemData(i,positions[i]);
        position->setCurrentIndex(std::max(0,int(positions.indexOf(owner._context.file_configs.ui.workspace.value("data_flow_position").toString("bottom")))));
        layout->addRow(new ui::FieldRow("Data Flow position",position,workspace));pageLayout->insertWidget(0,workspace);
        connect(position,&QComboBox::currentIndexChanged,this,[this,position]{
            owner._context.file_configs.ui.workspace["data_flow_position"]=position->currentData().toString();
            emit owner.workspace_position_changed();owner._on_config_change(ui::SettingsChangeScope::SessionWorkspace);
        });
    }
    static void append_cadence_rate(double supported,QSet<int>& shown,QList<QPair<QString,int>>& choices){
        if(!std::isfinite(supported) || supported<1 || supported>1000)return;
        const int target=int(std::round(supported));
        if(shown.contains(target))return;
        shown.insert(target);
        choices.append({ui::text("%1 Hz").arg(QString::number(supported,'f',supported==std::floor(supported)?0:2)),target});
    }
    QList<QPair<QString,int>> cadence_choices(double rate){
        auto rates=owner._display_rates;if(rates.supported.isEmpty())rates.supported.append(rate);
        QList<QPair<QString,int>> choices{{ui::text("System refresh rate"),-2},{ui::text("Maximum"),0}};
        std::sort(rates.supported.begin(),rates.supported.end());QSet<int> shown;
        for(double supported:std::as_const(rates.supported))append_cadence_rate(supported,shown,choices);
        choices.append({ui::text("Custom"),-1});return choices;
    }
    void update_cadence_choices(const QList<QPair<QString,int>>& choices){
        bool same=fpsMode->count()==choices.size();
        for(int i=0;same && i<choices.size();++i)same=fpsMode->itemData(i).toInt()==choices[i].second;
        // Preserve the active popup model when only preferences change.
        if(!same)fpsMode->clear();
        for(int i=0;i<choices.size();++i){
            if(same)fpsMode->setItemText(i,choices[i].first);else fpsMode->addItem(choices[i].first,choices[i].second);
        }
    }
    int selected_cadence(const QJsonObject& cadence)const{
        const bool screen=cadence["gui_fps_mode"]=="screen",maximum=cadence["gui_fps_mode"]=="maximum";
        const bool display=cadence["gui_fps_mode"]=="display";const int matching=fpsMode->findData(cadence["gui_target_fps"].toInt());
        if(maximum || (display && matching<0))return fpsMode->findData(0);
        if(screen)return fpsMode->findData(-2);
        return display && matching>0?matching:fpsMode->findData(-1);
    }
    void refresh_screen_rate(){
        const auto cadence=SurfaceSettings::normalizedCadence(rendering());
        if(owner._display_rates_dirty){owner._display_rates=owner._display_rate_provider?owner._display_rate_provider(owner._display_screen.data()):SettingsWidget::DisplayRates{};owner._display_rates_dirty=false;}
        const double rate=owner.display_refresh_rate();const QSignalBlocker modeBlock(fpsMode),valueBlock(fpsValue);
        update_cadence_choices(cadence_choices(rate));fpsMode->setPlaceholderText(ui::text("Screen refresh rate"));fpsMode->setCurrentIndex(selected_cadence(cadence));
        const bool fixed=cadence["gui_fps_mode"]=="fixed";
        fpsValue->setValue(fixed?cadence["gui_target_fps"].toInt():SurfaceSettings::renderFps(cadence,rate));
        fpsInput->setVisible(fpsMode->currentData().toInt()==-1);
        screenRate->setText(ui::text("Screen: %1 Hz").arg(QString::number(rate,'f',rate==std::floor(rate)?0:2)));
    }
    bool request_display_mode(int choice){
        if(choice<0 || !owner._display_mode_requester)return true;
        if(!owner._display_mode_requester(owner._display_screen.data(),choice)){if(owner._refresh_screen_rate)owner._refresh_screen_rate();return false;}
        if(!owner._display_current_rate_provider)owner._display_rates_dirty=true;
        return true;
    }
    void save_cadence(){
        const int choice=fpsMode->currentData().toInt();if(!request_display_mode(choice))return;
        auto schema=rendering();schema["gui_fps_mode"]=choice==-2?"screen":choice==0?"maximum":choice>0?"display":"fixed";
        schema["gui_target_fps"]=choice>0?choice:fpsValue->value();rendering()=SurfaceSettings::normalizedCadence(schema);
        {const QSignalBlocker block(fpsValue);fpsValue->setValue(SurfaceSettings::renderFps(rendering(),owner.display_refresh_rate()));}
        emit owner.cadence_changed();publish();
    }
    void select_cadence(){
        const QSignalBlocker block(fpsValue);const int fixed=fpsMode->currentData().toInt();fpsInput->setVisible(fixed==-1);
        if(fixed>0)fpsValue->setValue(fixed);else if(fixed==0)fpsValue->setValue(SurfaceSettings::renderFps({},owner.display_refresh_rate()));
        save_cadence();
    }
    void setup_precision(){
        auto* row=new ui::SliderValueControl(group);auto* precision=row->slider();precision->setObjectName("surface_axis_precision");precision->setRange(0,8);
        const auto effective=SurfaceSettings::effective(rendering());
        auto* label=row->valueLabel();precision->setValue(effective.value("axis_precision").toInt());label->setText(QString::number(precision->value()));
        connect(precision,&QSlider::valueChanged,this,[this,label](int value){
            label->setText(QString::number(value));auto defaults=rendering()["defaults"].toObject();defaults["axis_precision"]=value;rendering()["defaults"]=defaults;publish();
        });form->addRow(new ui::FieldRow("Axis decimal places",row,group));
    }
    void setup_flags(){
        const std::pair<const char*,const char*> flags[]={{"mesh_grid","Surface mesh"},{"space_grid","Space grid"},{"shadows","Shadows"},{"light","Lighting"}};
        for(const auto& [key,title]:flags){auto* box=ui::make<QCheckBox>(title,group);box->setObjectName("surface_"+QString(key));checks[key]=box;if(QString(key)!="mesh_grid")form->addRow(box);}
    }
    void setup_scalar(const char* key,const char* title){
        auto* row=new ui::SliderValueControl(group);auto* slider=row->slider();slider->setObjectName("surface_"+QString(key));
        slider->setRange(QString(key)=="brightness"?10:0,200);slider->setSingleStep(1);slider->setPageStep(10);slider->setTickInterval(50);slider->setTickPosition(QSlider::NoTicks);
        row->setFormatter([](int n){return QString::number(n)+"%";});row->setDefaultValue(100);
        auto* value=row->valueLabel();value->setObjectName("surface_"+QString(key)+"_value");row->resetButton()->setObjectName("surface_"+QString(key)+"_default");
        numbers[key]=slider;numberLabels[key]=value;form->addRow(new ui::FieldRow(title,row,group));
    }
    static QWidget* button_row(QPushButton* button,QWidget* parent){
        auto* row=new QWidget(parent);auto* layout=new QHBoxLayout(row);layout->setContentsMargins(0,0,0,0);layout->addWidget(button,1);return row;
    }
    void setup_mesh(){
        auto* mesh=ui::make<QGroupBox>("Surface mesh",group);mesh->setObjectName("surfaceMeshSettings");auto* layout=new QFormLayout(mesh);layout->addRow(checks["mesh_grid"]);
        auto* width=new ui::SliderValueControl(mesh);meshWidth=width->slider();meshWidth->setObjectName("surface_mesh_width");meshWidth->setRange(50,300);meshWidth->setSingleStep(5);
        width->setFormatter([](int n){return QString::number(n/100.0,'f',2);});layout->addRow(new ui::FieldRow("Mesh thickness",width,mesh));
        setup_mesh_contrast(mesh,layout);form->addRow(mesh);
    }
    void setup_mesh_contrast(QGroupBox* mesh,QFormLayout* layout){
        auto* contrast=new ui::SliderValueControl(mesh);meshContrast=contrast->slider();meshContrast->setObjectName("surface_mesh_contrast");meshContrast->setRange(0,100);
        contrast->setFormatter([](int n){return QString::number(n)+"%";});layout->addRow(new ui::FieldRow("Mesh contrast",contrast,mesh));
        meshColor=ui::make<QPushButton>("Mesh color",mesh);meshColor->setObjectName("surface_mesh_color");layout->addRow(button_row(meshColor,mesh));
    }
    void setup_background(){
        color=ui::make<QPushButton>("Choose background color",group);color->setObjectName("surface_background_color");
        form->addRow(ui::make<QLabel>("Background",group),button_row(color,group));
    }
    static void load_flag(const QString& key,QCheckBox* box,const QJsonObject& settings){
        const QSignalBlocker block(box);
        if(key!="space_grid"){box->setChecked(settings[key].toBool());return;}
        const bool axis=settings["axis_grid"].toBool(),floor=settings["floor_grid"].toBool();
        box->setTristate(axis!=floor);box->setCheckState(axis!=floor?Qt::PartiallyChecked:axis?Qt::Checked:Qt::Unchecked);
    }
    void load_numbers(const QJsonObject& settings){
        for(auto it=numbers.begin();it!=numbers.end();++it){const QSignalBlocker block(it.value());it.value()->setValue(qRound(settings[it.key()].toDouble()*100));numberLabels[it.key()]->setText(QString::number(it.value()->value())+"%");}
    }
    static void load_mesh_slider(QSlider* slider,double value,bool percentage){
        const QSignalBlocker block(slider);slider->setValue(qRound(value*100));auto* label=slider->parentWidget()->findChild<QLabel*>();
        if(label)label->setText(percentage?QString::number(slider->value())+"%":QString::number(slider->value()/100.0,'f',2));
    }
    static void swatch(QPushButton* button,const QColor& value){QPixmap icon(18,18);icon.fill(value);button->setIcon(QIcon(icon));}
    void load_colors(const QJsonObject& settings){
        color->setProperty("backgroundMode",settings["background_mode"]);color->setProperty("selectedColor",settings["background_color"]);
        meshColor->setProperty("selectedColor",settings["mesh_color"].toString());
        swatch(color,settings["background_mode"]=="custom"?QColor(settings["background_color"].toString()):ui::Theme::instance().palette().color(QPalette::Window));
        swatch(meshColor,QColor(settings["mesh_color"]=="auto"?"#08171c":settings["mesh_color"].toString()));
        meshColor->setToolTip(settings["mesh_color"]=="auto"?ui::text("Default mesh color"):settings["mesh_color"].toString());
    }
    void load(){
        const auto settings=SurfaceSettings::effective(rendering());sceneAspect->set_options(settings);rangeInterval->set_seconds(settings["auto_range_interval_seconds"].toDouble(10));
        for(auto it=checks.begin();it!=checks.end();++it)load_flag(it.key(),it.value(),settings);
        load_numbers(settings);load_mesh_slider(meshWidth,settings["mesh_width"].toDouble(),false);load_mesh_slider(meshContrast,settings["mesh_contrast"].toDouble(),true);
        load_colors(settings);refresh_screen_rate();
    }
    void save_flags(QJsonObject& values){
        for(auto it=checks.begin();it!=checks.end();++it){
            if(it.key()!="space_grid"){values[it.key()]=it.value()->isChecked();continue;}
            if(it.value()->checkState()!=Qt::PartiallyChecked){values["axis_grid"]=it.value()->isChecked();values["floor_grid"]=it.value()->isChecked();}
        }
    }
    static double retained_slider_value(double previous,QSlider* slider){return qRound(previous*100)==slider->value()?previous:slider->value()/100.0;}
    void save_values(QJsonObject& values,const QJsonObject& previous){
        save_flags(values);
        for(auto it=numbers.begin();it!=numbers.end();++it)values[it.key()]=retained_slider_value(previous[it.key()].toDouble(),it.value());
        values["background_mode"]=color->property("backgroundMode").toString();values["background_color"]=color->property("selectedColor").toString();
        values["mesh_width"]=retained_slider_value(previous["mesh_width"].toDouble(),meshWidth);values["mesh_contrast"]=retained_slider_value(previous["mesh_contrast"].toDouble(),meshContrast);
        values["mesh_color"]=meshColor->property("selectedColor").toString();values["auto_range_interval_seconds"]=rangeInterval->seconds();
    }
    void save(){
        auto values=rendering()["defaults"].toObject();save_values(values,SurfaceSettings::effective(rendering()));
        rendering()["version"]=1;rendering()["defaults"]=sceneAspect->apply(values);publish();load();
    }
    void select_mesh_color(){
        bool reset=false;const auto selected=meshColor->property("selectedColor").toString();
        const auto c=ui::select_color(selected=="auto"?QColor(8,23,28):QColor(selected),&owner,ui::text("Mesh color"),&reset,QColor(8,23,28));
        if(c.isValid()){meshColor->setProperty("selectedColor",reset?QString("auto"):c.name());save();}
    }
    void select_background(){
        bool reset=false;const auto fallback=ui::Theme::instance().palette().color(QPalette::Window);
        const auto initial=color->property("backgroundMode")=="auto"?fallback:QColor(color->property("selectedColor").toString());
        const auto selected=ui::select_color(initial,&owner,ui::text("Background"),&reset,fallback);
        if(selected.isValid()){if(!reset)color->setProperty("selectedColor",selected.name());color->setProperty("backgroundMode",reset?"auto":"custom");save();}
    }
    void connect_appearance_flags(){
        for(auto it=checks.begin();it!=checks.end();++it){if(it.key()=="space_grid")connect(it.value(),&QCheckBox::checkStateChanged,this,[this]{save();});else connect(it.value(),&QCheckBox::toggled,this,[this]{save();});}
    }
    void connect_appearance_sliders(){
        for(auto* slider:std::as_const(numbers))connect(slider,&QSlider::valueChanged,this,[this]{save();});
        connect(meshWidth,&QSlider::valueChanged,this,[this]{save();});connect(meshContrast,&QSlider::valueChanged,this,[this]{save();});
    }
    void connect_appearance(){
        sceneAspect->changed=[this]{save();};connect(rangeInterval->box,&QDoubleSpinBox::valueChanged,this,[this]{save();});
        connect_appearance_flags();connect_appearance_sliders();
        connect(meshColor,&QPushButton::clicked,this,[this]{select_mesh_color();});connect(color,&QPushButton::clicked,this,[this]{select_background();});
        connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this]{load();});connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{load();});
    }
    void setup_render_controls(){
        sceneAspect=new ui::SceneAspectControl(group);form->addRow(sceneAspect);
        rangeInterval=new ui::AutoRangeIntervalControl(group);form->addRow(rangeInterval);
        rangeInterval->box->setObjectName("surface_auto_range_interval_seconds");
        setup_flags();setup_scalar("brightness","Brightness");setup_scalar("light_strength","Light strength");
        setup_scalar("ambient_strength","Ambient light");setup_mesh();setup_background();
    }
public:
    explicit GraphSettingsPage(SettingsWidget& settings):QWidget(&settings),owner(settings){
        setup_appearance();setup_cadence();setup_workspace();setup_precision();
        setup_render_controls();connect_appearance();owner._refresh_graph_targets=[this]{load();};load();
    }
};
QWidget* SettingsWidget::_setup_graphs(){return new GraphSettingsPage(*this);}
