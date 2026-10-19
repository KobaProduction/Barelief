#include <desktop/numeric_source_format.h>
#include "synthetic_engine_provider.h"
#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/node_parameter_form.h"
#include "core_app/scheme_editor/numeric_node_operations.h"
#include "core_app/scheme_editor/editor_geometry.h"
#include "barelief/ui_text.h"
#include <desktop/numeric_project_document.h>
#include <pipeline/numeric_projection.h>
#include <QtNodes/DataFlowGraphicsScene>
#include <QApplication>
#include <QPushButton>
#include <QLabel>
#include <QInputDialog>
#include <QTimer>
#include <QClipboard>
#include <QLineEdit>
#include <QCheckBox>
#include <QLineF>
#include <QVBoxLayout>
#include <QtNodes/GraphicsView>
#include <QTreeWidget>
#include <QMenu>
#include <QComboBox>
#include <QTableWidget>
#include <QDoubleSpinBox>
#include <QTransform>
#include <QGraphicsView>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <vector>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void run_editor_check(const char* name,void (*check)(),QStringList& failures){
    ui::Language::instance().set("en");
    try {check();}
    catch(const std::exception& error){failures.append(QString::fromLatin1(name)+": "+QString::fromUtf8(error.what()));}
}
static std::shared_ptr<QtNodes::NodeDelegateModelRegistry> inventory_registry(){
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for(const auto& kind:numeric_editor::catalog())registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);});
    return registry;
}
struct CatalogueSpec {const char* kind;unsigned inputs,outputs;const char* control;NodeParameterModel::Editor editor;};
static std::vector<CatalogueSpec> established_catalogue_specs(){
    using E=NodeParameterModel::Editor;
    // Established product oracles are independent of descriptor enumeration.
    return {
        {"AudioSource",0,1,"captureDevice",E::Choice},{"SignalGenerator",0,1,"oscillatorCount",E::Integer},
        {"SampleClock",0,0,"clockSampleRate",E::Integer},{"PcmResampler",1,1,"output_rate",E::Literal},
        {"Window",1,1,"size",E::Literal},{"WindowFunction",1,1,"function",E::Choice},
        {"StereoFFT",1,1,nullptr,E::Caption},{"BarkWeighting",1,1,nullptr,E::Caption},
        {"Mel",1,1,"bands",E::Literal},{"SpectrumGain",1,1,"gain",E::Literal},
        {"Mixer",2,1,"mixerMode",E::Choice},{"InputSelector",2,1,"numericSelectorChannels",E::Integer},
        {"Switch",1,2,"numericRouteChannels",E::Integer},{"Gate",1,1,"numericGate",E::Choice},
        {"Spectrogram",1,2,"numericCapacity",E::Literal},{"IndexExtract",1,1,"index",E::Literal},
        {"MatrixTranspose",1,1,nullptr,E::Caption},{"MatrixWindowFunction",1,1,"function",E::Choice},
        {"MatrixCenter",1,1,"method",E::Choice},
        {"MatrixFFT",1,1,"fft_size",E::Literal},{"MatrixRow",1,1,"row",E::Literal},
        {"ScalarReduce",1,1,"numericReduction",E::Choice},{"Converter",1,1,"numericRounding",E::Choice},
        {"AudioView",1,0,nullptr,E::Caption},{"WindowView",1,0,nullptr,E::Caption},
        {"WindowedView",1,0,nullptr,E::Caption},{"FFTView",1,0,nullptr,E::Caption},
        {"MelView",1,0,nullptr,E::Caption},{"SpectrogramView",1,0,nullptr,E::Caption},
        {"ScalarView",1,0,nullptr,E::Caption},{"NetworkInput",0,1,"adapterHost",E::Text},
        {"NetworkOutput",1,0,nullptr,E::Caption},{"WLEDEncode",3,1,"gain",E::Literal},
        {"WLEDOutput",1,0,"destination",E::Text},{"ChannelComparison",2,1,"window_samples",E::Literal}
    };
}
static void catalogue_port_contract(NodeParameterModel& parameters,const QString& prefix){
    const auto* caption=parameters.field(prefix+"Caption");
    require(caption && caption->editor==NodeParameterModel::Editor::Caption && !caption->pinnable && caption->section==NodeParameterModel::Section::Contract,"Every data port needs a read-only contract role");
    for(const auto* unsupported:{"Channels","Unit","Layout","Semantic","Meaning"})require(!parameters.field(prefix+unsupported),"Descriptive contract controls must not be registered");
    for(int axis=0;axis<2;++axis)if(const auto* extent=parameters.field(prefix+QString("Extent%1").arg(axis)))require(extent->literal_type==1 && std::holds_alternative<std::uint64_t>(std::get<signal_contracts::NumericLiteral>(extent->read())),"Dimensional editor must use exact UInt64");
}
static void catalogue_contract_controls(AudioNodeModel& node){
    const auto* dto=node.numeric_node();
    for(auto direction:{domain::PortDirection::Input,domain::PortDirection::Output}){
        const auto& ports=direction==domain::PortDirection::Input?dto->inputs:dto->outputs;
        for(const auto& port:ports)catalogue_port_contract(*node.parameter_model(),QString(direction==domain::PortDirection::Input?"input%1":"output%1").arg(port.id));
    }
}
static void matrix_center_offset_availability(NodeParameterModel& parameters){
    for(int method:{0,1}) {
        require(parameters.edit("method",method),"Automatic centering method selection failed");
        const auto* offset=parameters.field("offset");
        require(offset && offset->available && !offset->available(),"Manual centering offset is enabled for an automatic method");
        require(!parameters.edit("offset",signal_contracts::NumericLiteral{1.0}),"Automatic centering accepted a manual offset edit");
    }
    require(parameters.edit("method",2),"Manual centering method selection failed");
    const auto* offset=parameters.field("offset");
    require(offset && offset->available && offset->available(),"Manual centering offset remains disabled in manual mode");
    require(parameters.edit("offset",signal_contracts::NumericLiteral{0.0}),"Manual centering rejected a numeric offset edit");
    require(parameters.edit("method",0),"Default centering method could not be restored");
}
static void matrix_center_controls(const domain::ConfiguredProcessingNode& dto,NodeParameterModel& parameters){
    const auto* method=parameters.field("method");const auto* offset=parameters.field("offset");
    require(method && offset,"Centering controls absent");
    QStringList methods;for(const auto& option:method->options)methods.append(option.label);
    require(methods==QStringList{"Subtract mean","Center between minimum and maximum","Manual offset"} && std::get<int>(method->read())==0 && dto.enabled && method->primary,"Row centering exposes its explicit methods and defaults to enabled mean subtraction");
    require(offset->editor==NodeParameterModel::Editor::Literal && offset->label=="Manual offset","Manual centering offset lacks its numeric editor or user-facing label");
    require(!offset->literal_type && std::holds_alternative<double>(std::get<signal_contracts::NumericLiteral>(offset->read())),"Centering offset must default to Float64 while allowing explicit numeric representation");
    require(offset->available && !offset->available(),"Manual centering offset is disabled for automatic methods");
    matrix_center_offset_availability(parameters);
}
static void matrix_operation_controls(AudioNodeModel& node,const QString& kind){
    using E=NodeParameterModel::Editor;auto* parameters=node.parameter_model();
    if(kind=="MatrixFFT"){
        const auto* size=parameters->field("fft_size");
        require(size && size->editor==E::Literal && std::visit([](auto value){return value==512;},std::get<signal_contracts::NumericLiteral>(size->read())),"Matrix FFT exposes an explicit numeric N with a 512-value default");
        require(!parameters->field("function") && !parameters->field("window_enabled"),"Matrix FFT must not contain a hidden window stage");
    }
    if(kind=="MatrixWindowFunction"){
        const auto* window=parameters->field("function");
        require(window && window->editor==E::Choice && window->options.size()==7 && std::get<int>(window->read())==0 && !node.numeric_node()->enabled,"Matrix window exposes the shared window functions and defaults to generic bypass");
    }
    if(kind=="MatrixRow")require(parameters->field("row")->numeric_minimum==0 && std::visit([](auto value){return value==0;},std::get<signal_contracts::NumericLiteral>(parameters->field("row")->read())),"Matrix row selection must begin at zero");
    if(kind=="MatrixCenter")matrix_center_controls(*node.numeric_node(),*parameters);
}
static void wled_encoder_port_contracts(const domain::ConfiguredProcessingNode& node){
    using namespace signal_contracts;
    const auto& frequency=std::get<NumericContract>(node.inputs[1].contract.view());
    const auto& magnitude=std::get<NumericContract>(node.inputs[2].contract.view());
    require(node.inputs[0].id==0 && node.inputs[1].id==1 && node.inputs[2].id==2,"WLED encoder explicit inputs lost stable IDs");
    require(frequency.shape.rank==Rank::Scalar && frequency.dtype==DType::Float32 && frequency.unit==NumericUnit::Hertz,"WLED encoder needs its explicit scalar peak-frequency input");
    require(magnitude.shape.rank==Rank::Scalar && magnitude.dtype==DType::Float32 && magnitude.quantity==Quantity::SpectrumPeakAmplitude,"WLED encoder needs its explicit scalar peak-magnitude input");
    require(std::holds_alternative<StructuredContract>(node.outputs[0].contract.view()),"WLED encoder output must preserve its structured packet contract");
}
static void catalogue_specialized_controls(AudioNodeModel& node,const QString& kind){
    auto* parameters=node.parameter_model();
    if(numeric_editor::is_graph_view(kind))require(!parameters->field("input0Dtype") && !parameters->field("input0Extent0"),"View must derive its contract from upstream");
    if(kind=="AudioSource")require(!parameters->field("sample_rate") && parameters->field("source") && parameters->field("captureSavedFormat") && parameters->field("captureDeviceError") && parameters->field("refreshCaptureDevices"),"Capture operation and adapter responsibilities are not separated");
    if(kind=="NetworkInput")for(const auto* key:{"adapterProviderUuid","adapterStreamId","adapterHost","adapterPort","adapterDeviceIdentity","adapterGraphId","adapterNodeId","adapterPortIndex","adapterDirection"})require(parameters->field(key),"Network adapter route editor absent");
    if(kind=="Spectrogram")require(node.portCaption(QtNodes::PortType::Out,0).startsWith(ui::text("History")) && node.portCaption(QtNodes::PortType::Out,1).startsWith(ui::text("Filled frames")),"History/count outputs have ambiguous roles");
    if(kind=="WLEDEncode")wled_encoder_port_contracts(*node.numeric_node());
    matrix_operation_controls(node,kind);
}
static void catalogue_form_appearance(AudioNodeModel& node,QWidget& form){
    auto* bypass=form.findChild<QCheckBox*>("numericBypass");
    require(bool(bypass)==node.parameter_model()->supportsBypass(),"Catalogue form exposes bypass only for supported identity operations");
    if(bypass)require(bypass->text()==ui::text("Bypass"),"Catalogue form fails live translated bypass");
    for(auto* control:form.findChildren<QWidget*>())for(auto group:{QPalette::Active,QPalette::Inactive,QPalette::Disabled})
        require(control->palette().color(group,QPalette::WindowText)==QApplication::palette().color(group,QPalette::WindowText),"Catalogue control overrides centralized theme text colors");
    form.setEnabled(false);if(bypass)require(!bypass->isEnabled(),"Catalogue disabled controls ignore their inspector");form.setEnabled(true);
}
static void catalogue_live_preferences(AudioNodeModel& node,QWidget& form){
    const auto saved=node.save();
    for(const auto& language:{QString("ru"),QString("en")})for(const auto& theme:{QString("light"),QString("dark")}){
        ui::Language::instance().set(language);ui::Theme::instance().set(theme);QCoreApplication::processEvents();
        catalogue_form_appearance(node,form);
        require(node.save()==saved,"Catalogue localization/theme mutates processing configuration");
    }
}
static void catalogue_operation(const CatalogueSpec& spec){
    AudioNodeModel node(spec.kind);const auto* dto=node.numeric_node();auto* parameters=node.parameter_model();
    require(domain::data_inputs(*dto).size()==spec.inputs && domain::data_outputs(*dto).size()==spec.outputs,qPrintable(QString("Wrong mono data port roles for ")+spec.kind));
    if(spec.control){const auto* field=parameters->field(spec.control);require(field && field->editor==spec.editor,qPrintable(QString("Missing operation/adapter editor for ")+spec.kind));}
    catalogue_contract_controls(node);
    std::unique_ptr<QWidget> form(node.parameter_widget());
    const auto saved=node.save();node.set_processing_activity(true,true,120);require(node.save()==saved,"Catalogue diagnostics mutate a configured node");
    catalogue_specialized_controls(node,spec.kind);
    catalogue_live_preferences(node,*form);
}
static void oscillator_table_translation(){
    AudioNodeModel generator("SignalGenerator");std::unique_ptr<QWidget> form(generator.parameter_widget());auto* table=form->findChild<QTableWidget*>("oscillatorTable");require(table,"Generator table absent");
    const auto saved=generator.save();
    for(const auto& language:{QString("ru"),QString("en"),QString("ru")}){
        ui::Language::instance().set(language);QCoreApplication::processEvents();
        const char* labels[]{"Enabled","Frequency, Hz","Amplitude","Phase, radians","Attack, seconds","Release, seconds"};
        for(int column=0;column<6;++column)require(table->horizontalHeaderItem(column+1)->text()==ui::text(labels[column]),"Open oscillator headers fail live language update");
        require(generator.save()==saved,"Language switching changes oscillator parameters");
    }
    ui::Language::instance().set("en");
}
static void catalogue_acceptance_checks(){
    const auto specs=established_catalogue_specs();
    const auto actual=numeric_editor::catalog();
    for(const auto& spec:specs)require(actual.contains(spec.kind),qPrintable(QString("Missing established catalogue operation: ")+spec.kind));
    for(const auto& kind:actual){AudioNodeModel node(kind);require(node.numeric_node() && node.parameter_model(),"Catalogue operation has no numeric UI contract");}
    for(const auto& spec:specs)catalogue_operation(spec);
    oscillator_table_translation();
}
static void configure_oscillator_fixture(NumericEditorGraphModel& graph,QtNodes::NodeId id,AudioNodeModel& node){
    auto configured=*node.numeric_node();auto& tones=std::get<domain::SignalGeneratorProcessingParameters>(configured.parameters).oscillators;
    tones.resize(3);tones[0].id=7;tones[0].frequency_hz=440.123456789;tones[1].id=19;tones[1].frequency_hz=880;tones[2].id=41;tones[2].frequency_hz=1320;
    require(graph.edit_node(id,configured),"Exact oscillator fixture rejected");
}
static void oscillator_exact_edits(AudioNodeModel& node,QWidget& form,QUndoStack& undo){
    auto* edit=form.findChild<QLineEdit*>("oscillator0frequency_hz");
    const auto before=node.save();const int index=undo.index();
    require(edit->text().toDouble()==440.123456789,"Oscillator display loses Float64 precision");
    QMetaObject::invokeMethod(edit,"editingFinished",Qt::DirectConnection);
    require(node.save()==before && undo.index()==index,"Untouched oscillator creates lossy undo command");
    edit->setText("440.123456789");edit->setModified(true);emit edit->textEdited(edit->text());
    QMetaObject::invokeMethod(edit,"editingFinished",Qt::DirectConnection);
    require(node.save()==before && undo.index()==index,"Exact no-op oscillator edit creates undo command");
}
static void oscillator_draft_validation(AudioNodeModel& node,QWidget& form,QUndoStack& undo){
    auto* model=node.parameter_model();
    auto* edit=form.findChild<QLineEdit*>("oscillator0frequency_hz");
    edit->setText("987.654321987");edit->setModified(true);emit edit->textEdited(edit->text());
    model->setBypassed(true);ui::Language::instance().set("ru");QCoreApplication::processEvents();
    require(edit->text()=="987.654321987" && model->editState("oscillator0frequency_hz").dirty,"Bypass/language overwrites oscillator draft");
    ui::Language::instance().set("en");
    const auto invalidBefore=node.save();const int invalidIndex=undo.index();
    edit->setText("-1");edit->setModified(true);emit edit->textEdited(edit->text());
    QMetaObject::invokeMethod(edit,"editingFinished",Qt::DirectConnection);
    require(node.save()==invalidBefore && undo.index()==invalidIndex && !model->editState("oscillator0frequency_hz").error.isEmpty(),"Invalid oscillator frequency mutates model or lacks error");
    require(form.findChild<QLabel*>("oscillator0frequency_hz_error")->text().size()>0,"Oscillator validation is not visible");
}
static void oscillator_remove_undo(AudioNodeModel& node,QWidget& form,QUndoStack& undo){
    auto* model=node.parameter_model();
    model->setDraftInput("oscillator2amplitude","0.123456789",3);
    const auto original=*node.numeric_node();const int removeIndex=undo.index();
    auto* remove=form.findChild<QPushButton*>("oscillator1Remove");require(remove,"Per-row remove action missing");remove->click();
    const auto& remaining=std::get<domain::SignalGeneratorProcessingParameters>(node.numeric_node()->parameters).oscillators;
    require(remaining.size()==2 && remaining[0].id==7 && remaining[1].id==41 && undo.index()==removeIndex+1,"Middle removal renumbers stable oscillator IDs");
    require(model->editState("oscillator1amplitude").input=="0.123456789" && !model->editState("oscillator2amplitude").dirty,"Draft does not follow oscillator ID after middle removal");
    QCoreApplication::processEvents();
    require(form.findChild<QLabel*>("oscillator1Id")->text()=="41","Oscillator row identity not displayed");
    undo.undo();QCoreApplication::processEvents();
    require(domain::same_processing_parameters(original.parameters,node.numeric_node()->parameters),"Undo does not restore exact oscillator bank and IDs");
    require(model->editState("oscillator2amplitude").input=="0.123456789" && !model->editState("oscillator1amplitude").dirty,"Undo does not remap surviving oscillator draft by ID");
}
static void oscillator_bank_acceptance_checks(){
    NumericEditorGraphModel graph(inventory_registry());QUndoStack undo;graph.set_undo_stack(&undo);
    const auto id=graph.addNode("SignalGenerator");auto* node=graph.delegateModel<AudioNodeModel>(id);
    configure_oscillator_fixture(graph,id,*node);
    std::unique_ptr<QWidget> form(node->parameter_widget());
    require(form->findChild<QLineEdit*>("oscillator0frequency_hz"),"Exact oscillator text editor missing");
    oscillator_exact_edits(*node,*form,undo);
    oscillator_draft_validation(*node,*form,undo);
    oscillator_remove_undo(*node,*form,undo);
}
static void check_dimensional_field(AudioNodeModel& node,QWidget& form,QWidget& contracts,const QString& key){
    auto* model=node.parameter_model();
    const auto saved=node.save();const auto value=std::get<signal_contracts::NumericLiteral>(model->field(key)->read());
    const auto text=QString::number(std::get<std::uint64_t>(value));
    // Contract extents live in the dedicated contracts form, outside the inspector.
    auto* fieldForm=model->field(key)->section==NodeParameterModel::Section::Contract?&contracts:&form;
    auto* type=fieldForm->findChild<QComboBox*>(key+"_type");auto* edit=fieldForm->findChild<QLineEdit*>(key);
    require(type && edit && type->isHidden() && type->currentIndex()==1,"Size type chooser must be fixed to UInt64");
    for(int unsupported:{0,2,3})require(!model->editLiteral(key,text,unsupported) && node.save()==saved,"Size editor accepted unsupported literal representation");
}
static void check_dimensional_node(const QString& kind){
    AudioNodeModel node(kind);auto* model=node.parameter_model();std::unique_ptr<QWidget> form(node.parameter_widget());
    NodeParameterForm contracts(model,false,nullptr,true);
    QStringList sizes;
    for(const auto& field:model->fields())if(field.editor==NodeParameterModel::Editor::Literal && field.literal_type==1)sizes.append(field.key);
    for(const auto& key:sizes)check_dimensional_field(node,*form,contracts,key);
}
static void check_registered_size_writers(){
    struct Size {const char* kind;const char* key;int type;};
    for(const auto& size:std::array<Size,5>{{{"Window","size",0},{"Mel","bands",0},{"ChannelComparison","window_samples",0},{"IndexExtract","index",0},{"Spectrogram","numericCapacity",1}}}){
        AudioNodeModel node(size.kind);auto* model=node.parameter_model();const auto* field=model->field(size.key);
        require(field && field->literal_type==size.type,"Operation size editor lacks its registered exact writer type");
        const auto saved=node.save();for(int unsupported:{2,3})require(!model->editLiteral(size.key,"512",unsupported) && node.save()==saved,"Operation size accepted a floating point literal");
    }
 }
static void dimensional_editor_acceptance_checks(){
    for(const auto& kind:numeric_editor::catalog())check_dimensional_node(kind);
    AudioNodeModel history("Spectrogram");require(history.parameter_model()->editLiteral("numericCapacity","17",1),"UInt64 History capacity edit rejected");
    require(std::get<domain::HistoryProcessingParameters>(history.numeric_node()->parameters).capacity==17,"Capacity writer does not retain UInt64 size");
    check_registered_size_writers();
}
static void grouped_stream_name_roundtrip(NumericEditorGraphModel& connected,QUndoStack& history,QtNodes::NodeId route,const QString& longName){
    auto* routeModel=connected.delegateModel<AudioNodeModel>(route);auto presentation=routeModel->presentation();presentation["stream_names"]=QJsonArray{"Generator",longName};routeModel->set_presentation(presentation);
    const int before=history.index();require(routeModel->select_input(1),"Named stream selection failed");require(routeModel->operation_summary().contains(longName),"Selected stream name absent from card summary");
    history.undo();require(routeModel->selected_input()==0 && routeModel->operation_summary().contains("Generator"),"Undo leaves stale selected stream name");history.redo();require(history.index()==before+1 && routeModel->operation_summary().contains(longName),"Redo loses selected stream name");
    NumericEditorGraphModel restored(inventory_registry());restored.load(connected.save());auto* restoredRoute=restored.delegateModel<AudioNodeModel>(route);require(restoredRoute && restoredRoute->selected_input()==1 && restoredRoute->operation_summary().contains(longName),"Document reload loses selected stream name");
}
static void connected_grouped_stream_checks(const QString& longName){
    const auto registry=inventory_registry();
    NumericEditorGraphModel connected(registry);QUndoStack history;connected.set_undo_stack(&history);
    const auto producer=connected.addNode("AudioSource"),route=connected.addNode("InputSelector");
    connected.addConnection({producer,0,route,1});require(connected.connectionExists({producer,0,route,1}),"Connected selector fixture absent");
    const auto saved=connected.save();const int checkpoint=history.index();auto changed=*connected.delegateModel<AudioNodeModel>(route)->numeric_node();
    require(numeric_editor::set_selector_channels(changed,2),"Candidate grouped C rejected");
    require(connected.edit_node(route,changed) && history.index()==checkpoint+1 && connected.connectionExists({producer,0,route,1}),"Grouped draft edit must retain the existing stable endpoint in one Undo command");
    require(connected.delegateModel<AudioNodeModel>(route)->grouped_channels(QtNodes::PortType::In)==2,"Grouped draft edit did not retain the explicit channel setting");
    history.undo();
    require(connected.save()==saved && history.index()==checkpoint && connected.connectionExists({producer,0,route,1}),"Grouped draft Undo must restore geometry and the existing edge atomically");
    grouped_stream_name_roundtrip(connected,history,route,longName);
}
class GroupedLayoutChecks {
    NumericEditorGraphModel graph;
    QUndoStack undo;
    QtNodes::DataFlowGraphicsScene scene;
    QGraphicsView canvas;
    QtNodes::NodeId source,selector,router;
    QString longName;
    EditorNodeGeometry& geometry(){return static_cast<EditorNodeGeometry&>(scene.nodeGeometry());}
    void configure_capture(unsigned channels){
        auto* capture=graph.delegateModel<AudioNodeModel>(source);
        auto physical=*capture->numeric_node();const auto mono=physical.outputs.front();physical.outputs.clear();
        for(unsigned lane=0;lane<channels;++lane){auto port=mono;port.id=lane;physical.outputs.push_back(port);}
        capture->set_numeric_node(physical);
        capture->set_presentation(QJsonObject{{"display_name",longName}});
    }
    void configure_routes(unsigned channels){
        for(auto id:{selector,router}){
            auto* node=graph.delegateModel<AudioNodeModel>(id);auto dto=*node->numeric_node();
            require(id==selector?numeric_editor::set_selector_channels(dto,channels):numeric_editor::set_route_channels(dto,channels),"Configure grouped layout channels");
            require(id==selector?numeric_editor::set_input_count(dto,3):numeric_editor::set_output_count(dto,3),"Configure three grouped streams");node->set_numeric_node(dto);
            auto presentation=node->presentation();presentation["display_name"]=longName;presentation["stream_names"]=QJsonArray{longName,longName+" 2",longName+" 3"};node->set_presentation(presentation);
        }
    }
    void check_caption(QtNodes::NodeId id){
        auto captionFont=QApplication::font();
        const bool nickname= !graph.nodeData(id,QtNodes::NodeRole::Label).toString().isEmpty();
        captionFont.setBold(!nickname);captionFont.setItalic(nickname);
        const QFontMetricsF captionMetrics(captionFont);
        const auto captionText=graph.nodeData(id,QtNodes::NodeRole::Caption).toString();
        const auto textBounds=captionMetrics.boundingRect(captionText);
        const auto baseline=geometry().captionPosition(id);
        const double captionCenter=baseline.x()+geometry().captionRect(id).width()/2.;
        const QRectF paintedCaption(captionCenter-textBounds.width()/2.,baseline.y()-captionMetrics.ascent(),
        std::max(textBounds.width(),captionMetrics.horizontalAdvance(captionText)),captionMetrics.height());
        require(geometry().boundingRect(id).contains(paintedCaption),"Long user node name clips its painted header bounds");
    }
    void check_port_spacing(AudioNodeModel& node,QtNodes::NodeId id,QtNodes::PortType side,unsigned port,unsigned channels){
        if(!node.grouped_channels(side) || port==0)return;
        const double spacing=geometry().portPosition(id,side,port).y()-geometry().portPosition(id,side,port-1).y();
        const double pitch=id==selector && side==QtNodes::PortType::In?QFontMetricsF(geometry().label_font()).height()+16:geometry().pitch();
        require(std::abs(spacing-pitch-(port%channels==0?12.:0.))<1e-6,"Ports are not compact within a stream and separated between streams");
    }
    void check_port_zoom(QtNodes::NodeId id,QtNodes::PortType side,unsigned port,QPointF pin){
        for(double zoom:{.25,1.,2.}){
            canvas.setTransform(QTransform::fromScale(zoom,zoom));const auto local=canvas.viewportTransform().inverted().map(QPointF(canvas.mapFromScene(pin)));
            require(geometry().checkPortHit(id,side,local)==port,"Zoom changes multichannel pin hit routing");
        }
    }
    void check_selector_button(QtNodes::NodeId id,QtNodes::PortType side,unsigned port,unsigned channels){
        if(id!=selector || side!=QtNodes::PortType::In)return;
        const auto button=geometry().selectorButtonRect(id,port);
        if(port%channels==0)require(!button.isEmpty() && geometry().selectorButtonHit(id,button.center())==port,"Grouped selector button does not select the first port of its stream");
        else require(button.isEmpty(),"Grouped selector exposes a separate button for an interior channel");
    }
    void check_port(AudioNodeModel& node,QtNodes::NodeId id,QtNodes::PortType side,unsigned port,unsigned channels){
        const auto pin=geometry().portPosition(id,side,port),label=geometry().portTextPosition(id,side,port);
        QFontMetricsF metrics(geometry().label_font());
        for(const auto& line:node.portCaption(side,port).split('\n'))require(label.x()>=0 && label.x()+metrics.horizontalAdvance(line)<=geometry().size(id).width(),"Long RU/EN port caption clips node bounds");
        require(geometry().checkPortHit(id,side,pin)==port,"Multichannel pin hit loses actual port index");
        check_port_spacing(node,id,side,port,channels);
        check_port_zoom(id,side,port,pin);
        check_selector_button(id,side,port,channels);
    }
    void check_node(QtNodes::NodeId id,unsigned channels){
        auto* node=graph.delegateModel<AudioNodeModel>(id);auto* body=node->embeddedWidget();geometry().recomputeSize(id);
        require(geometry().boundingRect(id).contains(QRectF(geometry().widgetPosition(id),body->size())),"Multichannel card clips embedded controls");
        check_caption(id);
        for(auto side:{QtNodes::PortType::In,QtNodes::PortType::Out}){
            const auto numeric=side==QtNodes::PortType::In?node->numeric_node()->inputs.size():node->numeric_node()->outputs.size();
            for(unsigned port=0;port<numeric;++port)check_port(*node,id,side,port,channels);
        }
    }
    void check_preferences(unsigned channels){
        for(const auto& language:{QString("ru"),QString("en")})for(const auto& theme:{QString("light"),QString("dark")}){
            ui::Language::instance().set(language);ui::Theme::instance().set(theme);QCoreApplication::processEvents();
            for(auto id:{source,selector,router})check_node(id,channels);
        }
    }
public:
    GroupedLayoutChecks():graph(inventory_registry()),scene(graph),canvas(&scene){
        graph.set_undo_stack(&undo);
        scene.setNodeGeometry(std::make_unique<EditorNodeGeometry>(graph,[]{return QJsonObject{};}));
        canvas.resize(1000,700);
        source=graph.addNode("AudioSource");selector=graph.addNode("InputSelector");router=graph.addNode("Switch");
        longName=QString::fromUtf8("Длинное пользовательское имя потока — генераторы и источник звука для анализа частот");
    }
    void run(){
        for(unsigned channels:{1u,2u,4u,8u}){configure_capture(channels);configure_routes(channels);check_preferences(channels);}
        ui::Language::instance().set("en");ui::Theme::instance().set("dark");
        connected_grouped_stream_checks(longName);
    }
};
static void grouped_layout_acceptance_checks(){GroupedLayoutChecks checks;checks.run();}
static void node_activity_checks(AudioNodeModel& model,QWidget* body){
    if(!body)return;
    auto* activity=body->findChild<QLabel*>("pipelineNodeActivity");require(activity,"Block processing activity indicator absent");
    const auto configured=model.save();model.set_processing_activity(true,true,120.);
    require(activity->text()==ui::text("Activity: processing") && activity->toolTip().contains("120.0"),"Activity indicator ignores measured node telemetry");
    const auto statusText=activity->text();
    for(int i=0;i<100;++i)model.set_processing_activity(true,true,100.+i);
    require(activity->text()==statusText && model.save()==configured,"Execution counter refresh changes status/processing parameters");
    model.set_processing_activity(true,false,0.);require(activity->text()==ui::text("Activity: no processing in interval"),"Inactive interval retains active node indicator");
    model.set_processing_activity(false,false,120.);require(activity->toolTip().contains(QStringLiteral("—")),"Unmeasured activity displays configured target as measured rate");
}
static void scalar_inspector_order(QWidget* body,QWidget& inspector){
    auto* form=inspector.findChild<QWidget*>("numericNodeParameters");require(form,"Scalar parameter form absent");
    auto* wrapper=form->parentWidget();
    require(wrapper && wrapper->layout() && wrapper->layout()->count()==2 && wrapper->layout()->itemAt(0)->widget()==form && wrapper->layout()->itemAt(1)->widget() && wrapper->layout()->itemAt(1)->widget()->objectName()=="scalarInputsEditor","Scalar wrappers must place parameter form before metric input controls");
    require(!body || !body->findChild<QWidget*>("scalarInputsEditor"),"Scalar input management belongs to the inspector");
}
static void bypass_edit_checks(AudioNodeModel& model,QCheckBox& inspector){
    const auto presentation=model.presentation();
    model.parameter_model()->setBypassed(true);
    require(!model.numeric_node()->enabled && inspector.isChecked() && model.presentation()==presentation,"Bypass changes execution mode and synchronizes the inspector");
    inspector.setChecked(false);
    require(model.numeric_node()->enabled && !inspector.isChecked(),"Inspector must restore processing");
    model.set_numeric_edit_handler([](const auto&){return false;});inspector.setChecked(true);
    require(model.numeric_node()->enabled && !inspector.isChecked(),"Rejected bypass edit must restore bound controls");
}
static void bypass_policy_checks(AudioNodeModel& model,QWidget* body,QWidget& inspector,const QString& kind){
    require(!body || !body->findChild<QCheckBox*>("numericBypass"),"Inline node bypass uses the shared node painter");
    auto* toggle=inspector.findChild<QCheckBox*>("numericBypass");
    require(bool(toggle)==model.parameter_model()->supportsBypass(),"Only supported identity operations expose inspector bypass");
    require(!model.parameter_model()->field("numericEnabled") && !model.parameter_model()->field("numericBypass") && !inspector.findChild<QAction*>("showOnBlock_numericEnabled") && !inspector.findChild<QAction*>("showOnBlock_numericBypass"),"Bypass must not be a pinnable processing parameter");
    require(!model.set_parameter_on_block("numericBypass",false) && !model.parameter_model()->setOnBlock("numericEnabled",true),"Presentation pinning cannot hide or duplicate bypass");
    if(kind=="ScalarView")scalar_inspector_order(body,inspector);
    if(!toggle)return;
    const bool defaultBypass=kind=="MatrixWindowFunction";
    require(model.numeric_node()->enabled==!defaultBypass && toggle->isChecked()==defaultBypass,"Matrix window defaults to identity bypass; other identity operations process");
    auto* form=inspector.findChild<QWidget*>("numericNodeParameters");
    require(form && form->layout()->itemAt(0)->widget()==toggle,"Inspector bypass stays at the top of its parameter form");
    bypass_edit_checks(model,*toggle);
}
static void bypass_controls_checks(){
    for(const auto& kind:numeric_editor::catalog()){
        AudioNodeModel model(kind);
        std::unique_ptr<QWidget> body(model.embeddedWidget()),inspector(model.parameter_widget());
        node_activity_checks(model,body.get());bypass_policy_checks(model,body.get(),*inspector,kind);
    }
}
static void selector_repair_and_names_checks(){
    AudioNodeModel broken("InputSelector");auto malformed=*broken.numeric_node();malformed.outputs.clear();broken.set_numeric_node(malformed);
    const auto before=broken.save();std::unique_ptr<QWidget> brokenBody(broken.embeddedWidget()),brokenInspector(broken.parameter_widget());
    require(std::get<int>(broken.parameter_model()->field("numericInputs")->read())==0,"Empty selector outputs must show unavailable stream geometry safely");
    require(!broken.select_input(99) && broken.save()==before,"Rejected malformed selector selection must be atomic");
    AudioNodeModel mono("InputSelector");auto sparse=*mono.numeric_node();require(numeric_editor::set_input_count(sparse,3) && numeric_editor::remove_input(sparse,1),"Sparse mono selector fixture rejected");mono.set_numeric_node(sparse);
    std::unique_ptr<QWidget> inspector(mono.parameter_widget());auto* retained=inspector->findChild<QLineEdit*>("streamName2");
    require(retained && !inspector->findChild<QLineEdit*>("streamName1"),"Mono stream name controls must follow retained stable port IDs");
    retained->setText("Retained stream");QMetaObject::invokeMethod(retained,"editingFinished",Qt::DirectConnection);
    require(mono.stream_name(2)=="Retained stream" && mono.portCaption(QtNodes::PortType::In,mono.port_index(QtNodes::PortType::In,2)).contains("Retained stream"),"Retained stream rename must update its actual port caption");
}
static void canonical_selector_checks(){
    AudioNodeModel selector("InputSelector");require(selector.parameter_model()->field("numericSelectorChannels"),"Selector channels field filtered by suffix");
    auto grouped=*selector.numeric_node();require(numeric_editor::set_selector_channels(grouped,2) && numeric_editor::set_input_count(grouped,3),"Grouped selector geometry rejected");
    selector.set_numeric_node(grouped);
    require(selector.grouped_channels(QtNodes::PortType::In)==2 && domain::data_inputs(*selector.numeric_node()).size()==6,"Selector requires three groups of two ports");
    require(selector.portCaption(QtNodes::PortType::In,0).contains(ui::text("Channel %1").arg(1)) && selector.portCaption(QtNodes::PortType::In,1).contains(ui::text("Channel %1").arg(2)),"Selector channel captions lose positions");
    require(selector.parameter_model()->edit("numericResetSettings",false),"Grouped selector reset rejected");
    const auto& parameters=std::get<domain::TransformProcessingParameters>(selector.numeric_node()->parameters).values;
    require(std::count_if(parameters.begin(),parameters.end(),[](const auto& value){return value.key=="channels_per_stream";})==1,"Selector reset duplicates registered channel setting");
    require(domain::data_inputs(*selector.numeric_node()).size()==6 && domain::data_outputs(*selector.numeric_node()).size()==2,"Selector reset changes explicit port geometry");
}
static void canonical_routing_checks(){
    AudioNodeModel router("Switch");auto route=*router.numeric_node();
    require(numeric_editor::set_route_channels(route,2) && numeric_editor::set_output_count(route,3),"Grouped output switch rejected");router.set_numeric_node(route);
    require(domain::data_inputs(*router.numeric_node()).size()==2 && domain::data_outputs(*router.numeric_node()).size()==6 && router.grouped_channels(QtNodes::PortType::Out)==2,"Output switch must preserve C channels across S groups");
    AudioNodeModel clock("SampleClock");const auto clockPin=clock.configuration_port(QtNodes::PortType::Out,domain::ConfigurationRole::SampleClock);
    require(clockPin!=QtNodes::InvalidPortIndex && clock.configuration_role(QtNodes::PortType::Out,clockPin)==domain::ConfigurationRole::SampleClock,"Configuration clock port not exposed");
}
static void canonical_processing_controls_checks(){
    AudioNodeModel window("WindowFunction");auto* function=window.parameter_model()->field("function");
    require(function && function->editor==NodeParameterModel::Editor::Choice && function->options.size()==7,"Window function requires the complete shared enumerated editor");
    require(window.parameter_model()->edit("function",1),"Window choice edit rejected");
    require(std::get<int>(window.parameter_model()->field("function")->read())==1,"Window choice does not roundtrip canonical string parameter");
    AudioNodeModel history("Spectrogram");auto* capacity=history.parameter_model()->field("numericCapacity");
    require(capacity && capacity->literal_type==1,"History capacity must retain UInt64 representation");
    AudioNodeModel generator("SignalGenerator");require(generator.parameter_model()->edit("generatorBank1",false),"Generator bank action rejected");
    require(std::get<domain::SignalGeneratorProcessingParameters>(generator.numeric_node()->parameters).oscillators.size()==8,"Multitone bank not reflected in ordinary parameters");
    std::unique_ptr<QWidget> panel(generator.parameter_widget());require(panel->findChild<QWidget*>("oscillatorTable"),"Generator bank table absent");
}
static void literal_device_caption_checks(){
    AudioNodeModel capture("AudioSource");
    capture.set_capture_device_provider([](int,QString*){return QJsonArray{QJsonObject{{"id","literal-caption"},{"name","Input %1"}}};});
    std::unique_ptr<QWidget> inspector(capture.parameter_widget());
    auto* devices=inspector->findChild<QComboBox*>("captureDevice");
    require(devices && devices->findText("Input %1")>=0,"Literal device caption was interpreted as a numbered port label");
    require(ui::Language::instance().set("ru"),"RU language unavailable");
    require(devices->findText("Input %1")>=0,"Literal device caption was translated during language refresh");
    require(ui::Language::instance().set("en"),"EN language unavailable");
}
static void canonical_capture_controls_checks(){
    AudioNodeModel capture("AudioSource");int generation=0;
    capture.set_capture_device_provider([&](int,QString*){QJsonArray devices;for(int i=0;i<generation;++i)devices.append(QJsonObject{{"id",QString::number(i)},{"name",QString("Device %1").arg(i)}});return devices;});
    generation=2;require(capture.parameter_model()->edit("refreshCaptureDevices",false),"Capture refresh rejected");
    require(capture.parameter_model()->field("captureDevice")->options.size()==3,"Capture refresh retains old choices");
}
static void canonical_mixer_controls_checks(){
    AudioNodeModel mixer("Mixer");require(mixer.parameter_model()->field("mixerInputCount")->minimum==1,"Mixer cannot use one independent input");
    auto node=*mixer.numeric_node();auto& parameters=std::get<domain::MixerProcessingParameters>(node.parameters);
    parameters.mode=domain::MixerMode::WeightedMean;
    for(auto& weight:parameters.weights)weight.weight=std::numeric_limits<double>::max();
    mixer.set_numeric_node(node);
    const auto key=QString("mixerShare%1").arg(parameters.weights.front().port);
    require(std::get<QString>(mixer.parameter_model()->field(key)->read())=="50.00 %","Large finite weights overflow normalized UI proportion");
    for(auto& weight:parameters.weights)weight.weight=0;
    mixer.set_numeric_node(node);require(std::get<QString>(mixer.parameter_model()->field(key)->read())=="0.00 %","All-zero weights must show zero proportion");
}
static void explicit_profile_controls_checks(){
    AudioNodeModel selector("InputSelector");auto node=*selector.numeric_node();
    require(numeric_editor::set_selector_channels(node,2) && numeric_editor::set_input_count(node,3),"Profile fixture geometry rejected");selector.set_numeric_node(node);
    auto profile=numeric_editor::limits();profile.max_ports_per_node=2;
    const auto before=selector.save();selector.set_admission_limits(profile);
    std::unique_ptr<QWidget> inspector(selector.parameter_widget());auto* count=inspector->findChild<QSpinBox*>("numericInputs");
    require(count && count->value()==3,"Profile refresh clamps an existing draft value");
    require(selector.save()==before && selector.parameter_model()->field("numericInputs")->maximum==1,"Profile refresh changes draft or ignores geometry budget");
    require(!selector.parameter_model()->edit("numericInputs",2) && selector.save()==before,"Profile-dependent edit exceeds supplied execution profile");
    selector.set_admission_limits(std::nullopt);QApplication::processEvents();count=inspector->findChild<QSpinBox*>("numericInputs");
    require(count && count->value()==3 && !count->isEnabled() && selector.save()==before,"Missing profile uses fallback policy or mutates draft");
}
static void literal_operation_summary_checks(){
    AudioNodeModel fft("MatrixFFT");
    require(fft.parameter_model()->editLiteral("fft_size","512",0),"Matrix FFT size edit rejected");
    require(fft.operation_summary().contains("512"),"Matrix FFT summary infers size from a removed choice index");
    AudioNodeModel row("MatrixRow");
    require(row.parameter_model()->editLiteral("row","1",0),"Matrix row selection rejected");
    require(row.operation_summary().endsWith(": 1"),"Matrix row summary reads a literal as an integer editor index");
}
static void canonical_parameter_checks(){
    literal_operation_summary_checks();canonical_selector_checks();canonical_routing_checks();canonical_processing_controls_checks();canonical_capture_controls_checks();literal_device_caption_checks();canonical_mixer_controls_checks();explicit_profile_controls_checks();
}
struct PreviewPools {
    alignas(64) std::array<std::byte,8192> payload{},metadata{};
    std::array<signal_contracts::BufferSlot,4> ps{},ms{};
    signal_contracts::BufferPool p{payload,ps,2048},m{metadata,ms,2048};
};
static pipeline::DeliveredNumericFrame scalar_delivery(PreviewPools& pools,const domain::ConfiguredProcessingNode& node,
    pipeline::DeliveryStamp delivery,std::uint64_t value,std::uint64_t sequence) {
    using namespace signal_contracts;
    auto context=std::make_shared<pipeline::NumericDeliveryContext>();context->endpoint={"main",node.id,domain::PortDirection::Input,0};context->contract=node.inputs[0].contract;
    auto contract=std::get<NumericContract>(context->contract.view());
    alignas(64) std::array<std::byte,8> bytes{};const std::array<PlaneLayout,1> planes{{{0,{8,8}}}};
    MutableNumericView numeric{&contract,bytes,planes,{1,1},{},{}};require(store_element(numeric,{},value)==Status::ok,"Fixture UInt64 storage");
    FrameStamp stamp;stamp.sequence=sequence;stamp.producer_epoch=1;stamp.epoch=delivery.epoch;stamp.graph_revision=delivery.graph_revision;
    DataFrameView frame{stamp,numeric.view()};pipeline::DeliveredNumericFrame result;
    require(pipeline::retain_numeric_delivery(context,delivery,frame,Status::ok,numeric_editor::limits().frame,pools.p,pools.m,result)==Status::ok,"Fixture bounded retention");return result;
}
class ScalarPreviewChecks {
    using Status=signal_contracts::Status;
    using DType=signal_contracts::DType;
    using NumericContract=signal_contracts::NumericContract;
    using Semantic=signal_contracts::Semantic;
    AudioNodeModel model;
    domain::ConfiguredProcessingNode node;
    std::unique_ptr<QWidget> card;
    QLabel* exact=nullptr;QLabel* status=nullptr;
    PreviewPools pools;
    void foreign_and_unavailable(const pipeline::DeliveredNumericFrame& frame,const pipeline::DeliveredNumericFrame& stale){
    auto foreign=frame;auto wrong=std::make_shared<pipeline::NumericDeliveryContext>(*frame.context);wrong->endpoint.node=13;foreign.context=wrong;
    require(!model.preview(foreign) && exact->text()=="18446744073709551615","Foreign endpoint changed preview");
    auto unavailable=frame;unavailable.status=Status::unavailable;require(model.preview(unavailable) && exact->text()==QStringLiteral("—"),"Unavailable frame retained stale value");
    require(!model.preview(stale),"Unavailable discarded sequence fence");
    }
    void sequence_fences(){
        auto frame=scalar_delivery(pools,node,{3,5},std::numeric_limits<std::uint64_t>::max(),8);const auto snapshot=model.save();
    require(model.preview(frame) && exact->text()=="18446744073709551615","Preview rounds UInt64 maximum");
    require(model.save()==snapshot,"Diagnostic changed configured parameters");
    auto stale=scalar_delivery(pools,node,{3,5},7,7);require(!model.preview(stale) && exact->text()=="18446744073709551615","Late frame replaced newer exact value");
    foreign_and_unavailable(frame,stale);
    model.clear_preview();frame={};stale={};
    require(pools.p.in_use()==0 && pools.m.in_use()==0,"Preview retains pooled frame storage");
    }
    void preferences(){
    auto next=scalar_delivery(pools,node,{3,5},9007199254740993ULL,9);require(model.preview(next) && exact->text()=="9007199254740993","Preview loses integer above binary64 exact range");
    for(const auto& theme:{QString("light"),QString("dark")}){
        require(ui::Theme::instance().set(theme),"Theme switch refused");QApplication::processEvents();
        require(card->palette().color(QPalette::WindowText)==QApplication::palette().color(QPalette::WindowText),"Preview breaks inherited palette");
        require(exact->text()=="9007199254740993","Theme mutates diagnostic value");
    }
    require(ui::Language::instance().set("ru"),"RU language unavailable");require(status->text()==QString::fromUtf8("Входной порт 0 · Данные доступны"),"Live preview status not translated");
    require(card->findChild<QLabel*>("previewUnit")->text().contains(QString::fromUtf8("Входной порт")),"Live endpoint label not translated");
    require(ui::Language::instance().set("en"),"EN language unavailable");require(status->text()=="Input port 0 · Available","Preview fails live EN language update");
    }
    void bypass(){
    node.enabled=false;model.set_numeric_node(node);require(status->text()=="Input port 0 · Bypass · Waiting for data" && exact->text()==QStringLiteral("—"),"Bypass transition reused stale diagnostics");
    auto bypassZero=scalar_delivery(pools,node,{3,5},0,10);
    require(model.preview(bypassZero) && status->text()=="Input port 0 · Bypass · Available" && exact->text()=="0","Bypass must preview actual typed-zero output instead of reporting a stopped node");
    auto bypassUnavailable=bypassZero;bypassUnavailable.status=Status::unavailable;
    require(model.preview(bypassUnavailable) && status->text()=="Input port 0 · Bypass · Unavailable" && exact->text()==QStringLiteral("—"),"Bypass must preserve actual unavailability diagnostics");
    bypassZero={};bypassUnavailable={};
    node.enabled=true;model.set_numeric_node(node);require(exact->text()==QStringLiteral("—"),"Reenable reused old diagnostic");
    }
    void passive_tag(){
    auto tagged=std::get<NumericContract>(node.inputs[0].contract.view());tagged.semantic=Semantic::History;
    require(numeric_editor::set_contract(node,domain::PortDirection::Input,0,tagged),"Passive preview tag fixture");model.set_numeric_node(node);
    auto taggedScalar=scalar_delivery(pools,node,{3,5},42,11);
    require(model.preview(taggedScalar) && exact->text()=="42","A passive History tag must not suppress a physical scalar preview");taggedScalar={};
    }
public:
    ScalarPreviewChecks():model("ScalarView"),node(*model.numeric_node()){
        node.id=12;
    require(numeric_editor::set_dtype(node,domain::PortDirection::Input,0,DType::UInt64),"UInt64 scalar contract");model.set_numeric_node(node);
    card.reset(model.preview_widget());exact=card->findChild<QLabel*>("previewExactValue");status=card->findChild<QLabel*>("previewStatus");
    }
    void run(){sequence_fences();preferences();bypass();passive_tag();}
};
static void encode_history_coordinate(double value,std::span<std::byte> bytes){
    const auto bits=std::bit_cast<std::uint64_t>(value);
    for(unsigned byte=0;byte<8;++byte)bytes[byte]=std::byte((bits>>(8*byte))&255);
}
struct PreviewHistoryCoordinates {
    std::array<std::byte,32> times{};
    std::array<std::byte,50*8> frequencies{};
    PreviewHistoryCoordinates(){
        for(std::size_t rowIndex=1;rowIndex<4;++rowIndex)encode_history_coordinate(double(77+rowIndex)/60.,std::span(times).subspan(rowIndex*8,8));
        for(std::size_t band=0;band<50;++band)encode_history_coordinate(double(band+1)*100.,std::span(frequencies).subspan(band*8,8));
    }
    signal_contracts::CoordinateView view(bool compact)const{
        signal_contracts::CoordinateView result;
        if(!compact)result.axes[0]=times;
        result.axes[compact?0:1]=frequencies;
        return result;
    }
};
class HistoryPreviewChecks {
    using NumericContract=signal_contracts::NumericContract;
    using Status=signal_contracts::Status;
    PreviewPools pools;
    AudioNodeModel history;
    std::unique_ptr<QWidget> historyCard;
    std::shared_ptr<pipeline::NumericDeliveryContext> context;
    pipeline::DeliveredNumericHistoryRow row;
    static void configure_frequency_input(domain::ConfiguredProcessingNode& node){
        using namespace signal_contracts;
        auto input=std::get<NumericContract>(node.inputs[0].contract.view());
        input.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};
        require(domain::own_processing_contract(input,numeric_editor::limits().frame,2048,node.inputs[0].contract)==Status::ok,"History fixture frequency input contract");
    }
    void configure_contract(){
        using namespace signal_contracts;
        auto h=*history.numeric_node();h.id=14;
        configure_frequency_input(h);
    auto historyContract=std::get<NumericContract>(h.outputs[0].contract.view());historyContract.shape.extents[0].maximum=4;
    historyContract.coordinates[1]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};
    require(domain::own_processing_contract(historyContract,numeric_editor::limits().frame,2048,h.outputs[0].contract)==Status::ok,"Bounded History preview fixture contract");h.parameters=domain::HistoryProcessingParameters{4};history.set_numeric_node(h);historyCard.reset(history.preview_widget());
        require(historyContract.coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && historyContract.coordinates[0].unit==NumericUnit::Seconds,"History fixture must preserve the explicit source-clock contract");
    context=std::make_shared<pipeline::NumericDeliveryContext>();context->endpoint={"main",14,domain::PortDirection::Output,0};context->contract=history.numeric_node()->outputs[0].contract;
    row.compact.context=context;row.compact.delivery={3,5};row.event=pipeline::NumericHistoryEvent::Empty;
    row.capacity=std::get<NumericContract>(context->contract.view()).shape.extents[0].maximum;
    }
    void event_checks(){
    require(history.preview(row) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("0\n—"),"Empty history count not available zero");
    row.filled_count=row.capacity+1;require(!history.preview(row),"Invalid filled count accepted");
    row.filled_count=0;row.event=pipeline::NumericHistoryEvent::Unavailable;require(history.preview(row) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("—\n—"),"Unavailable history retained count");
    require(historyCard->findChild<QLabel*>("previewStatus")->text().split('\n').size()==2 && !historyCard->findChild<QLabel*>("previewStatus")->text().contains("Input port"),"Processor preview advertises unsupported input callback");
    }
    NumericContract retained_contract(bool compact)const{
        using namespace signal_contracts;
        auto contract=std::get<NumericContract>(context->contract.view());
        if(compact){contract.semantic=contract.history_element;contract.history_element=Semantic::Numeric;contract.shape={Rank::Vector,{contract.shape.axes[1],Axis::None},{contract.shape.extents[1],ExtentBounds{}}};contract.coordinates={contract.coordinates[1],AxisCoordinates{}};}
        return contract;
    }
    static signal_contracts::FrameStamp retained_stamp(bool compact,std::uint64_t sequence){
        signal_contracts::FrameStamp stamp;
        stamp.producer_epoch=701;stamp.generation=17;stamp.sequence=sequence;stamp.epoch=3;stamp.graph_revision=5;stamp.timestamp_ticks=compact?64800:64000;
        return stamp;
    }
    pipeline::DeliveredNumericFrame retained_history(bool compact,std::uint64_t sequence){
        using namespace signal_contracts;
        auto contract=retained_contract(compact);
        std::array<float,200> values{};values.fill(7);
        if(!compact)std::fill_n(values.begin(),50,0.f);
        PlaneLayout plane{0,compact?std::array<std::size_t,2>{4,0}:std::array<std::size_t,2>{200,4}};
        PreviewHistoryCoordinates coordinates;
        DataFrameView input{retained_stamp(compact,sequence),NumericView{&contract,std::as_bytes(std::span(values)),std::span(&plane,1),compact?std::array<std::size_t,2>{50,1}:std::array<std::size_t,2>{4,50},compact?RingRows{}:RingRows{4,0},coordinates.view(compact)}};
        pipeline::DeliveredNumericFrame result{context,{3,5},{},Status::ok};
        require(retain_frame(input,numeric_editor::limits().frame,pools.p,pools.m,result.frame)==Status::ok,"Cold History fixture retention");
        return result;
    }
    void continue_compact(pipeline::DeliveredNumericFrame& full,const QJsonObject& historyConfigured){
    row.compact=retained_history(true,801);row.event=pipeline::NumericHistoryEvent::Row;row.filled_count=4;row.accepted_row_ordinal=81;
    row.has_time_coordinate=true;encode_history_coordinate(81./60.,row.time_coordinate);
    require(history.preview(row) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("4\n—"),"Compact History does not continue after cold snapshot");
    require(!history.preview_history_snapshot(full,80,3) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("4\n—"),"Late cold snapshot replaces newer compact ordinal");
    require(history.save()==historyConfigured,"Cold History preview mutates parameters");full={};row.compact={};require(pools.p.in_use()==0 && pools.m.in_use()==0,"Cold History preview retains pooled storage");
    }
    void cold_snapshot_checks(){
        const auto historyConfigured=history.save();
    auto full=retained_history(false,800);const auto beforeCold=historyCard->findChild<QLabel*>("previewValues")->text();const auto retainedPayload=pools.p.in_use(),retainedMetadata=pools.m.in_use();
    const auto retainedTimes=std::get<signal_contracts::NumericView>(full.frame.view().payload).coordinates.axes[0];
    require(retainedTimes.size()==32 && pipeline::decode_coordinate(retainedTimes.subspan(0,8))==0 && pipeline::decode_coordinate(retainedTimes.subspan(8,8))==78./60. && pipeline::decode_coordinate(retainedTimes.subspan(24,8))==80./60.,"Cold History fixture must own individual real row times after its donor expires");
    require(!history.preview_history_snapshot(full,2,3) && historyCard->findChild<QLabel*>("previewValues")->text()==beforeCold && history.save()==historyConfigured && pools.p.in_use()==retainedPayload && pools.m.in_use()==retainedMetadata,"Malformed cold ordinal changes preview/count/parameters/storage");
    require(history.preview_history_snapshot(full,80,3) && historyCard->findChild<QLabel*>("previewValues")->text()==QStringLiteral("3\n—"),"Cold History snapshot count unavailable");
        continue_compact(full,historyConfigured);
    }
public:
    HistoryPreviewChecks():history("Spectrogram"){configure_contract();}
    void run(){event_checks();cold_snapshot_checks();}
};
static void preview_model_checks(){
    ScalarPreviewChecks scalar;scalar.run();
    HistoryPreviewChecks history;history.run();
}
static void collect_catalogue_leaves(QTreeWidgetItem* item,QStringList& result){
    const auto kind=item->data(0,Qt::UserRole).toString();if(!kind.isEmpty())result.append(kind);
    for(int i=0;i<item->childCount();++i)collect_catalogue_leaves(item->child(i),result);
}
static QStringList catalogue_leaves(QTreeWidget& tree){
    QStringList result;
    for(int i=0;i<tree.topLevelItemCount();++i)collect_catalogue_leaves(tree.topLevelItem(i),result);
    result.sort();return result;
}
static void widget_catalogue_checks(SchemeEditorWidget& editor){
    auto* library=editor.findChild<QTreeWidget*>("pipelineNodeLibrary");require(library && library->topLevelItemCount()==4,"Sidebar must expose sources, processors, outputs and graphs");
    auto* scene=editor.findChild<QtNodes::DataFlowGraphicsScene*>();require(scene,"Editor scene absent");
    std::unique_ptr<QMenu> menu(scene->createSceneMenu({9000,9000}));
    auto* context=menu->findChild<QTreeWidget*>("numericContextCatalog");require(context && context->topLevelItemCount()==4,"Context catalog must expose the same four root sections");
    auto expected=numeric_editor::visible_catalog();expected.sort();
    require(catalogue_leaves(*context)==catalogue_leaves(*library) && catalogue_leaves(*library)==expected,"Sidebar/context must expose every public catalog leaf in the shared hierarchy");
}
static void connection_policy_checks(SchemeEditorWidget& editor,NumericEditorGraphModel& graph){
    const auto source=graph.addNode("AudioSource"),sink=graph.addNode("Converter");auto node=*graph.delegateModel<AudioNodeModel>(sink)->numeric_node();
    numeric_editor::set_dtype(node,domain::PortDirection::Input,0,signal_contracts::DType::UInt16);graph.edit_node(sink,node);
    const QtNodes::ConnectionId edge{source,0,sink,0};graph.addConnection(edge);
    auto* scene=editor.findChild<QtNodes::DataFlowGraphicsScene*>();scene->connectionGraphicsObject(edge)->setSelected(true);
    auto* policy=editor.findChild<QComboBox*>("connectionConversionPolicy");require(policy && policy->currentIndex()==0 && editor.findChild<QLabel*>("connectionRisk"),"Selecting a dtype edge must expose its actual policy in the existing inspector");
    policy->setCurrentIndex(3);require(graph.connection_policy(edge)==3,"Inspector policy must edit the processing graph");
    editor.history().undo();require(graph.connection_policy(edge)==0 && policy->currentIndex()==0,"Inspector policy undo must refresh both representations");
    graph.deleteNode(sink);graph.deleteNode(source);
}
static domain::NumericProcessingGraph pinned_capture_graph(){
    domain::NumericProcessingGraph graph;graph.graph_id="main";
    auto source=numeric_editor::default_node("AudioSource");source.id=1;
    auto view=numeric_editor::default_node("AudioView");view.id=2;view.inputs[0].contract=source.outputs[0].contract;
    graph.nodes={source,view};graph.edges={{1,2,0,0}};QString error;
    require(desktop::resolve_numeric_source_format(graph,1,44100,3,numeric_editor::limits(),&error),"Prepare saved three-output contract");
    graph.edges[0].output_port=2;return graph;
}
static void rejected_pinned_import(SchemeEditorWidget& editor,NumericEditorGraphModel& model,const domain::NumericProcessingGraph& graph,const QJsonObject& adapters){
    const auto before=editor.document();const auto undoIndex=editor.history().index(),undoCount=editor.history().count();QString error;
    require(!editor.load_numeric_draft(graph,&error,adapters) && editor.document()==before && editor.history().index()==undoIndex && editor.history().count()==undoCount && model.delegateModel<AudioNodeModel>(1)->adapter().value("device_identity")=="saved-three","Rejected widget pinned import changed graph, bindings or Undo");
}
static void pinned_capture_import(SchemeEditorWidget& editor,NumericEditorGraphModel& model){
    auto devices=std::make_shared<QJsonArray>(QJsonArray{QJsonObject{{"id","default-mono"},{"is_default",true},{"native_sample_rate",48000},{"native_channels",1}},QJsonObject{{"id","saved-three"},{"is_default",false},{"native_sample_rate",44100},{"native_channels",3}}});
    model.set_admission_limits(numeric_editor::limits());model.set_capture_device_provider([devices](int,QString*){return *devices;});
    const auto graph=pinned_capture_graph();const QJsonObject adapters{{"1",QJsonObject{{"device_identity","saved-three"}}}};QString error;
    require(editor.load_numeric_draft(graph,&error,adapters),"Atomic widget pinned graph and adapter import");
    domain::NumericProcessingGraph applied;
    require(editor.numeric_draft(applied,&error) && applied.edges==graph.edges && domain::data_outputs(applied.nodes[0]).size()==3 && model.delegateModel<AudioNodeModel>(1)->adapter().value("device_identity")=="saved-three","Widget load rewired pinned output three to default mono");
    (*devices)[1]=QJsonObject{{"id","saved-three"},{"is_default",false},{"native_sample_rate",999},{"native_channels",3}};
    rejected_pinned_import(editor,model,graph,adapters);model.set_admission_limits(numeric_editor::limits());model.set_capture_device_provider({});
}
static void inspector_bypass_translation(QCheckBox& bypass){
    ui::Language::instance().set("ru");require(bypass.text()==ui::text("Bypass"),"Inspector bypass is not live translated");
    ui::Language::instance().set("en");require(bypass.text()==ui::text("Bypass"),"Inspector bypass does not restore English");
}
static void connected_bypass_undo(SchemeEditorWidget& editor,NumericEditorGraphModel& graph){
    auto node=numeric_editor::default_node("Converter");node.id=42;
    domain::NumericProcessingGraph draft;draft.graph_id="main";draft.nodes.push_back(node);
    require(editor.load_numeric_draft(draft),"Identity bypass fixture load");
    auto* model=graph.delegateModel<AudioNodeModel>(42);std::unique_ptr<QWidget> controls(model->parameter_widget());
    auto* bypass=controls->findChild<QCheckBox*>("numericBypass");require(bypass,"Identity operation inspector bypass absent");
    const int before=editor.history().index();bypass->setChecked(true);
    require(!model->numeric_node()->enabled && editor.history().index()==before+1,"Bypass edit bypasses editor command/undo path");
    editor.history().undo();require(model->numeric_node()->enabled && !bypass->isChecked(),"Undo fails to restore bypass control");
    editor.history().redo();require(!model->numeric_node()->enabled && bypass->isChecked(),"Redo fails to restore bypass control");
    editor.history().undo();inspector_bypass_translation(*bypass);
}
class CaptureStatusChecks {
    SchemeEditorWidget& editor_;GuiPresenter& presenter_;QLabel* capture_;QLabel* validation_;
    QJsonObject before_;int undo_;QString warning_;
public:
    CaptureStatusChecks(SchemeEditorWidget& editor,GuiPresenter& presenter):editor_(editor),presenter_(presenter),
        capture_(editor.toolbar()->findChild<QLabel*>("pipelineCaptureStatus")),validation_(editor.toolbar()->findChild<QLabel*>("pipelineValidation")),before_(editor.document()),undo_(editor.history().index()) {
        require(capture_ && validation_,"Capture status indicator absent");
    }
    void failure(){
        emit presenter_.apply_status_changed("Not applied: fixture contract");warning_=validation_->toolTip();
        emit presenter_.component_status_changed("capture",int(pipeline::State::Failed),"fixture: loopback device unavailable");
        require(capture_->isVisibleTo(editor_.toolbar()) && capture_->text()==ui::text("Capture failed") && capture_->toolTip().contains("fixture: loopback device unavailable"),"Projected capture failure is not visible with backend error");
        emit presenter_.apply_status_changed("Applied");emit presenter_.component_status_changed("analyzer",int(pipeline::State::Running),{});
        require(capture_->text()==ui::text("Capture failed") && validation_->toolTip()==warning_,"Apply or analyzer status masks capture/admission failure");
    }
    void recovery(){
        ui::Language::instance().set("ru");require(capture_->text()==QString::fromUtf8("Ошибка захвата") && capture_->toolTip().contains("fixture: loopback device unavailable"),"Live capture failure is not translated to RU");
        emit presenter_.component_status_changed("capture",int(pipeline::State::Starting),{});
        require(capture_->text()==ui::text("Capture starting") && !capture_->toolTip().contains("fixture:"),"Capture starting retains stale failure");
        emit presenter_.component_status_changed("capture",int(pipeline::State::Running),{});
        require(capture_->text()==ui::text("Audio capture: receiving") && validation_->toolTip()==warning_,"Capture recovery overwrites admission warning");
        ui::Language::instance().set("en");require(capture_->text()=="Audio capture: receiving","Live capture recovery fails EN refresh");
    }
    void inactive(){
        emit presenter_.component_status_changed("capture",int(pipeline::State::Stopped),{});
        require(capture_->isVisibleTo(editor_.toolbar()) && capture_->text()==ui::text("Audio capture: inactive") && !capture_->toolTip().isEmpty() && !capture_->toolTip().contains("fixture:") && capture_->textInteractionFlags()==Qt::NoTextInteraction,"Inactive capture status loses independence or visibility");
        require(validation_->toolTip()==warning_ && editor_.document()==before_ && editor_.history().index()==undo_,"Capture status mutates draft/history/admission warning");
    }
};
static void capture_status_checks(SchemeEditorWidget& editor,GuiPresenter& presenter){
    CaptureStatusChecks checks(editor,presenter);checks.failure();checks.recovery();checks.inactive();
}
static domain::NumericProcessingGraph exact_converter_graph(){
    auto converter=numeric_editor::default_node("Converter");converter.id=3;
    std::get<domain::ConverterProcessingParameters>(converter.parameters).policy.nan_replacement=std::uint64_t{9007199254740993ULL};
    domain::NumericProcessingGraph graph;graph.graph_id="main";graph.nodes.push_back(converter);return graph;
}
static void clipboard_shortcuts(QtNodes::GraphicsView& view){
    int actions=0;
    for(auto* action:view.actions())if(action->shortcuts().contains(QKeySequence(QKeySequence::Copy)) || action->shortcuts().contains(QKeySequence(QKeySequence::Paste))){
        ++actions;require(action->shortcutContext()==Qt::WidgetWithChildrenShortcut && !action->autoRepeat(),"Graph clipboard shortcuts escape the focused view or repeat");
    }
    require(actions==2,"Graph Ctrl+C/V shortcuts were remapped or missing");
}
class ExactDocumentChecks {
    SchemeEditorWidget& editor;NumericEditorGraphModel& graph;GuiPresenter& presenter;GuiState& state;
    QSignalSpy& typedSave;QSignalSpy& applied;
    domain::NumericProcessingGraph draft=exact_converter_graph();
    std::unique_ptr<QWidget> settings;QLineEdit* rate=nullptr;signal_contracts::Ratio interval;QString preset;
public:
    ExactDocumentChecks(SchemeEditorWidget& e,NumericEditorGraphModel& g,GuiPresenter& p,GuiState& s,QSignalSpy& saves,QSignalSpy& replies)
        :editor(e),graph(g),presenter(p),state(s),typedSave(saves),applied(replies){}
    void load(){
        QString error;require(editor.load_numeric_draft(draft,&error),"Widget canonical load failed");domain::NumericProcessingGraph exported;
        require(editor.numeric_draft(exported,&error) && exported.nodes.size()==1 && exported.nodes[0].id==3,"Widget export lost owning node/id");
        require(!editor.toolbar()->findChild<QLineEdit*>("pipelineAnalysisRate") && editor.toolbar()->findChild<QLabel*>("pipelineProcessingRate"),"Toolbar must show measured rate, with editable target in settings only");
        auto* status=editor.toolbar()->findChild<QLabel*>("pipelineCaptureStatus");
        require(status && !status->toolTip().isEmpty() && status->textInteractionFlags()==Qt::NoTextInteraction,"Capture must be a descriptive noninteractive status");
        settings.reset(editor.execution_settings_widget());rate=settings->findChild<QLineEdit*>("pipelineAnalysisRate");require(rate,"Execution settings missing rational rate editor");
        interval=graph.numeric_graph().execution_interval;
    }
    void rational_rate(){
        const auto before=editor.history().index();rate->setText("24000/1001");emit rate->editingFinished();
        require(graph.numeric_graph().execution_interval.numerator==1001 && graph.numeric_graph().execution_interval.denominator==24000 && editor.history().index()==before+1,"Rational analysis rate loses exact numerator/denominator or undo");
        editor.history().undo();require(graph.numeric_graph().execution_interval==interval,"Rate settings undo fails");
        rate->setText("120/0");emit rate->editingFinished();require(graph.numeric_graph().execution_interval==interval && editor.history().index()==before,"Malformed settings rate changes graph");
        auto sixty=draft;sixty.execution_interval={1,60};require(editor.load_numeric_draft(sixty) && rate->text()=="60","Loading a graph leaves existing rate settings on previous target");
        require(editor.load_numeric_draft(draft) && rate->text()=="120","Restoring graph does not synchronize persistent rate settings");
    }
    void rejected_load(){
        const auto before=editor.document();auto invalid=draft;invalid.nodes[0].kind="UnknownNumericNode";QString error;
        require(!editor.load_numeric_draft(invalid,&error) && editor.document()==before,"Rejected load changed current draft");
    }
    void inspector_order(QPushButton* rename){
        auto* inspector=editor.findChild<QWidget*>("pipelineInspector");require(inspector,"Node inspector absent");
        auto* preview=inspector->findChild<QWidget*>("pipelineDataStatus");auto* form=inspector->findChild<QWidget*>("numericNodeParameters");
        require(preview && form,"Inspector data availability or parameter form missing");
        auto* owner=preview->parentWidget();
        auto* parameters=form;
        while(parameters && parameters->parentWidget()!=owner)parameters=parameters->parentWidget();
        require(parameters && owner->layout(),"Inspector parameter wrapper missing from preview layout");
        const int previewIndex=owner->layout()->indexOf(preview),parameterIndex=owner->layout()->indexOf(parameters),renameIndex=owner->layout()->indexOf(rename);
        require(previewIndex>=0 && previewIndex<parameterIndex && parameterIndex<renameIndex,"Inspector data availability and parameters must precede rename/options");
    }
    void clipboard(){
        auto* view=editor.findChild<QtNodes::GraphicsView*>();require(view,"Graph clipboard view missing");clipboard_shortcuts(*view);
        auto* text=editor.findChild<QLineEdit*>("numericNan");require(text,"Exact numeric text editor absent");
        const auto count=graph.allNodeIds().size();text->setFocus();text->selectAll();QTest::keyClick(text,Qt::Key_C,Qt::ControlModifier);
        require(QApplication::clipboard()->text()=="9007199254740993" && graph.allNodeIds().size()==count,"Copy inside a parameter field copied graph nodes instead of text");
    }
    void rename(){
        graph.setNodeData(3,QtNodes::NodeRole::Position,QPointF(420,230));auto* model=graph.delegateModel<AudioNodeModel>(3);
        require(model->set_parameter_on_block("numericNan",true),"Preset visibility edit failed");
        editor.findChild<QtNodes::DataFlowGraphicsScene*>()->nodeSelected(3);auto* button=editor.findChild<QPushButton*>("pipelineRenameNode");require(button,"Ordinary processor inspector lacks rename control");
        inspector_order(button);clipboard();rename_command(*model,*button);
    }
    void rename_command(AudioNodeModel& model,QPushButton& button){
        const auto before=editor.history().index();
        QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)){dialog->setTextValue("Exact converter");dialog->accept();}});
        button.click();require(model.caption()=="Exact converter" && model.name()=="Converter" && editor.history().index()==before+1,"Inspector rename did not update user caption with one undo command");
        editor.history().undo();require(model.caption()!="Exact converter","Inspector rename undo did not restore default");editor.history().redo();
    }
    void reload_preset(){
        require(editor.save_preset("exact snapshot"),"Canonical preset save failed");preset=state.file_configs.ui.pipeline_presets.last().toObject()["id"].toString();
        rate->setText("90");emit rate->editingFinished();graph.deleteNode(3);require(editor.load_preset(preset),"Canonical preset reload failed");
        require(rate->text()=="120" && graph.numeric_graph().execution_interval==interval,"Preset replacement leaves stale persistent analysis target");
        verify_preset();
    }
    void verify_preset(){
        domain::NumericProcessingGraph exported;QString error;require(editor.numeric_draft(exported,&error),qPrintable(QString("Preset export failed: ")+error));
        const auto& replacement=std::get<domain::ConverterProcessingParameters>(exported.nodes[0].parameters).policy.nan_replacement;
        require(std::holds_alternative<std::uint64_t>(replacement),"Preset changes UInt64 literal tag");
        require(std::get<std::uint64_t>(replacement)==9007199254740993ULL,qPrintable(QString("Preset rounds exact UInt64 literal to ")+QString::number(std::get<std::uint64_t>(replacement))));
        require(graph.nodeData(3,QtNodes::NodeRole::Position).toPointF()==QPointF(420,230),"Preset loses node position");
        require(graph.delegateModel<AudioNodeModel>(3)->parameter_on_block("numericNan"),"Preset loses presentation flag");
        require(graph.delegateModel<AudioNodeModel>(3)->caption()=="Exact converter","Preset loses the user display name");
    }
    void inject_typed_extensions() {
        QByteArray encoded;
        require(bool(desktop::encode_numeric_graph(graph.numeric_graph(),encoded)), "Typed extension fixture encode failed");
        auto document=QJsonDocument::fromJson(encoded).object();
        auto nodes=document.value("nodes").toArray();
        auto node=nodes[0].toObject();
        const auto visual=editor.document().value("nodes").toArray()[0].toObject();
        node["position"]=visual.value("position");
        node["presentation"]=visual.value("internal-data").toObject().value("presentation");
        node["future_node"]=QJsonObject{{"value",42}};
        nodes[0]=node;
        document["nodes"]=nodes;
        document["future_graph"]=QJsonObject{{"value","retained"}};
        auto entries=state.file_configs.ui.pipeline_presets;
        auto saved=entries.last().toObject();
        saved["graph"]=document;
        entries[entries.size()-1]=saved;
        state.file_configs.ui.pipeline_presets=entries;
    }
    void typed_extensions() {
        inject_typed_extensions();
        require(editor.load_preset(preset),"Typed extension preset load failed");
        const auto saved=editor.document();
        const auto node=saved.value("nodes").toArray()[0].toObject();
        require(saved.value("future_graph").toObject().value("value")=="retained","Typed import loses graph extension");
        require(node.value("future_node").toObject().value("value")==42,"Typed import loses node extension");
        require(!node.contains("kind") && node.value("internal-data").toObject().contains("numeric_processing"),"Canonical processing fields polluted visual projection");
        verify_preset();
    }

    void apply(){
        applied.clear();editor.findChild<QPushButton*>("applyPipeline")->click();
        for(int i=0;i<100 && applied.isEmpty();++i)QTest::qWait(10);
        require(!applied.isEmpty() && applied.last()[1].toBool() && typedSave.count()>=1,"Canonical Apply not acknowledged by executor");
        require(!QJsonDocument(presenter.client().snapshot().graph).toJson().contains("inline_parameters"),"Presentation flags reached engine Apply snapshot");
        domain::NumericProcessingGraph actual;require(bool(desktop::decode_numeric_editor_graph(QJsonDocument(presenter.client().snapshot().graph).toJson(QJsonDocument::Compact),actual)) && actual.nodes.size()==1 && actual.nodes[0].id==3,"Apply did not reach canonical engine snapshot");
        graph.deleteNode(3);editor.flush_presentation();require(graph.has_numeric_nodes() && typedSave.count()>=2,"Empty draft drops owning save notification");
        require(editor.remove_preset(preset),"Preset removal failed");
    }
};
static void exact_document_checks(SchemeEditorWidget& editor,NumericEditorGraphModel& graph,GuiPresenter& presenter,GuiState& state,QSignalSpy& typedSave,QSignalSpy& applied){
    ExactDocumentChecks checks(editor,graph,presenter,state,typedSave,applied);
    checks.load();checks.rational_rate();checks.rejected_load();checks.rename();checks.reload_preset();checks.typed_extensions();checks.apply();
}
static domain::NumericProcessingGraph diagnostic_graph(){
    auto scalar=numeric_editor::default_node("ScalarView");scalar.id=12;
    require(numeric_editor::set_dtype(scalar,domain::PortDirection::Input,0,signal_contracts::DType::UInt64),"Diagnostic UInt64 input");
    domain::NumericProcessingGraph graph;graph.graph_id="main";graph.nodes.push_back(scalar);return graph;
}
class PreviewLifecycleChecks {
    SchemeEditorWidget& editor;NumericEditorGraphModel& graph;GuiPresenter& presenter;QSignalSpy& applied;
    domain::NumericProcessingGraph diagnostic=diagnostic_graph();AudioNodeModel* model=nullptr;
    std::unique_ptr<QWidget> card;QLabel* exact=nullptr;pipeline::EngineSnapshot snapshot;QJsonObject parameters;
    // Frame handles die before their arena; the fixture owns both across fences.
    PreviewPools pools;pipeline::DeliveredNumericFrame frame;
public:
    PreviewLifecycleChecks(SchemeEditorWidget& e,NumericEditorGraphModel& g,GuiPresenter& p,QSignalSpy& replies):editor(e),graph(g),presenter(p),applied(replies){}
    void load(){
        require(editor.load_numeric_draft(diagnostic),"Diagnostic graph load");applied.clear();editor.findChild<QPushButton*>("applyPipeline")->click();
        for(int i=0;i<100 && applied.isEmpty();++i)QTest::qWait(10);
        require(!applied.isEmpty() && applied.last()[1].toBool(),"Diagnostic graph Apply failed");
        model=graph.delegateModel<AudioNodeModel>(12);card.reset(model->preview_widget());exact=card->findChild<QLabel*>("previewExactValue");require(exact,"Registered diagnostic preview value absent");
        editor.findChild<QtNodes::DataFlowGraphicsScene*>()->nodeSelected(12);
    }
    void publish(){
        for(int i=0;i<200 && presenter.client().snapshot().statuses.value("analyzer").state!=pipeline::State::Running;++i)QTest::qWait(10);
        require(presenter.client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,"Preview fixture must run actual current graph");
        snapshot=presenter.client().snapshot();frame=scalar_delivery(pools,diagnostic.nodes[0],{snapshot.graph_revision,snapshot.epoch},9007199254740993ULL,1);
        emit presenter.numeric_frame_ready(frame);require(exact->text()=="9007199254740993","Connected presenter signal fails inline preview");parameters=model->save();
    }
    void visibility(){
        editor.hide();require(exact->text()==QStringLiteral("—"),"Hidden editor retains available stale diagnostic");
        emit presenter.numeric_frame_ready(frame);require(exact->text()==QStringLiteral("—"),"Hidden editor consumes numeric delivery");editor.show();
        emit presenter.numeric_frame_ready(frame);require(exact->text()=="9007199254740993","Reopened editor fails current delivery");
    }
    void revision_fence(){
        presenter.apply_pipeline_graph(editor.document());
        for(int i=0;i<200 && (presenter.client().snapshot().graph_revision==snapshot.graph_revision || presenter.graph_request_pending());++i)QTest::qWait(10);
        QCoreApplication::processEvents();
        require(presenter.client().snapshot().graph_revision>snapshot.graph_revision && presenter.client().snapshot().statuses.value("analyzer").state==pipeline::State::Running,"Apply did not advance the continuous graph");
        emit presenter.numeric_frame_ready(frame);require(exact->text()==QStringLiteral("—"),"New graph revision accepts stale preview frame");
    }
    void divergent_draft(){
        auto changed=*model->numeric_node();changed.enabled=false;require(graph.edit_node(12,changed),"Draft parameter edit failed");changed.enabled=true;require(graph.edit_node(12,changed),"Draft reenable failed");
        auto alternate=*model->numeric_node();numeric_editor::set_dtype(alternate,domain::PortDirection::Input,0,signal_contracts::DType::Int64);require(graph.edit_node(12,alternate),"Diverging contract edit failed");
        emit presenter.numeric_frame_ready(frame);require(exact->text()==QStringLiteral("—"),"Divergent draft displays active graph data");require(parameters!=model->save(),"Fixture draft edit failed to change typed snapshot");
    }
    void foreign_graph(){
        auto bad=frame;auto context=std::make_shared<pipeline::NumericDeliveryContext>(*frame.context);context->endpoint.graph_id="foreign";bad.context=context;
        emit presenter.numeric_frame_ready(bad);require(exact->text()==QStringLiteral("—"),"Foreign graph populates inline preview");
        std::unique_ptr<QWidget> controls(model->parameter_widget());require(!controls->findChild<QCheckBox*>("numericBypass"),"Scalar sink must not expose identity bypass");
    }
};
static void editor_layout_checks(SchemeEditorWidget&,NumericEditorGraphModel&);
static void preview_lifecycle_checks(SchemeEditorWidget& editor,NumericEditorGraphModel& graph,GuiPresenter& presenter,QSignalSpy& applied){
    PreviewLifecycleChecks checks(editor,graph,presenter,applied);
    checks.load();checks.publish();checks.visibility();checks.revision_fence();checks.divergent_draft();checks.foreign_graph();
    editor_layout_checks(editor,graph);
}
static QJsonValue processing_snapshot(SchemeEditorWidget& editor){return editor.document().value("nodes").toArray().first().toObject().value("internal-data").toObject().value("numeric_processing");}
static void ordinary_layout_checks(SchemeEditorWidget& editor,NumericEditorGraphModel& graph,QtNodes::GraphicsView& view){
    const auto position=graph.nodeData(12,QtNodes::NodeRole::Position).toPointF();const auto scale=view.getScale();
    editor.hide();editor.show();QApplication::processEvents();
    require(graph.nodeData(12,QtNodes::NodeRole::Position).toPointF()==position && view.getScale()==scale,"Ordinary show resets explicit layout");
}
static void one_time_layout_checks(SchemeEditorWidget& editor,NumericEditorGraphModel& graph,QtNodes::GraphicsView& view){
    editor.hide();graph.setNodeData(12,QtNodes::NodeRole::Position,QPointF(9000,7000));editor.initialize_default_layout();QApplication::processEvents();
    require(graph.nodeData(12,QtNodes::NodeRole::Position).toPointF()==QPointF(9000,7000),"Default layout runs before editor viewport visible");
    const auto before=processing_snapshot(editor);editor.show();QApplication::processEvents();const auto arranged=graph.nodeData(12,QtNodes::NodeRole::Position).toPointF();
    require(arranged!=QPointF(9000,7000) && processing_snapshot(editor)==before,"One-time default layout missing or mutates processing");
    auto* scene=editor.findChild<QtNodes::DataFlowGraphicsScene*>();
    require(QLineF(view.mapToScene(view.viewport()->rect().center()),scene->itemsBoundingRect().center()).length()<5/view.getScale(),"Default fit fails to center ready viewport");
}
static void replacement_layout_checks(SchemeEditorWidget& editor,NumericEditorGraphModel& graph){
    graph.setNodeData(12,QtNodes::NodeRole::Position,QPointF(111,222));editor.hide();editor.show();QApplication::processEvents();
    require(graph.nodeData(12,QtNodes::NodeRole::Position).toPointF()==QPointF(111,222),"Default layout repeats and destroys user positions");
    editor.hide();editor.initialize_default_layout();require(editor.load_numeric_draft(diagnostic_graph()),"Explicit typed replacement rejected");graph.setNodeData(12,QtNodes::NodeRole::Position,QPointF(333,444));
    editor.show();QApplication::processEvents();require(graph.nodeData(12,QtNodes::NodeRole::Position).toPointF()==QPointF(333,444),"Stale initial layout overwrites explicit typed replacement");
}
static void editor_layout_checks(SchemeEditorWidget& editor,NumericEditorGraphModel& graph){
    auto* view=editor.findChild<QtNodes::GraphicsView*>();require(view,"Editor viewport absent");
    ordinary_layout_checks(editor,graph,*view);one_time_layout_checks(editor,graph,*view);replacement_layout_checks(editor,graph);
}
static std::shared_ptr<NumericEditorGraphModel> widget_graph(GuiState& state){
    state.file_configs.ui.autosave_enabled=false;state.file_configs.ui.pipeline_auto_apply=false;
    state.delegate_model_registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for(const auto& kind:numeric_editor::catalog())state.delegate_model_registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    auto graph=std::make_shared<NumericEditorGraphModel>(state.delegate_model_registry);state.data_flow_graph=graph;
    domain::NumericProcessingGraph empty;empty.graph_id="main";require(graph->set_numeric_graph(empty),"Empty canonical fixture rejected");state.file_configs.scene_graph=graph->save();return graph;
}
static void widget_acceptance_checks(){
        GuiState state;auto graph=widget_graph(state);
        GuiPresenter presenter(state,test_fixture::synthetic_engine_provider(false));SchemeEditorWidget editor(presenter);editor.resize(1000,700);editor.show();QApplication::processEvents();
        QSignalSpy typedSave(&editor,&SchemeEditorWidget::numeric_draft_changed),applied(&presenter,&GuiPresenter::graph_apply_finished);
        capture_status_checks(editor,presenter);
        widget_catalogue_checks(editor);connection_policy_checks(editor,*graph);pinned_capture_import(editor,*graph);connected_bypass_undo(editor,*graph);
        exact_document_checks(editor,*graph,presenter,state,typedSave,applied);
        preview_lifecycle_checks(editor,*graph,presenter,applied);
}
static void model_acceptance_checks(){
    QStringList failures;
    const std::pair<const char*,void (*)()> checks[]{
        {"catalogue",catalogue_acceptance_checks},{"oscillator bank",oscillator_bank_acceptance_checks},
        {"dimensional editors",dimensional_editor_acceptance_checks},{"grouped layout",grouped_layout_acceptance_checks},
        {"bypass controls",bypass_controls_checks},{"canonical parameters",canonical_parameter_checks},
        {"selector repair",selector_repair_and_names_checks},{"preview model",preview_model_checks}};
    for(const auto& [name,check]:checks)run_editor_check(name,check,failures);
    require(failures.isEmpty(),qPrintable(failures.join('\n')));
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        QStringList failures;
        run_editor_check("model acceptance",model_acceptance_checks,failures);
        run_editor_check("widget acceptance",widget_acceptance_checks,failures);
        require(failures.isEmpty(),qPrintable(failures.join('\n')));
        qInfo("Canonical editor: catalog, atomic import, presets, Apply, exact bounded diagnostic, fences, live theme/RU/EN passed");return 0;
    }catch(const std::exception& error){qCritical("%s",error.what());return 1;}
}
