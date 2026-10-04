#include "core/performance.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QSignalBlocker>
AudioNodeModel::AudioNodeModel(QString kind) : _kind(std::move(kind)) {
    auto style=nodeStyle(); style.ShadowEnabled=false; setNodeStyle(style);
    _parameters = {{"merge",2},{"channel",0},{"gain",1.0},{"history_size",400}};
}
unsigned int AudioNodeModel::nPorts(QtNodes::PortType type) const {
    auto definition = pipeline::node_definition(_kind);
    return !(type == QtNodes::PortType::In ? definition.input : definition.output).isEmpty();
}
QtNodes::NodeDataType AudioNodeModel::dataType(QtNodes::PortType type, QtNodes::PortIndex) const {
    auto definition = pipeline::node_definition(_kind);
    auto id = type == QtNodes::PortType::In ? definition.input : definition.output;
    return {id,id};
}
QJsonObject AudioNodeModel::save() const { auto json = _parameters; json["model-name"] = _kind; return json; }
void AudioNodeModel::load(const QJsonObject &json) { _parameters = json; sync_controls(); }
void AudioNodeModel::sync_controls() {
    if (!_widget) return;
    for (auto *combo : _widget->findChildren<QComboBox *>()) { QSignalBlocker block(combo); combo->setCurrentIndex(_parameters.value(combo->objectName()).toInt(combo->objectName()=="merge" ? 2 : 0)); }
    if (auto *gain = _widget->findChild<QDoubleSpinBox *>("gain")) { QSignalBlocker block(gain); gain->setValue(_parameters.value("gain").toDouble(1)); }
    if (auto *history = _widget->findChild<QSpinBox *>("history_size")) { QSignalBlocker block(history); history->setValue(_parameters.value("history_size").toInt(400)); }
}
QWidget *AudioNodeModel::embeddedWidget() {
    if (_widget) return _widget;
    _widget = new QWidget; _widget->setMinimumWidth(180);
    auto *layout = new QVBoxLayout(_widget);
    auto combo = [&](QString key, QStringList names) {
        auto *control = new QComboBox(_widget); control->setObjectName(key); control->addItems(names);
        layout->addWidget(control);
        connect(control,&QComboBox::currentIndexChanged,this,[this,key](int value){ _parameters[key]=value; emit requestNodeUpdate(); });
    };
    if (_kind == "AudioSource") combo("source",{"System output","System microphone"});
    if (_kind == "SpectrumMerge") combo("merge",{"Mean","Maximum","Energy (RMS)"});
    if (_kind == "FFTView") { combo("channel",{"Left","Right","Merged"}); combo("merge",{"Mean","Maximum","Energy (RMS)"}); }
    if (_kind == "SpectrumGain" || _kind == "WLED") {
        auto *control = new QDoubleSpinBox(_widget); control->setObjectName("gain"); control->setRange(0,100); control->setSingleStep(0.1); control->setPrefix("Gain × ");
        layout->addWidget(control); connect(control,&QDoubleSpinBox::valueChanged,this,[this](double value){ _parameters["gain"]=value; emit requestNodeUpdate(); });
    }
    if (_kind == "Spectrogram") {
        auto *control = new QSpinBox(_widget); control->setObjectName("history_size"); control->setRange(2,2000); control->setPrefix("History ");
        layout->addWidget(control); connect(control,&QSpinBox::valueChanged,this,[this](int value){ _parameters["history_size"]=value; emit requestNodeUpdate(); });
    }
    if (_kind == "StereoFFT") layout->addWidget(new QLabel("Stereo · 4096 samples · 60 Hz",_widget));
    if (_kind == "Mel") layout->addWidget(new QLabel("50 Mel bands",_widget));
    _preview = new QLabel("Waiting for audio",_widget);
    _preview->setFixedSize(210,22);
    layout->addWidget(_preview);
    sync_controls(); return _widget;
}
void AudioNodeModel::preview(pipeline::NodePreview value) {
    core::perf::Scope trace(core::perf::Stage::NodePreview);
    if (!_preview) return;
    const auto text = QString("%1 values · peak %2").arg(value.values).arg(value.peak,0,'g',3);
    if (_preview->text() != text) _preview->setText(text);
}
