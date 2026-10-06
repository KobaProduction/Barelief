#include "visualization/ui_text.h"
#include "core/performance.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/node_preview_card.h"
#include <QComboBox>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
AudioNodeModel::AudioNodeModel(QString kind) : _kind(std::move(kind)) {
    update_node_style();
    connect(&ui::Theme::instance(), &ui::Theme::changed, this, [this] { update_node_style(); });
    _parameters = pipeline::node_definition(_kind).defaults;
    connect(&ui::Language::instance(), &ui::Language::changed, this, [this] { emit requestNodeUpdate(); });
}
void AudioNodeModel::update_node_style() {
        auto style = nodeStyle();
        const auto palette = QApplication::palette();
        style.setBackgroundColor(palette.color(QPalette::Window));
        style.Opacity = 1.0f;
        style.FontColor = palette.color(QPalette::WindowText);
        style.FontColorFaded = palette.color(QPalette::Disabled, QPalette::WindowText);
        style.NormalBoundaryColor = palette.color(QPalette::Mid);
        style.SelectedBoundaryColor = palette.color(QPalette::Highlight);
        style.ConnectionPointColor = palette.color(QPalette::Text);
        style.FilledConnectionPointColor = palette.color(QPalette::Highlight);
        style.ShadowEnabled = false;
        setNodeStyle(style);
        if (_widget) update_widget_style(_widget);
        for (auto widget : _controls) if (widget) update_widget_style(widget);
        emit requestNodeUpdate();
}
bool AudioNodeModel::eventFilter(QObject *object, QEvent *event) {
    if(event->type()==QEvent::PaletteChange) update_node_style();
    return QtNodes::NodeDelegateModel::eventFilter(object,event);
}
unsigned int AudioNodeModel::nPorts(QtNodes::PortType type) const {
    const auto definition = pipeline::node_definition(_kind);
    return !(type == QtNodes::PortType::In ? definition.input : definition.output).isEmpty();
}
QtNodes::NodeDataType AudioNodeModel::dataType(QtNodes::PortType type, QtNodes::PortIndex) const {
    const auto definition = pipeline::node_definition(_kind);
    auto id = type == QtNodes::PortType::In ? definition.input : definition.output;
    if (id == "typed")
        id = _parameters.value("data_type").toString(_parameters.value("type").toString("mel"));
    return {id, ui::text(id.toUtf8().constData())};
}
QJsonObject AudioNodeModel::save() const {
    auto json = _parameters;
    json["model-name"] = _kind;
    return json;
}
void AudioNodeModel::load(const QJsonObject &json) {
    _parameters = pipeline::node_definition(_kind).defaults;
    for (auto it = json.begin(); it != json.end(); ++it)
        _parameters[it.key()] = it.value();
    if ((_kind == "NetworkInput" || _kind == "NetworkOutput") && json.contains("type") && !json.contains("data_type"))
        _parameters["data_type"] = json["type"];
    sync_controls();
    update_previews();
}
void AudioNodeModel::set_parameter(const QString &key, const QJsonValue &value) {
    if (_parameters.value(key) == value)
        return;
    _parameters[key] = value;
    sync_controls();
    if(key=="enabled") _last_preview={};
    update_previews();
    emit requestNodeUpdate();
}
void AudioNodeModel::sync_controls() {
    for (auto widget : _controls) {
        if (!widget)
            continue;
        // Native spin boxes own a QLineEdit named qt_spinbox_lineedit. It is
        // presentation, not a parameter: touching it produced [null] on selection.
        for (auto *field : widget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (auto *control = qobject_cast<QComboBox *>(field)) {
            QSignalBlocker block(control);
            const auto value = _parameters.value(control->objectName());
            int selected = -1;
            for (int i = 0; i < control->count(); ++i)
                if (QJsonValue::fromVariant(control->itemData(i)) == value)
                    selected = i;
            control->setCurrentIndex(selected);
        } else if (auto *control = qobject_cast<QDoubleSpinBox *>(field)) {
            QSignalBlocker block(control);
            control->setValue(_parameters.value(control->objectName()).toDouble());
        } else if (auto *control = qobject_cast<QCheckBox *>(field)) {
            QSignalBlocker block(control);
            control->setChecked(_parameters.value(control->objectName()).toBool());
        } else if (auto *control = qobject_cast<QLineEdit *>(field)) {
            QSignalBlocker block(control);
            const auto value = _parameters.value(control->objectName());
            if (control->property("pipelineParameterType").toString()=="json" && !value.isUndefined()) {
                const auto wrapped=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
                control->setText(QString::fromUtf8(wrapped.mid(1,wrapped.size()-2)));
            } else control->setText(value.toString());
            control->setModified(false);
        }
        }
    }
}
void AudioNodeModel::update_widget_style(QWidget *widget) {
    // Native controls inherit the application's palette and Fusion style.
    // Only the custom QtNodes body needs the explicit semantic colors above.
    widget->setAutoFillBackground(true);
    widget->installEventFilter(this);
}
QWidget *AudioNodeModel::parameter_widget(QWidget *parent) {
    _controls.removeIf([](const auto &widget) { return widget.isNull(); });
    auto *widget = new QWidget(parent);
    widget->setObjectName("pipelineNodeParameters");
    update_widget_style(widget);
    _controls.append(widget);
    auto *layout = new QFormLayout(widget);
    layout->setContentsMargins(0,4,0,8);
    layout->setVerticalSpacing(4);
    layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto definition = pipeline::node_definition(_kind);
    for (auto it = definition.constraints.begin(); it != definition.constraints.end(); ++it) {
        const auto key = it.key();
        const auto metadata = it.value().toObject();
        const auto type = metadata.value("type").toString();
        QWidget *control = nullptr;
        if (type == "enum") {
            auto *combo = new QComboBox(widget);
            for (const auto &value : metadata.value("options").toArray()) {
                const auto option = value.toObject();
                combo->addItem(ui::text(option.value("label").toString().toUtf8().constData()),
                               option.value("value").toVariant());
            }
            connect(&ui::Language::instance(),&ui::Language::changed,combo,[combo,options=metadata.value("options").toArray()] {
                const QSignalBlocker blocker(combo);
                for(int i=0;i<options.size();++i)
                    combo->setItemText(i,ui::text(options[i].toObject().value("label").toString().toUtf8().constData()));
            });
            connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key](int index) {
                if (index >= 0)
                    set_parameter(key, QJsonValue::fromVariant(combo->itemData(index)));
            });
            control = combo;
        } else if (type == "integer" || type == "number") {
            auto *number = new QDoubleSpinBox(widget);
            number->setDecimals(type == "integer" ? 0 : 4);
            number->setRange(metadata.value("min").toDouble(-1e9), metadata.value("max").toDouble(1e9));
            number->setSingleStep(type == "integer" ? 1 : 0.1);
            connect(number, &QDoubleSpinBox::valueChanged, this,
                    [this, key](double value) { if (std::isfinite(value)) set_parameter(key, value); });
            control = number;
        } else if (type == "bool") {
            auto *boolean = new QCheckBox(widget);
            if(key=="enabled") ui::bind(boolean,"text","Output enabled");
            connect(boolean, &QCheckBox::toggled, this, [this, key](bool value) {
                const bool previous=_parameters.value(key).toBool(true);
                set_parameter(key, value);
                if(key=="enabled" && previous!=value && (_kind=="AudioSource" || _kind=="TestSignal"))
                    emit local_source_output_changed(value);
            });
            control = boolean;
        } else {
            auto *text = new QLineEdit(widget);
            text->setProperty("pipelineParameterType",type);
            connect(text, &QLineEdit::editingFinished, this, [this, text, key, type] {
                if (!text->isModified()) return;
                if (type == "json") {
                    const auto parsed = QJsonDocument::fromJson("["+text->text().toUtf8()+"]");
                    if (parsed.isArray() && parsed.array().size() == 1)
                        set_parameter(key, parsed.array()[0]);
                } else
                    set_parameter(key, text->text());
                text->setModified(false);
            });
            control = text;
        }
        control->setObjectName(key);
        auto *label = new QLabel(ui::text(metadata.value("label").toString(key).toUtf8().constData()), widget);
        ui::bind(label,"text",metadata.value("label").toString(key));
        label->setWordWrap(true);
        if(key=="enabled") { delete label; layout->addRow(control); }
        else layout->addRow(label, control);
    }
    sync_controls();
    return widget;
}
QWidget *AudioNodeModel::embeddedWidget() {
    if (_widget)
        return _widget;
    _widget = new QWidget;
    _widget->setObjectName("pipelineNodeBody");
    update_widget_style(_widget);
    // Equal embedded-body width; QtNodes still adds the actual port/caption
    // bounds independently, so semantic port labels remain fully visible.
    _widget->setFixedWidth(300);
    auto *layout = new QVBoxLayout(_widget);
    layout->setContentsMargins(8,8,8,8);
    layout->setSpacing(6);
    if (_kind == "TestSignal")
        layout->addWidget(ui::make<QLabel>("Signal generator · no speaker output", _widget));
    layout->addWidget(parameter_widget(_widget));
    layout->addWidget(preview_widget(_widget));
    return _widget;
}
void AudioNodeModel::preview(pipeline::NodePreview value) {
    core::perf::Scope trace(core::perf::Stage::NodePreview);
    _last_preview = _parameters.value("enabled").toBool(true) ? value : pipeline::NodePreview{};
    update_previews();
}
QWidget *AudioNodeModel::preview_widget(QWidget *parent) {
    auto *card=new NodePreviewCard(parent);
    _previews.removeIf([](const auto &widget) { return widget.isNull(); });
    _previews.append(card);
    update_previews();
    return card;
}
void AudioNodeModel::update_previews() {
    const auto definition=pipeline::node_definition(_kind);
    auto type=definition.output.isEmpty() ? definition.input : definition.output;
    if(type=="typed") type=_parameters.value("data_type").toString("mel");
    for(auto card:_previews)
        if(card) card->set_data(_last_preview,_parameters.value("enabled").toBool(true),type);
}
QString AudioNodeModel::caption() const {
    const QMap<QString, const char *> names{{"AudioSource", "Audio source"},
                                            {"TestSignal", "Signal generator"},
                                            {"StereoFFT", "Stereo FFT"},
                                            {"SpectrumMerge", "Spectrum merge"},
                                            {"SpectrumGain", "Spectrum gain"},
                                            {"Mel", "Mel"},
                                            {"Spectrogram", "Spectrogram"},
                                            {"FFTView", "FFT graph"},
                                            {"MelView", "Mel graph"},
                                            {"AudioView", "Waveform"},
                                            {"SpectrogramView", "Spectrogram view"},
                                            {"WLED", "WLED"}};
    return names.contains(_kind) ? ui::text(names.value(_kind)) : _kind;
}
