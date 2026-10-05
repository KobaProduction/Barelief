#include "visualization/ui_text.h"
#include "core/performance.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "pipeline/test_signal.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QSignalBlocker>
AudioNodeModel::AudioNodeModel(QString kind) : _kind(std::move(kind)) {
    auto style=nodeStyle(); style.ShadowEnabled=false; setNodeStyle(style);
    _parameters = {{"merge",2},{"channel",0},{"gain",1.0},{"history_size",400}};
    if (_kind == "TestSignal") _parameters = pipeline::TestSignalSettings{}.json();
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this] { emit requestNodeUpdate(); });
}
unsigned int AudioNodeModel::nPorts(QtNodes::PortType type) const {
    auto definition = pipeline::node_definition(_kind);
    return !(type == QtNodes::PortType::In ? definition.input : definition.output).isEmpty();
}
QtNodes::NodeDataType AudioNodeModel::dataType(QtNodes::PortType type, QtNodes::PortIndex) const {
    auto definition = pipeline::node_definition(_kind);
    auto id = type == QtNodes::PortType::In ? definition.input : definition.output;
    return {id,ui::text(id.toUtf8().constData())};
}
QJsonObject AudioNodeModel::save() const { auto json = _parameters; json["model-name"] = _kind; return json; }
void AudioNodeModel::load(const QJsonObject &json) { _parameters = json; sync_controls(); }
void AudioNodeModel::sync_controls() {
    if (!_widget) return;
    for (auto *combo : _widget->findChildren<QComboBox *>()) { QSignalBlocker block(combo); combo->setCurrentIndex(_parameters.value(combo->objectName()).toInt(combo->objectName()=="merge" ? 2 : 0)); }
    if (auto *gain = _widget->findChild<QDoubleSpinBox *>("gain")) { QSignalBlocker block(gain); gain->setValue(_parameters.value("gain").toDouble(1)); }
    if (auto *history = _widget->findChild<QSpinBox *>("history_size")) { QSignalBlocker block(history); history->setValue(_parameters.value("history_size").toInt(400)); }
    if (_kind == "TestSignal") {
        const auto defaults = pipeline::TestSignalSettings{}.json();
        for (auto *control : _widget->findChildren<QDoubleSpinBox *>()) {
            QSignalBlocker block(control);
            control->setValue(_parameters.value(control->objectName()).toDouble(defaults.value(control->objectName()).toDouble()));
        }
    }
}
QWidget *AudioNodeModel::embeddedWidget() {
    if (_widget) return _widget;
    _widget = new QWidget; _widget->setMinimumWidth(180);
    auto *layout = new QVBoxLayout(_widget);
    auto combo = [&](QString key, QStringList names) {
        auto *control = new QComboBox(_widget); control->setObjectName(key); ui::items(control,names);
        layout->addWidget(control);
        connect(control,&QComboBox::currentIndexChanged,this,[this,key](int value){ _parameters[key]=value; emit requestNodeUpdate(); });
    };
    if (_kind == "AudioSource") combo("source",{"System output","System microphone"});
    if (_kind == "TestSignal") {
        layout->addWidget(ui::make<QLabel>("Test source · no speaker output", _widget));
        const auto defaults = pipeline::TestSignalSettings{}.json();
        const auto number = [&](const char *key, const char *label, double min, double max, int decimals) {
            auto *control = new QDoubleSpinBox(_widget); control->setObjectName(key);
            control->setDecimals(decimals); control->setRange(min,max); ui::bind(control,"prefix",QString::fromLatin1(label));
            control->setValue(defaults.value(key).toDouble()); layout->addWidget(control);
            connect(control,&QDoubleSpinBox::valueChanged,this,[this,key](double value) {
                _parameters[key] = value; emit requestNodeUpdate();
            });
        };
        number("tones", "Tones ", 5,10,0);
        number("sample_rate", "Sample rate Hz ",8000,192000,0);
        number("minimum_hz", "Carrier min Hz ",10,90000,1);
        number("maximum_hz", "Carrier max Hz ",10,90000,1);
        number("amplitude", "Amplitude ",0,1,3);
        number("period_seconds", "Sweep period s ",0.1,3600,2);
        number("sweep_depth", "Frequency deviation ",0,0.95,3);
    }
    if (_kind == "SpectrumMerge") combo("merge",{"Mean","Maximum","Energy (RMS)"});
    if (_kind == "FFTView") { combo("channel",{"Left","Right","Merged"}); combo("merge",{"Mean","Maximum","Energy (RMS)"}); }
    if (_kind == "SpectrumGain" || _kind == "WLED") {
        auto *control = new QDoubleSpinBox(_widget); control->setObjectName("gain"); control->setRange(0,100); control->setSingleStep(0.1); ui::bind(control,"prefix","Gain × ");
        layout->addWidget(control); connect(control,&QDoubleSpinBox::valueChanged,this,[this](double value){ _parameters["gain"]=value; emit requestNodeUpdate(); });
    }
    if (_kind == "Spectrogram") {
        auto *control = new QSpinBox(_widget); control->setObjectName("history_size"); control->setRange(2,2000); ui::bind(control,"prefix","History ");
        layout->addWidget(control); connect(control,&QSpinBox::valueChanged,this,[this](int value){ _parameters["history_size"]=value; emit requestNodeUpdate(); });
    }
    if (_kind == "StereoFFT") layout->addWidget(ui::make<QLabel>("Stereo · 4096 samples · 60 Hz", _widget));
    if (_kind == "Mel") layout->addWidget(ui::make<QLabel>("50 Mel bands", _widget));
    _preview = ui::make<QLabel>("Waiting for audio", _widget);
    _preview->setFixedSize(210,22);
    layout->addWidget(_preview);
    sync_controls(); return _widget;
}
void AudioNodeModel::preview(pipeline::NodePreview value) {
    core::perf::Scope trace(core::perf::Stage::NodePreview);
    if (!_preview) return;
    const auto text = ui::text("%1 values · peak %2").arg(value.values).arg(value.peak,0,'g',3);
    if (_preview->text() != text) _preview->setText(text);
}

QString AudioNodeModel::caption() const {
    const QMap<QString,const char *> names{{"AudioSource","Audio source"},{"TestSignal","Test source"},
        {"StereoFFT","Stereo FFT"},{"SpectrumMerge","Spectrum merge"},{"SpectrumGain","Spectrum gain"},
        {"Mel","Mel"},{"Spectrogram","Spectrogram"},{"FFTView","FFT graph"},{"MelView","Mel graph"},
        {"AudioView","Waveform"},{"SpectrogramView","Spectrogram view"},{"WLED","WLED"}};
    return names.contains(_kind) ? ui::text(names.value(_kind)) : _kind;
}
