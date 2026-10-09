#include "visualization/ui_text.h"
#include "core/performance.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/node_preview_card.h"
#include "core_app/scheme_editor/embedded_widget_layout.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/node_parameter_form.h"
#include "core_app/wheel_input.h"
#include <desktop/numeric_project_document.h>
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
#include <QToolButton>
#include <QMenu>
#include <cmath>
#include <algorithm>
AudioNodeModel::AudioNodeModel(QString kind) : _kind(std::move(kind)) {
    update_node_style();
    connect(&ui::Theme::instance(), &ui::Theme::changed, this, [this] { update_node_style(); });
    _numeric = numeric_editor::default_node(_kind);
    _parameters=new NodeParameterModel(this);
    _parameters->setEditHandlers([this](const auto& n){return edit_numeric(n);},[this](const auto& a){return edit_adapter(a);},[this](const auto& key,bool shown){return set_parameter_on_block(key,shown);});
    _parameters->setNode(*_numeric,true);
    connect(this,&AudioNodeModel::parameter_values_changed,_parameters,[this]{if(_numeric)_parameters->setNode(*_numeric,false);});
    connect(this,&AudioNodeModel::parameter_structure_changed,_parameters,[this]{if(_numeric)_parameters->setNode(*_numeric,true);});
    connect(this,&AudioNodeModel::adapter_changed,_parameters,[this]{_parameters->setAdapter(_adapter);});
    connect(this,&AudioNodeModel::presentation_changed,_parameters,[this]{_parameters->setPresentation(_presentation);});
    connect(this,&AudioNodeModel::capture_devices_changed,_parameters,[this]{_parameters->setCaptureDeviceProvider(_capture_device_provider);});

    connect(&ui::Language::instance(), &ui::Language::changed, this, [this] { emit parameter_values_changed();update_previews();emit requestNodeUpdate(); });
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
    if(object==_widget && event->type()==QEvent::LayoutRequest && !_widget->property("nodeSizePending").toBool()){
        _widget->setProperty("nodeSizePending",true);
        QTimer::singleShot(0,this,[this]{if(!_widget)return;_widget->setProperty("nodeSizePending",false);
            auto* layout=_widget->layout();if(!layout)return;
            const auto height=embeddedContentHeight(_widget);
            const int preferred=_widget->property("nodePreferredWidth").isValid()?_widget->property("nodePreferredWidth").toInt():240;
            if(height!=_widget->height() || embeddedContentWidth(_widget,preferred)!=_widget->width())emit embeddedWidgetSizeUpdated();
        });
    }
    return QtNodes::NodeDelegateModel::eventFilter(object,event);
}
unsigned int AudioNodeModel::nPorts(QtNodes::PortType type) const {
    if(!_numeric_load_error.isEmpty())return 0;
    if(_numeric)return unsigned((type==QtNodes::PortType::In?_numeric->inputs:_numeric->outputs).size());
    return 0;
}
QtNodes::NodeDataType AudioNodeModel::dataType(QtNodes::PortType type, QtNodes::PortIndex port) const {
    if(port>=nPorts(type))return {};
    if(_numeric){const auto& ports=type==QtNodes::PortType::In?_numeric->inputs:_numeric->outputs;return {"numeric-contract",numeric_editor::port_caption(ports[port],!numeric_editor::is_graph_view(_kind))};}
    return {};
}
QString AudioNodeModel::portCaption(QtNodes::PortType type, QtNodes::PortIndex port) const {
    if(_kind=="InputSelector" && type==QtNodes::PortType::In)return ui::text("Input %1").arg(unsigned(port_id(type,port))+1);
    auto label=dataType(type,port).name;
    return nPorts(type)>1?QStringLiteral("%1 · %2").arg(port).arg(label):label;
}
QJsonObject AudioNodeModel::save() const {
    if(!_numeric_load_error.isEmpty())return {{"model-name",_kind},{"numeric_processing",_rejected_numeric_snapshot}};
    if(_numeric){QByteArray bytes;const auto result=desktop::encode_numeric_node(*_numeric,bytes);return {{"model-name",_kind},{"numeric_processing",QString::fromUtf8(bytes)},{"numeric_error",result.error},{"presentation",_presentation},{"adapter",_adapter}};}
    return {{"model-name",_kind}};
}
void AudioNodeModel::load(const QJsonObject &json) {
    if((json.contains("presentation") && !json.value("presentation").isObject()) || !valid_presentation(json.value("presentation").toObject())){
        _numeric.reset();_numeric_load_error=ui::text("Invalid node presentation");_rejected_numeric_snapshot=json.value("numeric_processing").toString();clear_preview();return;
    }
    set_presentation(json.value("presentation").toObject());
    if((json.contains("adapter") && !json.value("adapter").isObject()) || !set_adapter(json.value("adapter").toObject())){
        _numeric.reset();_numeric_load_error=ui::text("Invalid network source settings");_rejected_numeric_snapshot=json.value("numeric_processing").toString();clear_preview();return;
    }
    if(json.contains("numeric_processing")){
        domain::ConfiguredProcessingNode node;
        const auto result=desktop::decode_numeric_node(json.value("numeric_processing").toString().toUtf8(),node);
        if(result && node.kind==_kind.toStdString())set_numeric_node(node);
        else {_numeric.reset();_numeric_load_error=result?ui::text("Invalid node kind"):result.error;_rejected_numeric_snapshot=json.value("numeric_processing").toString();clear_preview();}
        return;
    }
    _numeric.reset();
    _diagnostics.clear();
    _numeric_load_error=ui::text("Unsupported numeric draft version");
    _rejected_numeric_snapshot.clear();
    update_previews();
}
void AudioNodeModel::set_numeric_node(const domain::ConfiguredProcessingNode& value) {
    if(value.kind!=_kind.toStdString())return;
    auto candidate=value;
    for(auto* ports:{&candidate.inputs,&candidate.outputs})std::sort(ports->begin(),ports->end(),[](const auto& a,const auto& b){return a.id<b.id;});
    if(_numeric)for(auto direction:{QtNodes::PortType::In,QtNodes::PortType::Out}) {
        const auto& before=direction==QtNodes::PortType::In?_numeric->inputs:_numeric->outputs;
        const auto& after=direction==QtNodes::PortType::In?candidate.inputs:candidate.outputs;
        for(std::size_t i=0;i<before.size();++i)if(i>=after.size() || !before[i].contract || !after[i].contract || !signal_contracts::same_contract(before[i].contract.view(),after[i].contract.view()))emit portsAboutToBeDeleted(direction,unsigned(i),unsigned(i));
        if(after.size()>before.size())emit portsAboutToBeInserted(direction,unsigned(before.size()),unsigned(after.size()-1));
    }
    const auto prior=_numeric;
    auto same_structure=[](const auto& a,const auto& b){if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(a[i].id!=b[i].id || a[i].contract.view().index()!=b[i].contract.view().index())return false;return true;};
    const bool structure_changed=prior && (!same_structure(prior->inputs,candidate.inputs) || !same_structure(prior->outputs,candidate.outputs));
    _numeric_load_error.clear();_rejected_numeric_snapshot.clear();
    _numeric=std::move(candidate);
    if(prior){emit portsDeleted();if(_numeric->inputs.size()>prior->inputs.size() || _numeric->outputs.size()>prior->outputs.size())emit portsInserted();}
    if(structure_changed)emit parameter_structure_changed();else emit parameter_values_changed();
    _diagnostics.clear();
    update_previews();
    emit requestNodeUpdate();
    if(prior && prior->enabled!=_numeric->enabled && (_kind=="AudioSource" || _kind=="TestSignal"))emit local_source_output_changed(_numeric->enabled);
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
bool AudioNodeModel::valid_presentation(const QJsonObject& value) {
    if(value.contains("display_name")){
        if(!value.value("display_name").isString())return false;
        const auto name=value.value("display_name").toString();
        if(name.size()>256 || std::any_of(name.begin(),name.end(),[](QChar c){return c.category()==QChar::Other_Control;}))return false;
    }
    if(value.contains("scalar_inputs")){
        if(!value.value("scalar_inputs").isObject())return false;
        const auto inputs=value.value("scalar_inputs").toObject();if(inputs.size()>8)return false;
        for(auto it=inputs.begin();it!=inputs.end();++it){
            bool valid=false;const auto id=it.key().toUInt(&valid);if(!valid || id>65535 || QString::number(id)!=it.key() || !it.value().isObject())return false;
            const auto input=it.value().toObject();
            if(input.contains("enabled") && !input.value("enabled").isBool())return false;
            if(input.contains("display_name") && (!input.value("display_name").isString() || !valid_presentation(QJsonObject{{"display_name",input.value("display_name")}})))return false;
        }
    }
    if(value.contains("inline_parameters")) {
        if(!value.value("inline_parameters").isArray())return false;
        const auto keys=value.value("inline_parameters").toArray();QSet<QString> seen;
        if(keys.size()>128)return false;
        for(const auto key:keys){if(!key.isString() || key.toString().isEmpty() || key.toString().toUtf8().size()>1024 || seen.contains(key.toString()))return false;seen.insert(key.toString());}
    }
    return true;
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
    _presentation=value;emit presentation_changed();
    if(_widget){_widget->layout()->activate();_widget->adjustSize();}
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
    for(unsigned i=0;i<ports.size();++i)if(ports[i].id==id)return i;
    return QtNodes::InvalidPortIndex;
}
domain::PortId AudioNodeModel::selected_input() const {
    if(_numeric && _kind=="InputSelector")for(const auto& p:std::get<domain::TransformProcessingParameters>(_numeric->parameters).values)
        if(p.key=="selected_input")return std::visit([](auto v){return domain::PortId(v);},std::get<signal_contracts::NumericLiteral>(p.value));
    return 0;
}
bool AudioNodeModel::select_input(domain::PortId id) {
    if(!_numeric || _kind!="InputSelector" || port_index(QtNodes::PortType::In,id)==QtNodes::InvalidPortIndex)return false;
    auto n=*_numeric;for(auto& p:std::get<domain::TransformProcessingParameters>(n.parameters).values)
        if(p.key=="selected_input")p.value=signal_contracts::NumericLiteral{std::int64_t(id)};
    return edit_numeric(std::move(n));
}
void AudioNodeModel::populate_selector_menu(QMenu* menu) {
    if(!_numeric || _kind!="InputSelector")return;
    const QPointer<AudioNodeModel> guard(this);
    auto* add=menu->addAction(ui::text("Add input"));add->setEnabled(_numeric->inputs.size()<numeric_editor::limits().max_ports_per_node && _numeric->inputs.back().id<65535);
    connect(add,&QAction::triggered,menu,[guard]{if(guard && guard->_numeric){auto n=*guard->_numeric;if(numeric_editor::add_input(n))guard->edit_numeric(std::move(n));}});
    auto* remove=menu->addMenu(ui::text("Remove input"));remove->setEnabled(_numeric->inputs.size()>1);
    for(const auto& port:_numeric->inputs){auto* action=remove->addAction(ui::text("Input %1").arg(unsigned(port.id)+1));
        connect(action,&QAction::triggered,menu,[guard,id=port.id]{if(guard && guard->_numeric){auto n=*guard->_numeric;if(numeric_editor::remove_input(n,id))guard->edit_numeric(std::move(n));}});}
}
std::vector<QtNodes::PortIndex> AudioNodeModel::visual_port_order(QtNodes::PortType side) const {
    std::vector<QtNodes::PortIndex> order;
    for(const auto value:_presentation.value(side==QtNodes::PortType::In?"input_port_order":"output_port_order").toArray()){
        const int id=value.toInt(-1);const auto index=id>=0 && id<=65535?port_index(side,domain::PortId(id)):QtNodes::InvalidPortIndex;if(index!=QtNodes::InvalidPortIndex && std::find(order.begin(),order.end(),index)==order.end())order.push_back(index);
    }
    for(unsigned id=0;id<nPorts(side);++id)if(std::find(order.begin(),order.end(),id)==order.end())order.push_back(id);
    return order;
}
bool AudioNodeModel::set_parameter_on_block(const QString& key,bool shown) {
    if(key=="numericEnabled" || key=="numericBypass")return false;
    if(key.isEmpty() || key.size()>128 || parameter_on_block(key)==shown)return false;
    auto value=_presentation;auto keys=value.value("inline_parameters").toArray();
    if(shown){if(keys.size()>=128)return false;keys.append(key);}
    else for(int i=keys.size()-1;i>=0;--i)if(keys[i]==key)keys.removeAt(i);
    value["inline_parameters"]=keys;
    if(_presentation_edit)return _presentation_edit(value);
    set_presentation(value);return true;
}
bool AudioNodeModel::add_scalar_input(){
    if(!_numeric || _kind!="ScalarView")return false;
    auto node=*_numeric;if(!numeric_editor::add_input(node))return false;
    if(_scalar_topology_edit)return _scalar_topology_edit(node,_presentation);
    return edit_numeric(std::move(node));
}
bool AudioNodeModel::remove_scalar_input(domain::PortId id){
    if(!_numeric || _kind!="ScalarView")return false;
    auto node=*_numeric;if(!numeric_editor::remove_input(node,id))return false;
    auto presentation=_presentation;auto inputs=presentation.value("scalar_inputs").toObject();inputs.remove(QString::number(id));presentation["scalar_inputs"]=inputs;
    if(_scalar_topology_edit)return _scalar_topology_edit(node,presentation);
    if(!edit_numeric(std::move(node)))return false;set_presentation(presentation);return true;
}
bool AudioNodeModel::edit_scalar_input_name(domain::PortId id,const QString& text){
    if(!_numeric || _kind!="ScalarView" || port_index(QtNodes::PortType::In,id)==QtNodes::InvalidPortIndex)return false;
    auto presentation=_presentation;auto inputs=presentation.value("scalar_inputs").toObject();auto input=inputs.value(QString::number(id)).toObject();input["display_name"]=text.trimmed();inputs[QString::number(id)]=input;presentation["scalar_inputs"]=inputs;
    if(!valid_presentation(presentation))return false;
    if(_presentation_edit)return _presentation_edit(presentation);set_presentation(presentation);return true;
}
bool AudioNodeModel::set_scalar_input_enabled(domain::PortId id,bool enabled){
    if(!_numeric || _kind!="ScalarView" || port_index(QtNodes::PortType::In,id)==QtNodes::InvalidPortIndex)return false;
    auto presentation=_presentation;auto inputs=presentation.value("scalar_inputs").toObject();auto input=inputs.value(QString::number(id)).toObject();input["enabled"]=enabled;inputs[QString::number(id)]=input;presentation["scalar_inputs"]=inputs;
    if(_presentation_edit)return _presentation_edit(presentation);set_presentation(presentation);return true;
}
QWidget* AudioNodeModel::numeric_parameter_widget(QWidget* parent,bool inline_form) {
    auto* form=new NodeParameterForm(_parameters,inline_form,parent);update_widget_style(form);
    connect(_parameters,&NodeParameterModel::structureChanged,form,[this]{
        QTimer::singleShot(0,this,[this]{if(_widget){_widget->layout()->activate();_widget->adjustSize();}emit embeddedWidgetSizeUpdated();});
    });
    if(_kind!="ScalarView")return form;
    auto* container=new QWidget(parent);auto* layout=new QVBoxLayout(container);layout->setContentsMargins(0,0,0,0);layout->addWidget(form);
    layout->addWidget(scalar_inputs_widget(container));return container;
}
QWidget* AudioNodeModel::scalar_inputs_widget(QWidget* parent){
    auto* inputs=new QWidget(parent);inputs->setObjectName("scalarInputsEditor");
    const QPointer<AudioNodeModel> guard(this);
    connect(this,&QObject::destroyed,inputs,[inputs]{inputs->setEnabled(false);});
    auto rebuild=[this,guard,inputs]{
        if(!guard)return;
        for(auto* child:inputs->findChildren<QWidget*>(QString{},Qt::FindDirectChildrenOnly))delete child;delete inputs->layout();
        auto* rows=new QVBoxLayout(inputs);rows->setContentsMargins(0,0,0,0);
        if(!_numeric)return;
        const auto metadata=_presentation.value("scalar_inputs").toObject();
        for(const auto& port:_numeric->inputs){
            auto* row=new QWidget(inputs);auto* rowLayout=new QVBoxLayout(row);rowLayout->setContentsMargins(0,0,0,0);auto* fields=new QHBoxLayout;rowLayout->addLayout(fields);fields->setContentsMargins(0,0,0,0);
            auto* enabled=new QCheckBox(row);enabled->setObjectName(QString("scalarInput%1Enabled").arg(port.id));ui::bind(enabled,"toolTip","Show value");enabled->setChecked(metadata.value(QString::number(port.id)).toObject().value("enabled").toBool(true));fields->addWidget(enabled);
            auto* name=new QLineEdit(metadata.value(QString::number(port.id)).toObject().value("display_name").toString(),row);name->setObjectName(QString("scalarInput%1Name").arg(port.id));name->setMaxLength(256);name->setPlaceholderText(ui::text("Value %1").arg(unsigned(port.id)+1));rowLayout->insertWidget(0,name);
            auto* remove=new QPushButton(ui::text("Remove input"),row);remove->setObjectName(QString("scalarInput%1Remove").arg(port.id));remove->setEnabled(_numeric->inputs.size()>1);fields->addWidget(remove);rows->addWidget(row);
            connect(enabled,&QCheckBox::toggled,row,[guard,id=port.id](bool value){if(guard)guard->set_scalar_input_enabled(id,value);});
            connect(name,&QLineEdit::editingFinished,row,[guard,name,id=port.id]{if(guard)guard->edit_scalar_input_name(id,name->text());});
            connect(remove,&QPushButton::clicked,row,[guard,id=port.id]{if(guard)guard->remove_scalar_input(id);});
        }
        auto* add=new QPushButton(ui::text("Add input"),inputs);add->setObjectName("addScalarInput");add->setEnabled(_numeric->inputs.size()<numeric_editor::limits().max_ports_per_node);rows->addWidget(add);connect(add,&QPushButton::clicked,inputs,[guard]{if(guard)guard->add_scalar_input();});
    };
    rebuild();
    auto schedule=[inputs,rebuild]{if(inputs->property("scalarRebuildPending").toBool())return;inputs->setProperty("scalarRebuildPending",true);QTimer::singleShot(0,inputs,[inputs,rebuild]{inputs->setProperty("scalarRebuildPending",false);rebuild();});};
    connect(this,&AudioNodeModel::parameter_structure_changed,inputs,schedule);connect(this,&AudioNodeModel::presentation_changed,inputs,schedule);connect(&ui::Language::instance(),&ui::Language::changed,inputs,schedule);
    return inputs;
}
void AudioNodeModel::update_widget_style(QWidget *widget) {
    // Native controls inherit the application's palette and Fusion style.
    // Only the custom QtNodes body needs the explicit semantic colors above.
    widget->setAutoFillBackground(true);
    widget->installEventFilter(this);
}
QWidget *AudioNodeModel::parameter_widget(QWidget *parent) {
    if(!_numeric_load_error.isEmpty()){auto* error=new QLabel(_numeric_load_error,parent);error->setWordWrap(true);return error;}
    if(_numeric)return numeric_parameter_widget(parent);
    auto* error=ui::make<QLabel>("Unsupported numeric draft version",parent);error->setWordWrap(true);return error;
}
QWidget *AudioNodeModel::embeddedWidget() {
    if (_widget)
        return _widget;
    _widget = new QWidget;
    _widget->setObjectName("pipelineNodeBody");
    update_widget_style(_widget);
    // Equal embedded-body width; QtNodes still adds the actual port/caption
    // bounds independently, so semantic port labels remain fully visible.
    _widget->setFixedWidth(240);
    auto *layout = new QVBoxLayout(_widget);
    layout->setContentsMargins(8,8,8,8);
    layout->setSpacing(6);
    layout->addWidget(_numeric?numeric_parameter_widget(_widget,true):parameter_widget(_widget));

    return _widget;
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
bool AudioNodeModel::preview(const pipeline::DeliveredNumericFrame& delivery) {
    using namespace signal_contracts;
    if(!_numeric || !delivery.context || delivery.context->endpoint.node!=_numeric->id)return false;
    const auto& endpoint=delivery.context->endpoint;
    domain::ResolvedProcessingNode resolved;
    if(!domain::resolve_processing_node(*_numeric,numeric_editor::limits(),resolved) || (endpoint.direction==domain::PortDirection::Input && resolved.role!=domain::NodeRole::Sink))return false;
    const auto& ports=endpoint.direction==domain::PortDirection::Output?_numeric->outputs:_numeric->inputs;
    auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==endpoint.port && p.direction==endpoint.direction;});
    if(port==ports.end() || !port->contract || !delivery.context->contract || !same_contract(port->contract.view(),delivery.context->contract.view()))return false;
    DiagnosticPreview result;result.direction=endpoint.direction;result.port=endpoint.port;result.epoch=delivery.delivery.epoch;result.revision=delivery.delivery.graph_revision;
    result.description=numeric_editor::port_caption(*port);
    if(delivery.status==Status::overflow)result.failure="Range overflow";
    else if(delivery.status==Status::invalid_argument)result.failure="Precision or value rejected";
    if(delivery.status!=Status::ok || !delivery.frame)return publish_preview(std::move(result));
    const auto frame=delivery.frame.view();
    result.producer_epoch=frame.stamp.producer_epoch;result.generation=frame.stamp.generation;result.sequence=frame.stamp.sequence;result.sequence_seen=true;
    result.timestamp_ticks=frame.stamp.timestamp_ticks;
    if(!frame.stamp.available || validate_view(frame,numeric_editor::limits().frame)!=Status::ok)return publish_preview(std::move(result));
    const auto declared=port->contract.view();
    if(const auto* numeric=std::get_if<NumericView>(&frame.payload)) {
        if(!numeric->contract || !same_physical_contract(declared,DataContract{*numeric->contract}) || numeric->ring.capacity)return publish_preview(std::move(result));
        result.count=std::uint64_t(numeric->actual[0])*numeric->actual[1]*numeric->planes.size();
        NumericLiteral first;
        if(result.count && pipeline::read_numeric_literal(*numeric,{0,0,0},first)!=Status::ok)return publish_preview(std::move(result));
        if(result.count && !std::visit([](auto value){if constexpr(std::is_floating_point_v<decltype(value)>)return std::isfinite(value);else return true;},first))return publish_preview(std::move(result));
        if(result.count)result.exact_value=QString::fromStdString(pipeline::exact_scalar_text(first));
        switch(numeric->contract->shape.axes[0]) {
        case Axis::Sample:result.count_source="Samples";break;
        case Axis::Bin:result.count_source="Bins";break;
        case Axis::Band:result.count_source="Bands";break;
        default:break;
        }
        if(numeric->contract->quantity==Quantity::FilledFrameCount && result.count==1 && std::holds_alternative<std::uint64_t>(first)) {
            result.count_source="Frames";result.count=std::get<std::uint64_t>(first);
        }
    } else {
        const auto& structured=std::get<StructuredView>(frame.payload);
        if(!structured.contract || !same_contract(declared,DataContract{*structured.contract}))return publish_preview(std::move(result));
        result.count=structured.bytes.size();result.count_source="Bytes";
    }
    result.available=true;return publish_preview(std::move(result));
}
bool AudioNodeModel::preview(const pipeline::DeliveredNumericHistoryRow& row) {
    using namespace signal_contracts;
    const auto& delivery=row.compact;
    if(!_numeric || !delivery.context || delivery.context->endpoint.node!=_numeric->id)return false;
    const auto& endpoint=delivery.context->endpoint;
    domain::ResolvedProcessingNode resolved;
    if(!domain::resolve_processing_node(*_numeric,numeric_editor::limits(),resolved) || (endpoint.direction==domain::PortDirection::Input && resolved.role!=domain::NodeRole::Sink))return false;
    const auto& ports=endpoint.direction==domain::PortDirection::Output?_numeric->outputs:_numeric->inputs;
    const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==endpoint.port && p.direction==endpoint.direction;});
    if(port==ports.end() || !port->contract || !delivery.context->contract || !same_contract(port->contract.view(),delivery.context->contract.view()))return false;
    const auto contract=port->contract.view();const auto* history=std::get_if<NumericContract>(&contract);
    if(!history || history->shape.rank!=Rank::Matrix || row.capacity!=history->shape.extents[0].maximum || row.filled_count>row.capacity)return false;
    DiagnosticPreview result;result.direction=endpoint.direction;result.port=endpoint.port;result.epoch=delivery.delivery.epoch;result.revision=delivery.delivery.graph_revision;
    result.count_source="Frames";result.description=numeric_editor::port_caption(*port);
    if(row.event==pipeline::NumericHistoryEvent::Row) {
        if(delivery.status!=Status::ok || !delivery.frame)return publish_preview(std::move(result));
        const auto frame=delivery.frame.view();const auto* numeric=std::get_if<NumericView>(&frame.payload);
        auto expected=*history;expected.semantic=history->history_element;expected.history_element=Semantic::Numeric;
        expected.shape={Rank::Vector,{history->shape.axes[1],Axis::None},{history->shape.extents[1],ExtentBounds{}}};
        expected.coordinates={history->coordinates[1],AxisCoordinates{}};
        if(!numeric || !numeric->contract || !same_physical_contract(expected,*numeric->contract) || !frame.stamp.available || validate_view(frame,numeric_editor::limits().frame)!=Status::ok)return publish_preview(std::move(result));
        result.producer_epoch=frame.stamp.producer_epoch;result.generation=frame.stamp.generation;result.sequence=row.accepted_row_ordinal;result.sequence_seen=true;
        result.available=true;result.count=row.filled_count;
    } else if(row.event==pipeline::NumericHistoryEvent::Empty || row.event==pipeline::NumericHistoryEvent::Reset) {
        if(row.filled_count!=0)return false;
        result.available=true;result.sequence_reset=true;
    }
    return publish_preview(std::move(result));
}
bool AudioNodeModel::preview_history_snapshot(const pipeline::DeliveredNumericFrame& delivery,std::uint64_t ordinal,std::uint64_t filled_count) {
    using namespace signal_contracts;
    if(!_numeric || !delivery.context || delivery.context->endpoint.node!=_numeric->id)return false;
    const auto& endpoint=delivery.context->endpoint;
    domain::ResolvedProcessingNode resolved;
    if(!domain::resolve_processing_node(*_numeric,numeric_editor::limits(),resolved) || (endpoint.direction==domain::PortDirection::Input && resolved.role!=domain::NodeRole::Sink))return false;
    const auto& ports=endpoint.direction==domain::PortDirection::Output?_numeric->outputs:_numeric->inputs;
    const auto port=std::find_if(ports.begin(),ports.end(),[&](const auto& p){return p.id==endpoint.port && p.direction==endpoint.direction;});
    if(port==ports.end() || !port->contract || !delivery.context->contract || !same_contract(port->contract.view(),delivery.context->contract.view()))return false;
    const auto declared=port->contract.view();const auto* history=std::get_if<NumericContract>(&declared);
    if(!history || history->shape.rank!=Rank::Matrix)return false;
    DiagnosticPreview result;result.direction=endpoint.direction;result.port=endpoint.port;result.epoch=delivery.delivery.epoch;result.revision=delivery.delivery.graph_revision;
    result.count_source="Frames";result.description=numeric_editor::port_caption(*port);
    if(delivery.status!=Status::ok || !delivery.frame)return publish_preview(std::move(result));
    const auto frame=delivery.frame.view();const auto* numeric=std::get_if<NumericView>(&frame.payload);
    if(!numeric || !numeric->contract || !same_physical_contract(*history,*numeric->contract) || validate_view(frame,numeric_editor::limits().frame)!=Status::ok)return false;
    if(numeric->actual[0]!=history->shape.extents[0].maximum || filled_count>numeric->actual[0] || ordinal<filled_count)return false;
    if(!frame.stamp.available)return publish_preview(std::move(result));
    result.available=true;result.count=filled_count;
    result.producer_epoch=frame.stamp.producer_epoch;result.generation=frame.stamp.generation;result.sequence=ordinal;result.sequence_seen=true;
    return publish_preview(std::move(result));
}
void AudioNodeModel::set_preview_activity(const QString& status_key) {if(_preview_activity==status_key)return;_preview_activity=status_key;if(!status_key.isEmpty())_diagnostics.clear();update_previews();}
void AudioNodeModel::clear_preview() { _diagnostics.clear();update_previews(); }
QWidget *AudioNodeModel::preview_widget(QWidget *parent,bool compact) {
    auto *card=new NodePreviewCard(parent);
    card->set_compact(compact);
    _previews.removeIf([](const auto &widget) { return widget.isNull(); });
    _previews.append(card);
    update_previews();
    return card;
}
void AudioNodeModel::update_previews() {
    QString errors;for(const auto& p:_diagnostics)errors+=p.failure;
    if(errors!=_diagnostic_error_key){_diagnostic_error_key=errors;emit diagnostics_changed();}
    std::vector<DiagnosticPreview> values;
    domain::ResolvedProcessingNode resolved;
    if(_numeric && domain::resolve_processing_node(*_numeric,numeric_editor::limits(),resolved))for(const auto& port:resolved.role==domain::NodeRole::Sink?_numeric->inputs:_numeric->outputs) {
        auto found=std::find_if(_diagnostics.begin(),_diagnostics.end(),[&](const auto& p){return p.direction==port.direction && p.port==port.id;});
        DiagnosticPreview value;if(found!=_diagnostics.end())value=*found;
        value.direction=port.direction;value.port=port.id;value.description=numeric_editor::port_caption(port);
        if(port.contract){const auto contract=port.contract.view();if(const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract);numeric && numeric->channel_order.size()>1 && !value.exact_value.isEmpty())value.description+=QStringLiteral(" · ")+ui::text("Channel %1").arg(numeric->channel_order[0]);}
        values.push_back(std::move(value));
    }
    for(auto card:_previews)
        if(card) card->set_data(values,_numeric && _numeric->enabled,_preview_activity);
}
QString AudioNodeModel::diagnostic_failure(domain::PortDirection direction,domain::PortId port) const {
    for(const auto& p:_diagnostics)if(p.direction==direction && p.port==port)return p.failure.isEmpty()?QString{}:ui::text(p.failure.toUtf8().constData());
    return {};
}
QString AudioNodeModel::caption() const {
    const auto custom=_presentation.value("display_name").toString().trimmed();if(!custom.isEmpty())return custom;
    if(_kind=="IndexExtract")return ui::text("Index extractor");
    if(_kind=="PcmResampler")return ui::text("PCM resampler");
    if(_kind=="NetworkInput")return ui::text("BDSP input (TCP)");
    if(_kind=="NetworkOutput")return ui::text("BDSP socket output");
    if(_kind=="InputSelector")return ui::text("Input selector");
    if(_kind=="Converter")return ui::text("Converter");
    if(_kind=="Spectrogram")return ui::text("Spectrogram accumulator");
    if(_kind=="SpectrogramView")return ui::text("Spectrogram graph");
    const QMap<QString, const char *> names{{"AudioSource", "Audio source"},
                                            {"Window", "Audio window buffer"},
                                            {"WindowFunction", "Window function"},
                                            {"WindowView", "Audio window graph"},
                                            {"WindowedView", "Windowed audio graph"},
                                            {"WindowFunctionView", "Window function graph"},
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
    const QMap<QString,const char *> additions{{"Splitter","Splitter"},{"Switch","Route switch"},{"Gate","Gate"},
        {"ScalarReduce","Signal measurement"},{"ScalarView","Value graph"},{"WLEDEncode","WLED packet encoder"},{"WLEDOutput","WLED network output"}};
    return names.contains(_kind) ? ui::text(names.value(_kind)) : additions.contains(_kind)?ui::text(additions.value(_kind)):_kind;
}
