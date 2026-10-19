#include "barelief/ui_text.h"
#include "core/performance.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/node_preview_card.h"
#include "core_app/scheme_editor/embedded_widget_layout.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/node_parameter_form.h"
#include "core_app/scheme_editor/typed_parameter_helper.h"
#include "oscillator_bank_widget.h"
#include "core_app/wheel_input.h"
#include <desktop/numeric_project_document.h>
#include <domain/graph/node_factory.h>
#include <pipeline/numeric_projection.h>
#include <QSpinBox>
#include <QPushButton>
#include <QMenu>
#include <QComboBox>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QFormLayout>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QScrollArea>
#include <QToolButton>
#include <QMenu>
#include <cmath>
#include <algorithm>
#include <limits>
#include <utility>
namespace {
bool execution_sync_port(const domain::ConfiguredProcessingNode& node,QtNodes::PortType side) {
    if(side==QtNodes::PortType::In)return node.kind=="SignalGenerator" || node.kind=="Mixer";
    return node.kind=="SampleClock" || node.kind=="SignalGenerator";
}
const char* row_caption(const QString& kind,int id) {
    if(id==0)return kind=="BroadbandRow" ? "Broadband row" : "Note spectrum";
    if(id==1)return "Full note-grid agreement, %";
    return "Selected note-grid agreement, %";
}
const char* musical_spectrogram_caption(QtNodes::PortType type,int id) {
    if(type==QtNodes::PortType::In)return "Spectrum";
    if(id==0)return "Composite row: low, mid, high, notes";
    if(id==1)return "Full note-grid agreement, %";
    if(id==2)return "Selected note-grid agreement, %";
    return "Note spectrum";
}
}
AudioNodeModel::AudioNodeModel(QString kind) : _kind(std::move(kind)) {
    update_node_style();
    connect(&ui::Theme::instance(), &ui::Theme::changed, this, [this] { update_node_style(); });
    _numeric = numeric_editor::default_node(_kind);
    _parameters=new NodeParameterModel(this);
    bind_parameter_model();
    connect(&ui::Language::instance(), &ui::Language::changed, this, [this] { emit parameter_values_changed();update_previews();emit requestNodeUpdate(); });
}
void AudioNodeModel::bind_parameter_model() {
    _parameters->setEditHandlers([this](const auto& n){return edit_numeric(n);},[this](const auto& a){return edit_adapter(a);},[this](const auto& key,bool shown){return set_parameter_on_block(key,shown);});
    _parameters->setNode(*_numeric,true);
    connect(this,&AudioNodeModel::parameter_values_changed,_parameters,[this]{if(_numeric)_parameters->setNode(*_numeric,false);});
    connect(this,&AudioNodeModel::parameter_structure_changed,_parameters,[this]{if(_numeric)_parameters->setNode(*_numeric,true);});
    connect(this,&AudioNodeModel::adapter_changed,_parameters,[this]{_parameters->setAdapter(_adapter);});
    connect(this,&AudioNodeModel::presentation_changed,_parameters,[this]{_parameters->setPresentation(_presentation);});
    connect(this,&AudioNodeModel::capture_devices_changed,_parameters,[this]{_parameters->setCaptureDeviceProvider(_capture_device_provider);});
}
void AudioNodeModel::set_admission_limits(std::optional<domain::ProcessingAdmissionLimits> limits) {
    _admission_limits=limits;
    _parameters->setAdmissionLimits(std::move(limits));
    emit parameter_structure_changed();
}
void AudioNodeModel::update_node_style() {
        auto style = nodeStyle();
        const auto palette = QApplication::palette();
        style.setBackgroundColor(palette.color(QPalette::Window));
        style.Opacity = 1.0f;
        style.FontColor = palette.color(QPalette::WindowText);
        style.FontColorFaded = palette.color(QPalette::Disabled, QPalette::WindowText);
        const auto kind=domain::node_kind(_kind.toStdString());
        const auto role=kind?domain::node_role(*kind):domain::NodeRole::Processor;
        style.NormalBoundaryColor = palette.color(role==domain::NodeRole::Source?QPalette::Link:
            role==domain::NodeRole::Sink?QPalette::Highlight:QPalette::Mid);
        style.SelectedBoundaryColor = palette.color(QPalette::Highlight);
        style.ConnectionPointColor = palette.color(QPalette::Text);
        style.FilledConnectionPointColor = palette.color(QPalette::Highlight);
        style.ShadowEnabled = false;
        setNodeStyle(style);
        if (_widget) update_widget_style(_widget);
        emit requestNodeUpdate();
}
bool AudioNodeModel::eventFilter(QObject *object, QEvent *event) {
    if(event->type()==QEvent::PaletteChange) update_node_style();
    if(_widget && (object==_widget || (qobject_cast<QWidget*>(object) && _widget->isAncestorOf(qobject_cast<QWidget*>(object)))) &&
       event->type()==QEvent::LayoutRequest && !_widget->property("nodeSizePending").toBool())
        schedule_widget_size_update();
    return QtNodes::NodeDelegateModel::eventFilter(object,event);
}
void AudioNodeModel::schedule_widget_size_update() {
    _widget->setProperty("nodeSizePending",true);
    QTimer::singleShot(0,this,[this]{refresh_widget_size();});
}
void AudioNodeModel::refresh_widget_size() {
    if(!_widget)return;
    _widget->setProperty("nodeSizePending",false);
    if(!_widget->layout())return;
    const auto height=embeddedContentHeight(_widget);
    const int preferred=_widget->property("nodePreferredWidth").isValid()
        ? _widget->property("nodePreferredWidth").toInt() : 240;
    if(height!=_widget->height() || embeddedContentWidth(_widget,preferred)!=_widget->width())
        emit embeddedWidgetSizeUpdated();
}
unsigned int AudioNodeModel::nPorts(QtNodes::PortType type) const {
    if(!_numeric_load_error.isEmpty())return 0;
    if(_numeric)return visible_numeric_ports(type,*_numeric)+unsigned(execution_sync_port(*_numeric,type));
    return 0;
}
unsigned AudioNodeModel::visible_numeric_ports(QtNodes::PortType side,const domain::ConfiguredProcessingNode& node) const {
    const auto count=unsigned((side==QtNodes::PortType::In?node.inputs:node.outputs).size());
    return side==QtNodes::PortType::Out && (node.kind=="Fourier" || node.kind=="RealFourier") && !dc_outputs_shown() && !_dc_connections_visible?std::min(count,2u):count;
}
void AudioNodeModel::update_dc_visibility(bool shown,bool connected) {
    if(!_numeric || (_kind!="Fourier" && _kind!="RealFourier"))return;
    const auto before=nPorts(QtNodes::PortType::Out);
    const auto after=!shown && !connected?std::min(unsigned(_numeric->outputs.size()),2u):
        unsigned(_numeric->outputs.size())+unsigned(execution_sync_port(*_numeric,QtNodes::PortType::Out));
    publish_dc_port_visibility(before,after,shown,connected);
}
void AudioNodeModel::publish_dc_port_visibility(unsigned before,unsigned after,bool shown,bool connected) {
    if(after<before)emit portsAboutToBeDeleted(QtNodes::PortType::Out,after,before-1);
    if(after>before)emit portsAboutToBeInserted(QtNodes::PortType::Out,before,after-1);
    _dc_connections_visible=connected;
    if(shown)_presentation["show_dc_outputs"]=true;else _presentation.remove("show_dc_outputs");
    if(after<before)emit portsDeleted();
    if(after>before)emit portsInserted();
    emit requestNodeUpdate();
}
void AudioNodeModel::set_dc_connections_visible(bool connected) {
    if(_dc_connections_visible!=connected)update_dc_visibility(dc_outputs_shown(),connected);
}
bool AudioNodeModel::edit_dc_outputs_shown(bool shown) {
    if((_kind!="Fourier" && _kind!="RealFourier") || shown==dc_outputs_shown())return false;
    auto presentation=_presentation;presentation["show_dc_outputs"]=shown;
    if(_presentation_edit)return _presentation_edit(presentation);
    set_presentation(presentation);return true;
}
std::optional<domain::ConfigurationRole> AudioNodeModel::configuration_role(QtNodes::PortType side,QtNodes::PortIndex port) const {
    if(!_numeric)return {};unsigned index=visible_numeric_ports(side,*_numeric);
    if(index==port && execution_sync_port(*_numeric,side))return domain::ConfigurationRole::SampleClock;return {};
}
QtNodes::PortIndex AudioNodeModel::configuration_port(QtNodes::PortType side,domain::ConfigurationRole role) const {
    for(unsigned i=0;i<nPorts(side);++i)if(configuration_role(side,i)==role)return i;return QtNodes::InvalidPortIndex;
}
QtNodes::NodeDataType AudioNodeModel::dataType(QtNodes::PortType type, QtNodes::PortIndex port) const {
    if(port>=nPorts(type))return {};
    if(configuration_role(type,port))return {"configuration-clock",ui::text("Synchronization")};
    if(_numeric){const auto& ports=type==QtNodes::PortType::In?_numeric->inputs:_numeric->outputs;return {"numeric-contract",numeric_editor::port_caption(ports[port],!numeric_editor::is_graph_view(_kind))};}
    return {};
}
namespace {
const char* numeric_parameter_title(std::string_view key) {
    constexpr std::pair<std::string_view,const char*> titles[]={
        {"first_bin","First bin index"},{"first_midi","First MIDI note"},{"bin_hz","Frequency step, Hz"},{"power_gain","Window coefficient square sum"},
        {"coherent_gain","Window coefficient sum"},{"size","Buffer size, N"},{"fft_size","FFT size, N"},
        {"input_rate","Input sample rate, Fs"},{"sample_rate","Sampling frequency, Hz"}};
    for(const auto& [name,title]:titles)if(name==key)return title;
    return "Output sample rate, Fs";
}
const char* amplitude_caption(signal_contracts::Quantity quantity,bool normalize) {
    using Q=signal_contracts::Quantity;
    if(!normalize)return quantity==Q::SpectrumPower?"Raw magnitude squared":"Raw magnitude";
    switch(quantity){
    case Q::SpectrumMagnitude:return "Compensated FFT magnitude";
    case Q::SpectrumPower:return "Power spectrum";
    case Q::SpectrumPsd:return "Power spectral density (PSD)";
    case Q::SpectrumAmplitudeDb:return "Amplitude, dB";
    case Q::SpectrumPowerDb:return "Power, dB";
    case Q::SpectrumPsdDb:return "PSD, dB";
    default:return "Peak amplitude";
    }
}
const char* transform_output_caption(const QString& kind,domain::PortId id) {
    if(kind=="AmplitudePhase" || kind=="ComplexPolar")return id==0?(kind=="AmplitudePhase"?"Amplitude":"Raw magnitude"):"Phase, radians";
    switch(id){case 0:return "Re";case 1:return "Im";case 2:return "DC Re (zero frequency)";default:return "DC Im (zero frequency)";}
}
bool transform_boolean(const domain::ConfiguredProcessingNode& node,std::string_view key,bool fallback) {
    const auto* parameters=std::get_if<domain::TransformProcessingParameters>(&node.parameters);if(!parameters)return fallback;
    for(const auto& field:parameters->values)if(field.key==key)if(const auto* value=std::get_if<bool>(&field.value))return *value;
    return fallback;
}
}
namespace {
const char* spectrum_peak_port_title(const QString& kind, QtNodes::PortType direction, unsigned id) {
    if (kind == "SpectrumPeak" && direction == QtNodes::PortType::Out)
        return id == 0 ? "Peak frequency, Hz" : "Peak magnitude";
    if (kind == "SpectrumPeak" && direction == QtNodes::PortType::In && id == 0) return "Spectrum";
    if (kind == "WLEDEncode" && direction == QtNodes::PortType::In) {
        if (id == 1) return "Peak frequency, Hz";
        if (id == 2) return "Peak magnitude";
    }
    return nullptr;
}
}

QString AudioNodeModel::music_port_caption(QtNodes::PortType type,QtNodes::PortIndex port) const {
    if((_kind=="BroadbandRow" || _kind=="NoteSpectrum") && type==QtNodes::PortType::Out) {
        const auto id=port_id(type,port);
        if(_kind=="NoteSpectrum" && id==domain::note_first_output)
            return ui::text("First MIDI note")+" · "+dataType(type,port).name;
        return ui::text(row_caption(_kind,id))+"\n"+dataType(type,port).name;
    }
    if(_kind=="MusicalSpectrogram")
        return ui::text(musical_spectrogram_caption(type,port_id(type,port)))+"\n"+dataType(type,port).name;
    return {};
}
QString AudioNodeModel::numeric_port_caption(QtNodes::PortType type,QtNodes::PortIndex port) const {
    const auto output=numeric_output_caption(type,port);
    if(!output.isEmpty())return output;
    if(_numeric && (_kind=="WindowFunction" || _kind=="ArrayFill") && type==QtNodes::PortType::In)return ui::text("Array size, N")+"\n"+dataType(type,port).name;
    if(_numeric && _kind=="ArrayMultiply")return array_multiply_caption(type,port);
    if(_numeric && type==QtNodes::PortType::In){
        const auto key=domain::numeric_parameter_key(_numeric->kind,port_id(type,port));
        if(!key.empty())return ui::text(numeric_parameter_title(key))+"\n"+dataType(type,port).name;
    }
    return {};
}
QString AudioNodeModel::numeric_output_caption(QtNodes::PortType type,QtNodes::PortIndex port) const {
    if(!_numeric || type!=QtNodes::PortType::Out)return {};
    const auto id=port_id(type,port);
    if(domain::has_sample_rate_output(_numeric->kind) && id==domain::sample_rate_output)
        return QStringLiteral("Fs · ")+dataType(type,port).name;
    if(id==domain::coherent_gain_output)
        return ui::text("Window coefficient sum")+QStringLiteral(" · ")+dataType(type,port).name;
    if(id==domain::power_gain_output)
        return ui::text("Window coefficient square sum")+QStringLiteral(" · ")+dataType(type,port).name;
    return {};
}
QString AudioNodeModel::array_multiply_caption(QtNodes::PortType type,QtNodes::PortIndex port) const {
    bool add=false;
    if(const auto* parameters=std::get_if<domain::TransformProcessingParameters>(&_numeric->parameters)) {
        for(const auto& field:parameters->values) {
            if(field.key!="operation")continue;
            if(const auto* value=std::get_if<std::string>(&field.value))add=*value=="add";
        }
    }
    const auto label=type==QtNodes::PortType::In
        ? ui::text(add?"Term %1":"Factor %1").arg(unsigned(port_id(type,port))+1)
        : ui::text(add?"Sum":"Product");
    return label+"\n"+dataType(type,port).name;
}
QString AudioNodeModel::fourier_port_caption(QtNodes::PortType type,QtNodes::PortIndex port) const {
    if(_kind!="Fourier" && _kind!="RealFourier" && _kind!="ComplexPolar" && _kind!="AmplitudePhase")return {};
    const auto id=port_id(type,port);
    const char* label=type==QtNodes::PortType::In?(id==0?"Real input":"Imaginary input"):transform_output_caption(_kind,id);
    if(_kind=="AmplitudePhase" && type==QtNodes::PortType::Out && id==0){
        const auto data=_numeric->outputs[port].contract.view();const auto quantity=std::get<signal_contracts::NumericContract>(data).quantity;
        label=amplitude_caption(quantity,transform_boolean(*_numeric,"normalize",true));
    }
    return ui::text(label)+"\n"+dataType(type,port).name;
}
QString AudioNodeModel::routing_port_caption(QtNodes::PortType type,QtNodes::PortIndex port) const {
    if(_numeric && _kind=="InputSelector"){
        const auto& ports=type==QtNodes::PortType::In?_numeric->inputs:_numeric->outputs;
        const auto data=ports[port].contract.view();const auto* value=std::get_if<signal_contracts::NumericContract>(&data);
        if(value && value->shape.rank==signal_contracts::Rank::Scalar && value->unit==signal_contracts::NumericUnit::Hertz){
            const auto width=_numeric->outputs.size();return (type==QtNodes::PortType::In?stream_name(unsigned(port_id(type,port))/unsigned(width))+QStringLiteral(" · "):QString{})+QStringLiteral("Fs · ")+dataType(type,port).name;
        }
    }
    if(const auto channels=grouped_channels(type);channels>0) {
        const auto id=unsigned(port_id(type,port));return stream_name(id/channels)+" · "+ui::text("Channel %1").arg(id%channels+1);
    }
    if((_kind=="InputSelector" && type==QtNodes::PortType::Out) || (_kind=="Switch" && type==QtNodes::PortType::In))return ui::text("Channel %1").arg(unsigned(port_id(type,port))+1);
    return {};
}
QString AudioNodeModel::descriptive_port_caption(QtNodes::PortType type,QtNodes::PortIndex port) const {
    const auto labelKey=QString(type==QtNodes::PortType::In?"input%1":"output%1").arg(port_id(type,port));
    const auto custom=_presentation.value("port_labels").toObject().value(labelKey).toString();
    if(!custom.isEmpty())return custom+"\n"+dataType(type,port).name;
    if(configuration_role(type,port))return dataType(type,port).name;
    if(_numeric && _kind=="FrequencyStep" && port_id(type,port)==0)return (type==QtNodes::PortType::In?QStringLiteral("Fs"):ui::text("Frequency step, Hz"))+QStringLiteral(" · ")+dataType(type,port).name;
    if (const auto* title = spectrum_peak_port_title(_kind, type, unsigned(port_id(type, port)))) return ui::text(title) + "\n" + dataType(type, port).name;
    for(const auto method:{&AudioNodeModel::numeric_port_caption,&AudioNodeModel::music_port_caption,&AudioNodeModel::fourier_port_caption,&AudioNodeModel::routing_port_caption}) {
        const auto caption=(this->*method)(type,port);if(!caption.isEmpty())return caption;
    }
    return fallback_port_caption(type,port);
}
QString AudioNodeModel::fallback_port_caption(QtNodes::PortType type,QtNodes::PortIndex port) const {
    if(_numeric && _kind=="Spectrogram" && type==QtNodes::PortType::Out)
        return ui::text(port==0?"History":"Filled frames")+"\n"+dataType(type,port).name;
    const auto label=dataType(type,port).name;
    return nPorts(type)>1?QStringLiteral("%1 · %2").arg(unsigned(port_id(type,port))+1).arg(label):label;
}
QString AudioNodeModel::portCaption(QtNodes::PortType type,QtNodes::PortIndex port) const {
    const auto caption=descriptive_port_caption(type,port);const auto type_name=dataType(type,port).name;
    const auto suffix=QStringLiteral(" · ")+type_name;
    return caption.endsWith(suffix)?caption.left(caption.size()-suffix.size())+"\n"+type_name:caption;
}
QJsonObject AudioNodeModel::save() const {
    if(!_numeric_load_error.isEmpty())return {{"model-name",_kind},{"numeric_processing",_rejected_numeric_snapshot}};
    if(_numeric){QByteArray bytes;const auto result=desktop::encode_numeric_node(*_numeric,bytes);return {{"model-name",_kind},{"numeric_processing",QString::fromUtf8(bytes)},{"numeric_error",result.error},{"presentation",_presentation},{"adapter",_adapter}};}
    return {{"model-name",_kind}};
}
void AudioNodeModel::load(const QJsonObject &json) {
    if((json.contains("presentation") && !json.value("presentation").isObject()) || !valid_presentation(json.value("presentation").toObject())){
        reject_numeric_snapshot(json.value("numeric_processing").toString(),ui::text("Invalid node presentation"));return;
    }
    set_presentation(json.value("presentation").toObject());
    if((json.contains("adapter") && !json.value("adapter").isObject()) || !set_adapter(json.value("adapter").toObject())){
        reject_numeric_snapshot(json.value("numeric_processing").toString(),ui::text("Invalid network source settings"));return;
    }
    if(json.contains("numeric_processing")){load_numeric_snapshot(json.value("numeric_processing").toString());return;}
    _numeric.reset();
    _diagnostics.clear();
    _numeric_load_error=ui::text("Unsupported numeric draft version");
    _rejected_numeric_snapshot.clear();
    update_previews();
}
void AudioNodeModel::reject_numeric_snapshot(const QString& snapshot,const QString& error) {
    _numeric.reset();_numeric_load_error=error;_rejected_numeric_snapshot=snapshot;clear_preview();
}
void AudioNodeModel::load_numeric_snapshot(const QString& snapshot) {
    domain::ConfiguredProcessingNode node;
    const auto result=desktop::decode_numeric_node(snapshot.toUtf8(),node);
    if(result && node.kind==_kind.toStdString())set_numeric_node(node);
    else reject_numeric_snapshot(snapshot,result ? ui::text("Invalid node kind") : result.error);
}
namespace {
bool same_port_structure(const std::vector<domain::ProcessingPort>& before,const std::vector<domain::ProcessingPort>& after) {
    if(before.size()!=after.size())return false;
    for(std::size_t i=0;i<before.size();++i)if(before[i].id!=after[i].id || before[i].contract.view().index()!=after[i].contract.view().index())return false;
    return true;
}
bool same_port_ids(const std::vector<domain::ProcessingPort>& before,
                   const std::vector<domain::ProcessingPort>& after) {
    return before.size()==after.size() &&
        std::equal(before.begin(),before.end(),after.begin(),
                   [](const auto& left,const auto& right){return left.id==right.id;});
}
void sort_processing_ports(std::vector<domain::ProcessingPort>& ports) {
    std::sort(ports.begin(),ports.end(),[](const auto& left,const auto& right){return left.id<right.id;});
}
}
void AudioNodeModel::replace_port_side(QtNodes::PortType direction,const domain::ConfiguredProcessingNode& candidate) {
    auto& current=direction==QtNodes::PortType::In?_numeric->inputs:_numeric->outputs;
    const auto& next=direction==QtNodes::PortType::In?candidate.inputs:candidate.outputs;
    if(same_port_ids(current,next))return;
    const auto before=visible_numeric_ports(direction,*_numeric),after=visible_numeric_ports(direction,candidate);
    // The graph owner detaches retained edges by stable ID before this transition.
    if(before)emit portsAboutToBeDeleted(direction,0,before-1);
    current.clear();
    if(before)emit portsDeleted();
    if(after)emit portsAboutToBeInserted(direction,0,after-1);
    current=next;
    if(after)emit portsInserted();
}
void AudioNodeModel::publish_numeric_change(const std::optional<domain::ConfiguredProcessingNode>& prior,bool structure_changed) {
    if(structure_changed)emit parameter_structure_changed();else emit parameter_values_changed();
    _diagnostics.clear();update_previews();emit requestNodeUpdate();
    if(prior && prior->enabled!=_numeric->enabled && (_kind=="AudioSource" || _kind=="TestSignal"))emit local_source_output_changed(_numeric->enabled);
}
void AudioNodeModel::set_numeric_node(const domain::ConfiguredProcessingNode& value) {
    if(value.kind!=_kind.toStdString())return;
    auto candidate=value;
    sort_processing_ports(candidate.inputs);
    sort_processing_ports(candidate.outputs);
    const auto prior=_numeric;
    const bool structure_changed=prior && (!same_port_structure(prior->inputs,candidate.inputs) || !same_port_structure(prior->outputs,candidate.outputs));
    if(_numeric)for(const auto direction:{QtNodes::PortType::In,QtNodes::PortType::Out})replace_port_side(direction,candidate);
    _numeric_load_error.clear();_rejected_numeric_snapshot.clear();_numeric=std::move(candidate);
    publish_numeric_change(prior,structure_changed);
}
bool AudioNodeModel::edit_numeric(domain::ConfiguredProcessingNode value) {
    if(_numeric_edit){const auto accepted=_numeric_edit(value);if(!accepted)emit parameter_values_changed();return accepted;}
    set_numeric_node(value);return true;
}
bool AudioNodeModel::set_adapter(const QJsonObject& value) {
    QJsonObject checked;
    if(!desktop::decode_numeric_node_adapter(QJsonDocument(value).toJson(QJsonDocument::Compact),checked))return false;
    if(_adapter==checked)return true;
    _adapter=std::move(checked);emit adapter_changed();emit requestNodeUpdate();return true;
}
bool AudioNodeModel::edit_adapter(const QJsonObject& value) {
    QJsonObject checked;
    if(!desktop::decode_numeric_node_adapter(QJsonDocument(value).toJson(QJsonDocument::Compact),checked))return false;
    if(_adapter==checked)return false;
    return _adapter_edit?_adapter_edit(checked):set_adapter(checked);
}
namespace {
bool valid_presentation_name(const QJsonValue& value) {
    if(!value.isString())return false;const auto name=value.toString();
    return name.size()<=256 && std::none_of(name.begin(),name.end(),[](QChar c){return c.category()==QChar::Other_Control;});
}
bool valid_port_key(const QString& key) {
    bool valid=false;const auto id=key.toUInt(&valid);
    return valid && id<=std::numeric_limits<domain::PortId>::max() && QString::number(id)==key;
}
bool valid_port_labels(const QJsonValue& value) {
    if(value.isUndefined())return true;if(!value.isObject())return false;
    const auto labels=value.toObject();if(labels.size()>512)return false;
    for(auto it=labels.begin();it!=labels.end();++it){
        const auto prefix=it.key().startsWith("input")?5:it.key().startsWith("output")?6:0;
        if(!prefix || !valid_port_key(it.key().sliced(prefix)) || !valid_presentation_name(it.value()))return false;
    }
    return true;
}
bool valid_stream_names(const QJsonValue& value) {
    if(value.isUndefined())return true;if(!value.isArray())return false;
    const auto names=value.toArray();if(names.size()>64)return false;
    return std::all_of(names.begin(),names.end(),[](const auto& name){return valid_presentation_name(name);});
}
bool valid_scalar_input_metadata(const QJsonValue& value) {
    if(!value.isObject())return false;
    const auto input=value.toObject();
    if(input.contains("enabled") && !input.value("enabled").isBool())return false;
    return !input.contains("display_name") || valid_presentation_name(input.value("display_name"));
}
bool valid_scalar_input_labels(const QJsonValue& value) {
    if(value.isUndefined())return true;if(!value.isObject())return false;
    const auto inputs=value.toObject();if(inputs.size()>int(domain::max_variadic_inputs))return false;
    for(auto it=inputs.begin();it!=inputs.end();++it){
        if(!valid_port_key(it.key()) || !valid_scalar_input_metadata(it.value()))return false;
    }
    return true;
}
bool valid_inline_parameters(const QJsonValue& value) {
    if(value.isUndefined())return true;if(!value.isArray())return false;
    const auto keys=value.toArray();QSet<QString> seen;if(keys.size()>128)return false;
    for(const auto key:keys){
        if(!key.isString() || key.toString().isEmpty() || key.toString().toUtf8().size()>1024 || seen.contains(key.toString()))return false;
        seen.insert(key.toString());
    }
    return true;
}
}
bool AudioNodeModel::valid_presentation(const QJsonObject& value) {
    if(value.contains("show_dc_outputs") && !value.value("show_dc_outputs").isBool())return false;
    if(value.contains("display_name") && !valid_presentation_name(value.value("display_name")))return false;
    return valid_port_labels(value.value("port_labels")) && valid_stream_names(value.value("stream_names")) &&
        valid_scalar_input_labels(value.value("scalar_inputs")) && valid_inline_parameters(value.value("inline_parameters"));
}
bool AudioNodeModel::edit_display_name(const QString& name){
    auto value=_presentation;const auto title=name.trimmed();
    if(title.isEmpty())value.remove("display_name");else value["display_name"]=title;
    if(!valid_presentation(value) || value==_presentation)return false;
    if(_presentation_edit)return _presentation_edit(value);
    set_presentation(value);return true;
}
void AudioNodeModel::set_presentation(const QJsonObject& value) {
    if(!valid_presentation(value))return;
    if(_presentation==value)return;
    update_dc_visibility(value.value("show_dc_outputs").toBool(),_dc_connections_visible);
    _presentation=value;emit presentation_changed();
    if(_widget){_widget->layout()->invalidate();_widget->layout()->activate();}
    emit embeddedWidgetSizeUpdated();emit requestNodeUpdate();
}
bool AudioNodeModel::parameter_on_block(const QString& key) const {
    if(key=="numericEnabled" || key=="numericBypass")return false;
    if(key=="numericSelected" && (_kind=="InputSelector" || _kind=="Switch"))return true;
    if(key=="numericGate" && _kind=="Gate")return true;
    if(key=="numericOutputs" && _kind=="Splitter")return true;
    return _presentation.value("inline_parameters").toArray().contains(key);
}
domain::PortId AudioNodeModel::port_id(QtNodes::PortType side,QtNodes::PortIndex index) const {
    if(!_numeric)return domain::PortId(index);
    const auto& ports=side==QtNodes::PortType::In?_numeric->inputs:_numeric->outputs;
    return index<ports.size()?ports[index].id:domain::PortId{};
}
QtNodes::PortIndex AudioNodeModel::port_index(QtNodes::PortType side,domain::PortId id) const {
    if(!_numeric)return id<nPorts(side)?id:QtNodes::InvalidPortIndex;
    const auto& ports=side==QtNodes::PortType::In?_numeric->inputs:_numeric->outputs;
    for(unsigned i=0;i<visible_numeric_ports(side,*_numeric);++i)if(ports[i].id==id)return i;
    return QtNodes::InvalidPortIndex;
}
domain::PortId AudioNodeModel::selected_input() const {
    if(_numeric && _kind=="InputSelector")for(const auto& p:std::get<domain::TransformProcessingParameters>(_numeric->parameters).values)
        if(p.key=="selected_input")return std::visit([](auto v){return domain::PortId(v);},std::get<signal_contracts::NumericLiteral>(p.value));
    return 0;
}
bool AudioNodeModel::select_input(domain::PortId id) {
    if(!_numeric || _kind!="InputSelector" || (domain::data_outputs(*_numeric).size()>1?id>=_numeric->inputs.size()/domain::data_outputs(*_numeric).size():port_index(QtNodes::PortType::In,id)==QtNodes::InvalidPortIndex))return false;
    auto n=*_numeric;auto& fields=std::get<domain::TransformProcessingParameters>(n.parameters).values;
    auto selected=std::find_if(fields.begin(),fields.end(),[](const auto& field){return field.key=="selected_input";});
    if(selected==fields.end())fields.push_back({"selected_input",signal_contracts::NumericLiteral{std::int64_t(id)}});
    else selected->value=signal_contracts::NumericLiteral{std::int64_t(id)};
    return edit_numeric(std::move(n));
}
bool AudioNodeModel::can_add_configured_input() const {
    if(!_numeric || !_admission_limits || _numeric->inputs.empty())return false;
    const auto step=_kind=="InputSelector"?domain::data_outputs(*_numeric).size():std::size_t{1};
    const auto count=_numeric->inputs.size(),maximum=_admission_limits->max_ports_per_node;
    return step && count<=maximum && step<=maximum-count && _numeric->inputs.back().id<std::numeric_limits<domain::PortId>::max();
}
bool AudioNodeModel::add_configured_input() {
    if(!can_add_configured_input())return false;
    auto node=*_numeric;
    if(!numeric_editor::add_input(node) || !domain::derive_processing_node_contracts(node,*_admission_limits))return false;
    return edit_numeric(std::move(node));
}
bool AudioNodeModel::remove_configured_input(domain::PortId id) {
    if(!_numeric)return false;auto node=*_numeric;
    return numeric_editor::remove_input(node,id) && edit_numeric(std::move(node));
}
void AudioNodeModel::append_stream_menu(QMenu* menu) {
    const QPointer<AudioNodeModel> guard(this);
    auto* add=menu->addAction(ui::text("Add stream"));add->setEnabled(can_add_configured_input());
    connect(add,&QAction::triggered,menu,[guard]{if(guard)guard->add_configured_input();});
    auto* remove=menu->addAction(ui::text("Remove last stream"));remove->setEnabled(_numeric->inputs.size()>domain::data_outputs(*_numeric).size());
    connect(remove,&QAction::triggered,menu,[guard]{if(guard && guard->_numeric && !guard->_numeric->inputs.empty())guard->remove_configured_input(guard->_numeric->inputs.back().id);});
}
void AudioNodeModel::append_input_menu(QMenu* menu) {
    const QPointer<AudioNodeModel> guard(this);
    auto* add=menu->addAction(ui::text(_kind=="ArrayMultiply"?"Add factor":"Add input"));add->setEnabled(can_add_configured_input());
    connect(add,&QAction::triggered,menu,[guard]{if(guard)guard->add_configured_input();});
    auto* remove=menu->addMenu(ui::text(_kind=="ArrayMultiply"?"Remove factor":"Remove input"));remove->setEnabled(_numeric->inputs.size()>(_kind=="ArrayMultiply"?2u:1u));
    for(const auto& port:_numeric->inputs){auto* action=remove->addAction(ui::text(_kind=="ArrayMultiply"?"Factor %1":"Input %1").arg(unsigned(port.id)+1));
        connect(action,&QAction::triggered,menu,[guard,id=port.id]{if(guard)guard->remove_configured_input(id);});}
}
void AudioNodeModel::populate_selector_menu(QMenu* menu) {
    if(!_numeric || (_kind!="InputSelector" && _kind!="ArrayMultiply"))return;
    if(domain::data_outputs(*_numeric).size()>1)append_stream_menu(menu);else append_input_menu(menu);
}
std::vector<QtNodes::PortIndex> AudioNodeModel::visual_port_order(QtNodes::PortType side) const {
    std::vector<QtNodes::PortIndex> order;
    const auto saved_order=_presentation.value(side==QtNodes::PortType::In?"input_port_order":"output_port_order").toArray();
    for(const auto value:saved_order){
        const int id=value.toInt(-1);const auto index=id>=0 && id<=65535?port_index(side,domain::PortId(id)):QtNodes::InvalidPortIndex;if(index!=QtNodes::InvalidPortIndex && std::find(order.begin(),order.end(),index)==order.end())order.push_back(index);
    }
    for(unsigned id=0;id<nPorts(side);++id)if(std::find(order.begin(),order.end(),id)==order.end())order.push_back(id);
    return order;
}
namespace {
bool edit_inline_parameter_keys(QJsonArray& keys,const QString& key,bool shown) {
    if(shown){if(keys.size()>=128)return false;keys.append(key);}
    else for(int i=keys.size()-1;i>=0;--i)if(keys[i]==key)keys.removeAt(i);
    return true;
}
bool scalar_view_kind(const QString& kind) {
    const auto id=domain::node_kind(kind.toStdString());
    return id && domain::is_scalar_view(*id);
}
}
bool AudioNodeModel::set_parameter_on_block(const QString& key,bool shown) {
    if(key=="numericEnabled" || key=="numericBypass")return false;
    if(key.isEmpty() || key.size()>128 || parameter_on_block(key)==shown)return false;
    auto value=_presentation;auto keys=value.value("inline_parameters").toArray();
    if(!edit_inline_parameter_keys(keys,key,shown))return false;
    value["inline_parameters"]=keys;
    if(_presentation_edit)return _presentation_edit(value);
    set_presentation(value);return true;
}
bool AudioNodeModel::add_scalar_input(){
    if(!_numeric || !scalar_view_kind(_kind) || !_admission_limits || _numeric->inputs.size()>=_admission_limits->max_ports_per_node)return false;
    auto node=*_numeric;if(!numeric_editor::add_input(node))return false;
    if(_scalar_topology_edit)return _scalar_topology_edit(node,_presentation);
    return edit_numeric(std::move(node));
}
bool AudioNodeModel::remove_scalar_input(domain::PortId id){
    if(!_numeric || !scalar_view_kind(_kind))return false;
    auto node=*_numeric;if(!numeric_editor::remove_input(node,id))return false;
    auto presentation=_presentation;auto inputs=presentation.value("scalar_inputs").toObject();inputs.remove(QString::number(id));presentation["scalar_inputs"]=inputs;
    if(_scalar_topology_edit)return _scalar_topology_edit(node,presentation);
    if(!edit_numeric(std::move(node)))return false;set_presentation(presentation);return true;
}
bool AudioNodeModel::edit_scalar_input_name(domain::PortId id,const QString& text){
    if(!_numeric || !scalar_view_kind(_kind) || port_index(QtNodes::PortType::In,id)==QtNodes::InvalidPortIndex)return false;
    auto presentation=_presentation;auto inputs=presentation.value("scalar_inputs").toObject();auto input=inputs.value(QString::number(id)).toObject();input["display_name"]=text.trimmed();inputs[QString::number(id)]=input;presentation["scalar_inputs"]=inputs;
    if(!valid_presentation(presentation))return false;
    if(_presentation_edit)return _presentation_edit(presentation);set_presentation(presentation);return true;
}
bool AudioNodeModel::set_scalar_input_enabled(domain::PortId id,bool enabled){
    if(!_numeric || !scalar_view_kind(_kind) || port_index(QtNodes::PortType::In,id)==QtNodes::InvalidPortIndex)return false;
    auto presentation=_presentation;auto inputs=presentation.value("scalar_inputs").toObject();auto input=inputs.value(QString::number(id)).toObject();input["enabled"]=enabled;inputs[QString::number(id)]=input;presentation["scalar_inputs"]=inputs;
    if(_presentation_edit)return _presentation_edit(presentation);set_presentation(presentation);return true;
}
QWidget* AudioNodeModel::numeric_parameter_widget(QWidget* parent,bool inline_form) {
    NodeParameterForm::CompositeFactory composite;
    if(_kind=="SignalGenerator")composite=[](auto* model,auto* owner){return new OscillatorBankWidget(model,owner);};
    auto* form=new NodeParameterForm(_parameters,inline_form,parent,false,std::move(composite));update_widget_style(form);
    connect(_parameters,&NodeParameterModel::structureChanged,form,[this]{
        QTimer::singleShot(0,this,[this]{if(_widget){_widget->layout()->invalidate();_widget->layout()->activate();}emit embeddedWidgetSizeUpdated();});
    });
    if(!inline_form && (_kind=="InputSelector" || _kind=="Switch"))return selector_parameter_widget(parent,form);
    if(!scalar_view_kind(_kind) || inline_form)return form;
    return scalar_parameter_widget(parent,form,inline_form);
}
QWidget* AudioNodeModel::selector_parameter_widget(QWidget* parent,QWidget* form) {
    auto* container=new QWidget(parent);container->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    auto* layout=new QVBoxLayout(container);layout->setContentsMargins(0,0,0,0);
    layout->addWidget(stream_names_widget(container));layout->addWidget(form);return container;
}
QWidget* AudioNodeModel::scalar_parameter_widget(QWidget* parent,QWidget* form,bool inline_form) {
    auto* container=new QWidget(parent);auto* layout=new QVBoxLayout(container);layout->setContentsMargins(0,0,0,0);layout->addWidget(form);
    container->setSizePolicy(inline_form?QSizePolicy::Preferred:QSizePolicy::Ignored,QSizePolicy::Preferred);
    layout->addWidget(scalar_inputs_widget(container,inline_form));return container;
}
std::vector<unsigned> AudioNodeModel::stream_ids() const {
    std::vector<unsigned> result;if(!_numeric)return result;
    if(_kind=="InputSelector")return selector_stream_ids();
    for(unsigned i=0;i<std::get<domain::RouteProcessingParameters>(_numeric->parameters).outputs;++i)result.push_back(i);
    return result;
}
std::vector<unsigned> AudioNodeModel::selector_stream_ids() const {
    std::vector<unsigned> result;
    const auto channels=domain::data_outputs(*_numeric).size();if(!channels)return result;
    for(const auto& port:_numeric->inputs){
        const auto id=unsigned(port.id)/unsigned(channels);
        if(std::find(result.begin(),result.end(),id)==result.end())result.push_back(id);
    }
    return result;
}
void AudioNodeModel::edit_stream_name(unsigned id,const QString& name) {
    auto presentation=_presentation;auto names=presentation.value("stream_names").toArray();
    while(names.size()<=int(id))names.append(QString{});
    names[int(id)]=name.trimmed();presentation["stream_names"]=names;
    if(_presentation_edit)_presentation_edit(presentation);else set_presentation(presentation);
}
void AudioNodeModel::append_stream_name(QVBoxLayout* rows,unsigned id) {
    auto* edit=new QLineEdit(rows->parentWidget());edit->setMaxLength(256);edit->setObjectName(QString("streamName%1").arg(id));
    edit->setPlaceholderText(ui::text("Stream %1").arg(id+1));const auto names=_presentation.value("stream_names").toArray();
    if(id<unsigned(names.size()))edit->setText(names[int(id)].toString());rows->addWidget(edit);
    const QPointer<AudioNodeModel> guard(this);
    connect(edit,&QLineEdit::editingFinished,edit,[guard,edit,id]{if(guard)guard->edit_stream_name(id,edit->text());});
}
void AudioNodeModel::rebuild_stream_names(QWidget* groups) {
    const auto group_children=groups->findChildren<QWidget*>(QString{},Qt::FindDirectChildrenOnly);
    for(auto* child:group_children)delete child;
    delete groups->layout();auto* rows=new QVBoxLayout(groups);rows->setContentsMargins(0,0,0,0);
    for(const auto id:stream_ids())append_stream_name(rows,id);
}
QWidget* AudioNodeModel::stream_names_widget(QWidget* parent) {
    auto* groups=new QWidget(parent);groups->setObjectName("streamNamesEditor");const QPointer<AudioNodeModel> guard(this);
    auto rebuild=[guard,groups]{if(guard && guard->_numeric)guard->rebuild_stream_names(groups);};rebuild();
    auto schedule=[groups,rebuild]{if(groups->property("rebuildPending").toBool())return;groups->setProperty("rebuildPending",true);QTimer::singleShot(0,groups,[groups,rebuild]{groups->setProperty("rebuildPending",false);rebuild();});};
    connect(this,&AudioNodeModel::parameter_structure_changed,groups,schedule);connect(this,&AudioNodeModel::presentation_changed,groups,schedule);connect(&ui::Language::instance(),&ui::Language::changed,groups,schedule);
    return groups;
}
namespace {
QLineEdit* scalar_name_editor(QWidget* parent,domain::PortId id,const QJsonObject& metadata) {
    auto* name=new QLineEdit(metadata.value("display_name").toString(),parent);
    name->setObjectName(QString("scalarInput%1Name").arg(id));name->setMaxLength(256);
    name->setPlaceholderText(ui::text("Value %1").arg(unsigned(id)+1));return name;
}
struct ScalarInputControls {QWidget* row;QCheckBox* enabled;QLineEdit* name;QPushButton* remove;};
ScalarInputControls scalar_input_controls(QWidget* parent,domain::PortId id,const QJsonObject& metadata,bool removable) {
    auto* row=new QWidget(parent);auto* vertical=new QVBoxLayout(row);vertical->setContentsMargins(0,0,0,0);
    auto* fields=new QHBoxLayout;vertical->addLayout(fields);fields->setContentsMargins(0,0,0,0);
    auto* enabled=new QCheckBox(row);enabled->setObjectName(QString("scalarInput%1Enabled").arg(id));
    ui::bind(enabled,"toolTip","Show value");enabled->setChecked(metadata.value("enabled").toBool(true));fields->addWidget(enabled);
    auto* name=scalar_name_editor(row,id,metadata);vertical->insertWidget(0,name);
    auto* remove=new QPushButton(ui::text("Remove input"),row);remove->setObjectName(QString("scalarInput%1Remove").arg(id));
    remove->setEnabled(removable);fields->addWidget(remove);return {row,enabled,name,remove};
}
}
void AudioNodeModel::append_scalar_input(QVBoxLayout* rows,domain::PortId id,const QJsonObject& metadata,bool inline_form) {
    const auto controls=scalar_input_controls(rows->parentWidget(),id,metadata,_numeric->inputs.size()>1);rows->addWidget(controls.row);
    if(!inline_form){controls.row->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);controls.remove->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);}
    const QPointer<AudioNodeModel> guard(this);
    connect(controls.enabled,&QCheckBox::toggled,controls.row,[guard,id](bool value){if(guard)guard->set_scalar_input_enabled(id,value);});
    connect(controls.name,&QLineEdit::editingFinished,controls.row,[guard,name=controls.name,id]{if(guard)guard->edit_scalar_input_name(id,name->text());});
    connect(controls.remove,&QPushButton::clicked,controls.row,[guard,id]{if(guard)guard->remove_scalar_input(id);});
}
void AudioNodeModel::append_scalar_input_action(QVBoxLayout* rows,bool inline_form) {
    auto* add=new QPushButton(ui::text("Add input"),rows->parentWidget());add->setObjectName("addScalarInput");
    add->setEnabled(_admission_limits && _numeric->inputs.size()<_admission_limits->max_ports_per_node);rows->addWidget(add);
    const QPointer<AudioNodeModel> guard(this);connect(add,&QPushButton::clicked,add,[guard]{if(guard)guard->add_scalar_input();});
    if(!inline_form)add->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
}
void AudioNodeModel::rebuild_scalar_inputs(QWidget* inputs,bool inline_form) {
    const auto input_children=inputs->findChildren<QWidget*>(QString{},Qt::FindDirectChildrenOnly);
    for(auto* child:input_children)delete child;
    delete inputs->layout();auto* rows=new QVBoxLayout(inputs);rows->setContentsMargins(0,0,0,0);
    if(!_numeric)return;
    const auto metadata=_presentation.value("scalar_inputs").toObject();
    for(const auto& port:_numeric->inputs)append_scalar_input(rows,port.id,metadata.value(QString::number(port.id)).toObject(),inline_form);
    append_scalar_input_action(rows,inline_form);
}
QWidget* AudioNodeModel::scalar_inputs_widget(QWidget* parent,bool inline_form){
    auto* inputs=new QWidget(parent);inputs->setObjectName("scalarInputsEditor");
    inputs->setSizePolicy(inline_form?QSizePolicy::Preferred:QSizePolicy::Ignored,QSizePolicy::Preferred);
    connect(this,&QObject::destroyed,inputs,[inputs]{inputs->setEnabled(false);});
    bind_scalar_input_updates(inputs,inline_form);
    return inputs;
}
void AudioNodeModel::bind_scalar_input_updates(QWidget* inputs,bool inline_form) {
    const QPointer<AudioNodeModel> guard(this);
    auto rebuild=[guard,inputs,inline_form]{if(guard)guard->rebuild_scalar_inputs(inputs,inline_form);};rebuild();
    auto schedule=[inputs,rebuild]{if(inputs->property("scalarRebuildPending").toBool())return;inputs->setProperty("scalarRebuildPending",true);QTimer::singleShot(0,inputs,[inputs,rebuild]{inputs->setProperty("scalarRebuildPending",false);rebuild();});};
    connect(this,&AudioNodeModel::parameter_structure_changed,inputs,schedule);connect(this,&AudioNodeModel::presentation_changed,inputs,schedule);connect(&ui::Language::instance(),&ui::Language::changed,inputs,schedule);
}
void AudioNodeModel::update_widget_style(QWidget *widget) {
    // Native controls inherit the application's palette and Fusion style.
    // Only the custom QtNodes body needs the explicit semantic colors above.
    widget->setAutoFillBackground(true);
    widget->installEventFilter(this);
}
void AudioNodeModel::edit_port_contracts(QWidget* parent) {
    QDialog dialog(parent);dialog.setObjectName("portContractsDialog");ui::bind(&dialog,"windowTitle","Port contracts");dialog.resize(560,620);
    auto* content=new QVBoxLayout(&dialog);auto* scroll=new QScrollArea(&dialog);scroll->setWidgetResizable(true);
    scroll->setWidget(new NodeParameterForm(_parameters,false,scroll,true));content->addWidget(scroll);
    auto* close=ui::make<QPushButton>("Close",&dialog);content->addWidget(close);
    connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);dialog.exec();
}
std::pair<QString,QLineEdit*> AudioNodeModel::port_label_editor(QWidget* parent,QtNodes::PortType side,domain::PortId port) const {
    const auto key=QString(side==QtNodes::PortType::In?"input%1":"output%1").arg(port);
    auto* edit=new QLineEdit(parent);edit->setObjectName("portLabel_"+key);edit->setMaxLength(256);
    edit->setText(_presentation.value("port_labels").toObject().value(key).toString());
    edit->setPlaceholderText(portCaption(side,port_index(side,port)).replace('\n'," · "));return {key,edit};
}
AudioNodeModel::PortLabelEdits AudioNodeModel::append_port_label_editors(QFormLayout* rows) const {
    PortLabelEdits edits;
    for(const auto side:{QtNodes::PortType::In,QtNodes::PortType::Out})for(const auto& port:side==QtNodes::PortType::In?_numeric->inputs:_numeric->outputs){
        auto editor=port_label_editor(rows->parentWidget(),side,port.id);
        rows->addRow(ui::text(side==QtNodes::PortType::In?"Input port %1":"Output port %1").arg(port.id),editor.second);edits.push_back(editor);
    }
    return edits;
}
void AudioNodeModel::apply_port_labels(const PortLabelEdits& edits) {
    auto presentation=_presentation;auto labels=presentation.value("port_labels").toObject();
    for(const auto& [key,edit]:edits){const auto text=edit->text().trimmed();if(text.isEmpty())labels.remove(key);else labels[key]=text;}
    if(labels.isEmpty())presentation.remove("port_labels");else presentation["port_labels"]=labels;
    if(_presentation_edit)_presentation_edit(presentation);else set_presentation(presentation);
}
namespace {
void append_dialog_buttons(QDialog& dialog,QVBoxLayout* content) {
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    content->addWidget(buttons);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
}
}
void AudioNodeModel::edit_port_labels(QWidget* parent) {
    if(!_numeric)return;const QPointer<AudioNodeModel> guard(this);
    QDialog dialog(parent);dialog.setObjectName("portLabelsDialog");ui::bind(&dialog,"windowTitle","Port labels");dialog.resize(560,420);
    auto* content=new QVBoxLayout(&dialog);auto* scroll=new QScrollArea(&dialog);scroll->setWidgetResizable(true);
    auto* body=new QWidget(scroll);auto* rows=new QFormLayout(body);scroll->setWidget(body);content->addWidget(scroll);
    const auto edits=append_port_label_editors(rows);
    append_dialog_buttons(dialog,content);
    if(dialog.exec()==QDialog::Accepted && guard)guard->apply_port_labels(edits);
}
QWidget* AudioNodeModel::numeric_inspector(QWidget* parent) {
    auto* container=new QWidget(parent);auto* layout=new QVBoxLayout(container);layout->setContentsMargins(0,0,0,0);
    auto* contracts=ui::make<QPushButton>("Edit port contracts…",container);contracts->setObjectName("editNodeContracts");layout->addWidget(contracts);
    auto* labels=ui::make<QPushButton>("Edit port labels…",container);labels->setObjectName("editPortLabels");layout->addWidget(labels);
    const QPointer<AudioNodeModel> guard(this);
    connect(contracts,&QPushButton::clicked,container,[guard,container]{if(guard)guard->edit_port_contracts(container);});
    connect(labels,&QPushButton::clicked,container,[guard,container]{if(guard)guard->edit_port_labels(container);});
    layout->addWidget(numeric_parameter_widget(container));return container;
}
QWidget* AudioNodeModel::parameter_widget(QWidget* parent) {
    if(!_numeric_load_error.isEmpty()){auto* error=new QLabel(_numeric_load_error,parent);error->setWordWrap(true);return error;}
    if(_numeric)return numeric_inspector(parent);
    auto* error=ui::make<QLabel>("Unsupported numeric draft version",parent);error->setWordWrap(true);return error;
}
QVBoxLayout* AudioNodeModel::create_embedded_body() {
    _widget = new QWidget;
    _widget->setObjectName("pipelineNodeBody");
    if(_kind=="Fs")ui::bind(_widget,"toolTip","Sampling frequency, Hz");
    update_widget_style(_widget);
    // Equal embedded-body width; QtNodes still adds the actual port/caption
    // bounds independently, so semantic port labels remain fully visible.
    _widget->setFixedWidth(240);
    auto *layout = new QVBoxLayout(_widget);
    layout->setContentsMargins(8,8,8,8);
    layout->setSpacing(6);
    return layout;
}
void AudioNodeModel::add_embedded_summary(QVBoxLayout* layout) {
    auto* summary=new QLabel(_widget);summary->setObjectName("pipelineOperationSummary");summary->setWordWrap(true);summary->setText(operation_summary());layout->addWidget(summary);
    const QPointer<AudioNodeModel> modelGuard(this);const QPointer<QLabel> summaryGuard(summary);
    auto refreshSummary=[modelGuard,summaryGuard]{if(!modelGuard || !summaryGuard)return;const auto text=modelGuard->operation_summary();if(summaryGuard->text()!=text)summaryGuard->setText(text);};
    connect(this,&AudioNodeModel::parameter_values_changed,summary,refreshSummary);connect(this,&AudioNodeModel::parameter_structure_changed,summary,refreshSummary);connect(this,&AudioNodeModel::presentation_changed,summary,refreshSummary);connect(&ui::Language::instance(),&ui::Language::changed,summary,refreshSummary);
}
void AudioNodeModel::initialize_embedded_activity() {
    _processing_activity=new QLabel(_widget);_processing_activity->setObjectName("pipelineNodeActivity");
    _processing_activity->setTextFormat(Qt::PlainText);_processing_activity->hide();
    set_processing_activity(_activity_measured,_activity_active,_activity_hz);
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{set_processing_activity(_activity_measured,_activity_active,_activity_hz);});
 }
QWidget* AudioNodeModel::embeddedWidget() {
    if(_kind=="Reroute" || _kind=="ArrayLength" || (numeric_editor::is_graph_view(_kind) && !scalar_view_kind(_kind)))return nullptr;
    if(_widget)return _widget;
    auto* layout=create_embedded_body();initialize_embedded_activity();add_embedded_summary(layout);
    layout->addWidget(_numeric?numeric_parameter_widget(_widget,true):parameter_widget(_widget));return _widget;
}
bool AudioNodeModel::publish_preview(DiagnosticPreview value) {
    auto existing=std::find_if(_diagnostics.begin(),_diagnostics.end(),[&](const auto& p){return p.direction==value.direction && p.port==value.port;});
    if(existing!=_diagnostics.end()) {
        if(pipeline::delivery_precedes({value.revision,value.epoch},{existing->revision,existing->epoch}))return false;
        if(value.sequence_seen && existing->sequence_seen && value.epoch==existing->epoch && value.revision==existing->revision &&
           std::tie(value.producer_epoch,value.generation,value.sequence)<=std::tie(existing->producer_epoch,existing->generation,existing->sequence))return false;
        if(!value.sequence_seen && !value.sequence_reset && value.epoch==existing->epoch && value.revision==existing->revision) {
            value.producer_epoch=existing->producer_epoch;value.generation=existing->generation;value.sequence=existing->sequence;value.sequence_seen=existing->sequence_seen;
        }
        *existing=std::move(value);
    } else _diagnostics.push_back(std::move(value));
    update_previews();return true;
}
namespace {
AudioNodeModel::DiagnosticPreview preview_description(const pipeline::DeliveredNumericFrame& delivery,const domain::ProcessingPort& port) {
    AudioNodeModel::DiagnosticPreview result;
    result.direction=delivery.context->endpoint.direction;result.port=delivery.context->endpoint.port;
    result.epoch=delivery.delivery.epoch;result.revision=delivery.delivery.graph_revision;
    result.description=numeric_editor::port_caption(port);return result;
}
void preview_stamp(AudioNodeModel::DiagnosticPreview& result,const signal_contracts::FrameStamp& stamp,std::uint64_t sequence) {
    result.producer_epoch=stamp.producer_epoch;result.generation=stamp.generation;result.sequence=sequence;result.sequence_seen=true;
}
QString preview_count_source(signal_contracts::Axis axis) {
    switch(axis){case signal_contracts::Axis::Sample:return "Samples";case signal_contracts::Axis::Bin:return "Bins";case signal_contracts::Axis::Band:return "Bands";default:return "Values";}
}
bool finite_literal(const signal_contracts::NumericLiteral& literal) {
    return std::visit([](auto value){if constexpr(std::is_floating_point_v<decltype(value)>)return std::isfinite(value);else return true;},literal);
}
void set_preview_count_source(const signal_contracts::NumericView& numeric,
                              const signal_contracts::NumericLiteral& first,
                              AudioNodeModel::DiagnosticPreview& result) {
    result.count_source=preview_count_source(numeric.contract->shape.axes[0]);
    if(numeric.contract->quantity==signal_contracts::Quantity::FilledFrameCount &&
       result.count==1 && std::holds_alternative<std::uint64_t>(first)) {
        result.count_source="Frames";
        result.count=std::get<std::uint64_t>(first);
    }
}
bool read_numeric_preview(const signal_contracts::NumericView& numeric,const signal_contracts::DataContract& declared,AudioNodeModel::DiagnosticPreview& result) {
    using namespace signal_contracts;
    if(!numeric.contract || !same_physical_contract(declared,DataContract{*numeric.contract}) || numeric.ring.capacity)return false;
    result.count=std::uint64_t(numeric.actual[0])*numeric.actual[1]*numeric.planes.size();
    NumericLiteral first;
    if(result.count && pipeline::read_numeric_literal(numeric,{0,0,0},first)!=Status::ok)return false;
    if(result.count && !finite_literal(first))return false;
    if(result.count)result.exact_value=QString::fromStdString(pipeline::exact_scalar_text(first));
    set_preview_count_source(numeric,first,result);
    return true;
}
bool valid_history_row(const signal_contracts::NumericContract& history,const signal_contracts::DataFrameView& frame) {
    using namespace signal_contracts;
    const auto* numeric=std::get_if<NumericView>(&frame.payload);
    auto expected=history;expected.semantic=history.history_element;expected.history_element=Semantic::Numeric;
    if(history.shape.rank==Rank::Vector){
        expected.shape={};
        expected.coordinates={};
    }else if(history.shape.rank==Rank::Matrix){
        expected.shape={Rank::Vector,{history.shape.axes[1],Axis::None},{history.shape.extents[1],ExtentBounds{}}};
        expected.coordinates={history.coordinates[1],AxisCoordinates{}};
    }else return false;
    return numeric && numeric->contract && same_physical_contract(expected,*numeric->contract) &&
        frame.stamp.available && validate_view(frame,numeric_editor::limits().frame)==Status::ok;
}
void read_history_preview(const pipeline::DeliveredNumericHistoryRow& row,const signal_contracts::NumericContract& history,AudioNodeModel::DiagnosticPreview& result) {
    if(row.compact.status!=signal_contracts::Status::ok || !row.compact.frame)return;
    const auto frame=row.compact.frame.view();
    if(!valid_history_row(history,frame))return;
    preview_stamp(result,frame.stamp,row.accepted_row_ordinal);
    result.available=true;result.count=row.filled_count;
}
bool read_history_event(const pipeline::DeliveredNumericHistoryRow& row,
                        const signal_contracts::NumericContract& history,
                        AudioNodeModel::DiagnosticPreview& result) {
    if(row.event==pipeline::NumericHistoryEvent::Row)read_history_preview(row,history,result);
    else if(row.event==pipeline::NumericHistoryEvent::Empty ||
            row.event==pipeline::NumericHistoryEvent::Reset) {
        if(row.filled_count!=0)return false;
        result.available=true;result.sequence_reset=true;
    }
    return true;
}
}
const domain::ProcessingPort* AudioNodeModel::preview_port(const pipeline::DeliveredNumericFrame& delivery) const {
    if(!_numeric || !delivery.context || delivery.context->endpoint.node!=_numeric->id)return nullptr;
    const auto& endpoint=delivery.context->endpoint;
    domain::ResolvedProcessingNode resolved;
    if(!domain::resolve_processing_node(*_numeric,numeric_editor::limits(),resolved) || (endpoint.direction==domain::PortDirection::Input && resolved.role!=domain::NodeRole::Sink))return nullptr;
    const auto& ports=endpoint.direction==domain::PortDirection::Output?_numeric->outputs:_numeric->inputs;
    const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& candidate){return candidate.id==endpoint.port && candidate.direction==endpoint.direction;});
    if(port==ports.end() || !port->contract || !delivery.context->contract || !signal_contracts::same_contract(port->contract.view(),delivery.context->contract.view()))return nullptr;
    return &*port;
}
QString AudioNodeModel::preview_failure(signal_contracts::Status status) const {
    if(status==signal_contracts::Status::overflow)return "Range overflow";
    if(status!=signal_contracts::Status::invalid_argument)return {};
    if(_kind=="NoteSpectrum" || _kind=="BroadbandRow" || _kind=="MusicalSpectrogram")return "Input spectrum must contain finite nonnegative values. Check correction coefficients and array operation.";
    return "Precision or value rejected";
}
bool AudioNodeModel::read_preview_value(const signal_contracts::DataFrameView& frame,const signal_contracts::DataContract& declared,DiagnosticPreview& result) const {
    if(const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload))return read_numeric_preview(*numeric,declared,result);
    const auto& structured=std::get<signal_contracts::StructuredView>(frame.payload);
    if(!structured.contract || !signal_contracts::same_contract(declared,signal_contracts::DataContract{*structured.contract}))return false;
    result.count=structured.bytes.size();result.count_source="Bytes";return true;
}
bool AudioNodeModel::preview(const pipeline::DeliveredNumericFrame& delivery) {
    using namespace signal_contracts;
    const auto* port=preview_port(delivery);if(!port)return false;
    auto result=preview_description(delivery,*port);result.failure=preview_failure(delivery.status);
    if(delivery.status!=Status::ok || !delivery.frame)return publish_preview(std::move(result));
    const auto frame=delivery.frame.view();preview_stamp(result,frame.stamp,frame.stamp.sequence);
    result.timestamp_ticks=frame.stamp.timestamp_ticks;
    if(!frame.stamp.available || validate_view(frame,numeric_editor::limits().frame)!=Status::ok)return publish_preview(std::move(result));
    result.available=read_preview_value(frame,port->contract.view(),result);
    return publish_preview(std::move(result));
}
bool AudioNodeModel::preview(const pipeline::DeliveredNumericHistoryRow& row) {
    using namespace signal_contracts;
    const auto& delivery=row.compact;
    const auto* port=preview_port(delivery);if(!port)return false;
    const auto contract=port->contract.view();const auto* history=std::get_if<NumericContract>(&contract);
    if(!history || history->semantic!=Semantic::History ||
       (history->shape.rank!=Rank::Matrix && history->shape.rank!=Rank::Vector) ||
       row.capacity!=history->shape.extents[0].maximum || row.filled_count>row.capacity)return false;
    auto result=preview_description(delivery,*port);
    result.count_source=history->shape.rank==Rank::Vector?"Values":"Frames";
    if(!read_history_event(row,*history,result))return false;
    return publish_preview(std::move(result));
}
namespace {
bool valid_history_snapshot(const signal_contracts::DataFrameView& frame,
                            const signal_contracts::NumericContract& history,
                            std::uint64_t ordinal,std::uint64_t filled_count) {
    const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload);
    if(!numeric || !numeric->contract ||
       !signal_contracts::same_physical_contract(history,*numeric->contract) ||
       signal_contracts::validate_view(frame,numeric_editor::limits().frame)!=signal_contracts::Status::ok)
        return false;
    return numeric->actual[0]==history.shape.extents[0].maximum &&
        filled_count<=numeric->actual[0] && ordinal>=filled_count;
}
}
bool AudioNodeModel::preview_history_snapshot(const pipeline::DeliveredNumericFrame& delivery,std::uint64_t ordinal,std::uint64_t filled_count) {
    using namespace signal_contracts;
    const auto* port=preview_port(delivery);if(!port)return false;
    const auto declared=port->contract.view();const auto* history=std::get_if<NumericContract>(&declared);
    if(!history || history->semantic!=Semantic::History ||
       (history->shape.rank!=Rank::Matrix && history->shape.rank!=Rank::Vector))return false;
    auto result=preview_description(delivery,*port);
    result.count_source=history->shape.rank==Rank::Vector?"Values":"Frames";
    if(delivery.status!=Status::ok || !delivery.frame)return publish_preview(std::move(result));
    const auto frame=delivery.frame.view();
    if(!valid_history_snapshot(frame,*history,ordinal,filled_count))return false;
    if(!frame.stamp.available)return publish_preview(std::move(result));
    result.available=true;result.count=filled_count;
    preview_stamp(result,frame.stamp,ordinal);
    return publish_preview(std::move(result));
}
void AudioNodeModel::set_preview_activity(const QString& status_key) {if(_preview_activity==status_key)return;_preview_activity=status_key;if(!status_key.isEmpty())_diagnostics.clear();update_previews();}
void AudioNodeModel::set_processing_activity(bool measured,bool active,double calls_hz) {
    const bool changed=_activity_measured!=measured || _activity_active!=active;
    _activity_measured=measured;_activity_active=active;_activity_hz=std::isfinite(calls_hz)?std::max(0.,calls_hz):0.;
    if(changed)emit activity_changed();
    if(!_processing_activity)return;
    refresh_processing_activity_label(measured,active);
}
void AudioNodeModel::refresh_processing_activity_label(bool measured,bool active) {
    const auto text=ui::text(!measured?"Activity: awaiting measurement":active?"Activity: processing":"Activity: no processing in interval");
    // A fixed status changes only when activity changes, never for every frame.
    if(_processing_activity->text()!=text)_processing_activity->setText(text);
    const auto tip=ui::text("Measured node executions: %1 Hz").arg(measured?QString::number(_activity_hz,'f',1):QStringLiteral("—"));
    if(_processing_activity->toolTip()!=tip)_processing_activity->setToolTip(tip);
    if(_widget)_widget->setToolTip(_kind=="Fs"?ui::text("Sampling frequency, Hz"):text+QStringLiteral("\n")+tip);
}
void AudioNodeModel::clear_preview() { _diagnostics.clear();update_previews(); }
QWidget *AudioNodeModel::preview_widget(QWidget *parent,bool compact) {
    auto *card=new NodePreviewCard(parent);
    card->set_compact(compact);
    _previews.removeIf([](const auto &widget) { return widget.isNull(); });
    _previews.append(card);
    update_previews();
    return card;
}
AudioNodeModel::DiagnosticPreview AudioNodeModel::preview_for_port(const domain::ProcessingPort& port) const {
    auto found=std::find_if(_diagnostics.begin(),_diagnostics.end(),[&](const auto& p){
        return p.direction==port.direction && p.port==port.id;
    });
    DiagnosticPreview value;
    if(found!=_diagnostics.end())value=*found;
    value.direction=port.direction;value.port=port.id;
    value.description=numeric_editor::port_caption(port);
    if(port.contract) {
        const auto contract=port.contract.view();
        const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract);
        if(numeric && numeric->channel_order.size()>1 && !value.exact_value.isEmpty())
            value.description+=QStringLiteral(" · ")+ui::text("Channel %1").arg(numeric->channel_order[0]);
    }
    return value;
}
void AudioNodeModel::update_previews() {
    update_diagnostic_error();
    std::vector<DiagnosticPreview> values;
    domain::ResolvedProcessingNode resolved;
    if(_numeric && domain::resolve_processing_node(*_numeric,numeric_editor::limits(),resolved))
        for(const auto& port:resolved.role==domain::NodeRole::Sink?_numeric->inputs:_numeric->outputs)
            values.push_back(preview_for_port(port));
    for(auto card:std::as_const(_previews))
        if(card) card->set_data(values,_numeric && _numeric->enabled,_preview_activity);
}
void AudioNodeModel::update_diagnostic_error() {
    QString errors;for(const auto& p:_diagnostics)errors+=p.failure;
    if(errors!=_diagnostic_error_key){_diagnostic_error_key=errors;emit diagnostics_changed();}
}
QString AudioNodeModel::diagnostic_failure(domain::PortDirection direction,domain::PortId port) const {
    for(const auto& p:_diagnostics)if(p.direction==direction && p.port==port)return p.failure.isEmpty()?QString{}:ui::text(p.failure.toUtf8().constData());
    return {};
}
unsigned AudioNodeModel::grouped_channels(QtNodes::PortType side) const {
    if(!_numeric)return 0;
    if(_kind=="InputSelector" && side==QtNodes::PortType::In)return unsigned(domain::data_outputs(*_numeric).size());
    if(_kind=="Switch" && side==QtNodes::PortType::Out)return std::get<domain::RouteProcessingParameters>(_numeric->parameters).channels_per_stream;
    return 0;
}
QString AudioNodeModel::stream_name(unsigned group) const {
    const auto names=_presentation.value("stream_names").toArray();const auto name=group<unsigned(names.size())?names[int(group)].toString().trimmed():QString{};
    return name.isEmpty()?ui::text("Stream %1").arg(group+1):name;
}
namespace {
QString selected_choice_text(const NodeParameterModel::Field& field,
                             const NodeParameterModel::Value& value) {
    const auto option=std::find_if(field.options.begin(),field.options.end(),
        [&](const auto& item){return item.value==value;});
    if(option==field.options.end())return {};
    auto text=option->translate?ui::text(option->label.toUtf8().constData()):option->label;
    for(const auto& argument:option->label_arguments)text=text.arg(argument);
    return text;
}
QString selected_field_text(const NodeParameterModel::Field& field) {
    const auto value=field.read();
    if(const auto* literal=std::get_if<signal_contracts::NumericLiteral>(&value))return numeric_editor::literal_text(*literal);
    const auto* integer=std::get_if<int>(&value);if(!integer)return {};
    if(field.editor!=NodeParameterModel::Editor::Choice)return QString::number(*integer);
    return selected_choice_text(field,value);
}
QString port_sample_rate(const std::vector<domain::ProcessingPort>& ports) {
    if(ports.empty() || !ports[0].contract)return QStringLiteral("?");
    const auto contract=ports[0].contract.view();const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract);
    return numeric && numeric->clock.sample_rate?QString::number(numeric->clock.sample_rate):QStringLiteral("?");
}
}
QString AudioNodeModel::field_summary(const QString& key,bool caption) const {
    const auto* field=_parameters->field(key);if(!field || !field->read)return {};
    const auto text=selected_field_text(*field);if(text.isEmpty())return {};
    return caption?ui::text(field->label.toUtf8().constData())+QStringLiteral(": ")+text:text;
}
QString AudioNodeModel::routing_summary() const {
    if(_kind=="InputSelector")return ui::text("Selected stream: %1").arg(stream_name(unsigned(selected_input())))+QStringLiteral(" · ")+ui::text("Values per stream: %1").arg(domain::data_outputs(*_numeric).size());
    if(_kind=="Switch")return ui::text("Selected stream: %1").arg(stream_name(std::get<domain::RouteProcessingParameters>(_numeric->parameters).selected))+QStringLiteral(" · ")+ui::text("Channels: %1").arg(domain::data_inputs(*_numeric).size());
    if(_kind=="Mixer")return ui::text("Inputs: %1").arg(domain::data_inputs(*_numeric).size())+QStringLiteral(" · ")+ui::text(std::get<domain::MixerProcessingParameters>(_numeric->parameters).mode==domain::MixerMode::WeightedMean?"Normalized proportions":"Mixing operation");
    return {};
}
QString AudioNodeModel::processing_summary() const {
    if(_kind=="PcmResampler")return ui::text("Sample rate, Hz")+QStringLiteral(": ")+port_sample_rate(_numeric->inputs)+QStringLiteral(" → ")+port_sample_rate(_numeric->outputs);
    if(_kind=="WindowFunction" || _kind=="MatrixWindowFunction")return field_summary("function",false);
    if(_kind=="MatrixCenter")return field_summary("method",false);
    if(_kind=="Spectrogram" || _kind=="NumberAccumulator")
        return ui::text(_kind=="NumberAccumulator"?"History capacity, values":"History capacity, frames")+QStringLiteral(": ")+
            QString::number(std::get<domain::HistoryProcessingParameters>(_numeric->parameters).capacity);
    if(_kind=="MatrixFFT")return field_summary("fft_size",true);
    if(_kind=="MatrixRow")return field_summary("row",true);
    return {};
}
QString AudioNodeModel::operation_summary() const {
    if(!_numeric)return {};
    if(_kind=="AudioSource")return ui::text(numeric_editor::capture_source_kind(*_numeric)==0?"Loopback":"Microphone")+QStringLiteral(" · ")+ui::text("Saved outputs: %1").arg(domain::data_outputs(*_numeric).size());
    if(_kind=="SignalGenerator")return ui::text("Oscillators: %1").arg(std::get<domain::SignalGeneratorProcessingParameters>(_numeric->parameters).oscillators.size());
    const auto route=routing_summary();return route.isEmpty()?processing_summary():route;
}
namespace {
QString static_node_caption(const QString& kind) {
    static const QMap<QString,const char*> names{
        {"AudioSource","Audio source"},
        {"Window","Audio window buffer"},
        {"WindowFunction","Window function"},
        {"WindowView","Audio vector graph"},
        {"WindowedView","Audio vector graph"},
        {"WindowFunctionView","Audio vector graph"},
        {"TestSignal","Test signal"},
        {"SignalGenerator","Signal generator"},
        {"SampleClock","Synchronization"},
        {"Mixer","Mixer"},
        {"StereoFFT","FFT"},
        {"Reroute","Routing point"},
        {"Fs","Fs"},
        {"UInt32Constant","Integer value"},
        {"ArrayLength","Array size"},
        {"SampleRate","Signal sample rate"},
        {"SpectrumMerge","Spectrum merge"},
        {"SpectrumGain","Spectrum scaling"},
        {"BarkWeighting","Bark weighting"},
        {"Mel","Mel"},
        {"Spectrogram","Array accumulator"},
        {"NumberAccumulator","Number accumulator"},
        {"FFTView","Audio vector graph"},
        {"MelView","Audio vector graph"},
        {"AudioView","Audio vector graph"},
        {"SpectrogramView","Audio matrix graph"},
        {"WLED","WLED"},
        {"Splitter","Splitter"},
        {"Switch","Output switch"},
        {"Gate","Gate"},
        {"ScalarReduce","Signal measurement"},
        {"ScalarView","Value graph"},
        {"ScalarMetricsView","Scalar values"},
        {"ScalarSignedBarView","Signed scalar bars"},
        {"VectorLineView","Line graph"},
        {"VectorPointView","Point graph"},
        {"VectorBarView","Bar graph"},
        {"VectorPieView","Pie graph"},
        {"MatrixImageView","Matrix image"},
        {"MatrixSurfaceView","Matrix surface"},
        {"AudioLineView","Audio line graph"},
        {"AudioPointView","Audio point graph"},
        {"AudioBarView","Audio bar graph"},
        {"AudioPieView","Audio pie graph"},
        {"AudioImageView","2D spectrogram"},
        {"AudioSurfaceView","3D spectrogram"},
        {"WLEDEncode","WLED packet encoder"},
        {"WLEDOutput","WLED network output"},
        {"ChannelComparison","Channel comparison"},
        {"MatrixTranspose","Matrix transpose"},
        {"MatrixCenter","Matrix row centering"},
        {"MatrixWindowFunction","Matrix window function"},
        {"MatrixFFT","Matrix row FFT"},
        {"MatrixRow","Matrix row selector"},
        {"IndexExtract","Index extractor"},
        {"PcmResampler","PCM resampler"},
        {"RFFT","Real FFT (complex)"},
        {"Fourier","Fourier (complex input)"},
        {"RealFourier","Real Fourier"},
        {"ArrayFill","Array coefficients"},
        {"AmplitudePhase","Spectrum amplitude and phase"},
        {"WindowGain","Window coefficient sum"},
        {"BroadbandRow","Broadband row"},
        {"PianoView","Piano: note activity"},
        {"NoteSpectrum","Note spectrum"},
        {"ArrayConcat","Join arrays"},
        {"FrequencyStep","Frequency step"},
        {"Number","Number"},
        {"ComplexPolar","Complex to polar"},
        {"SpectrumAmplitude","Spectral amplitude"},
        {"SpectrumPeak","Spectrum peak"},
        {"SpectrumProjection","Spectral projection"},
        {"BroadbandFeatures","Broadband features"},
        {"NoteSalience","Note salience (experimental)"},
        {"MusicalSpectrogram","Musical spectrogram row"},
        {"SpectralQuantize","Feature quantization"},
        {"NetworkInput","BDSP input (TCP)"},
        {"NetworkOutput","BDSP socket output"},
        {"InputSelector","Input switch"},
        {"Converter","Numeric converter"}};
    return names.contains(kind)?ui::text(names.value(kind)):kind;
}
bool array_addition(const domain::ConfiguredProcessingNode& node) {
    const auto* parameters=std::get_if<domain::TransformProcessingParameters>(&node.parameters);if(!parameters)return false;
    for(const auto& field:parameters->values)if(field.key=="operation")if(const auto* value=std::get_if<std::string>(&field.value))return *value=="add";
    return false;
}
QString normalizer_caption(const domain::ConfiguredProcessingNode* node) {
    if(node && !node->inputs.empty()){
        const auto data=node->inputs[0].contract.view();const auto* numeric=std::get_if<signal_contracts::NumericContract>(&data);
        if(numeric && numeric->quantity==signal_contracts::Quantity::SpectrumPeakAmplitude)return ui::text("Amplitude normalizer");
    }
    return ui::text("Frequency profile normalizer");
}
}
QString AudioNodeModel::caption() const {
    const auto custom=_presentation.value("display_name").toString().trimmed();
    if(_kind=="AudioSource" && _numeric)return (custom.isEmpty()?ui::text("Audio source"):custom)+QStringLiteral(" · ")+ui::text(numeric_editor::capture_source_kind(*_numeric)==0?"Loopback":"Microphone");
    if(!custom.isEmpty())return custom;
    if(_kind=="ArrayMultiply")return ui::text(_numeric && array_addition(*_numeric)?"Elementwise addition":"Elementwise multiplication");
    if(_kind=="FrequencyNormalizer")return normalizer_caption(numeric_node());
    return static_node_caption(_kind);
}
