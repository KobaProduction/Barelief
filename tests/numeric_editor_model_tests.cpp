#include <desktop/numeric_source_format.h>
#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/numeric_node_operations.h"
#include "../presentation/qt/src/editor/numeric_graph_draft.h"

#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/ui/collapsible_section.h"
#include "core_app/ui/field_row.h"

#include "barelief/ui_text.h"

#include <desktop/numeric_project_document.h>

#include <QApplication>
#include <QImage>
#include <QFontDatabase>
#include <QPainter>
#include <QDir>
#include <QQuickWidget>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QThread>
#include <barelief/quick3d/surface_quick_item.h>
#include <QSurfaceFormat>
#include <QTextStream>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <barelief/axis_control_widget.h>
#include <QDoubleSpinBox>

#include <QLineEdit>
#include <QDialog>
#include <QTimer>

#include <QSpinBox>

#include <QPushButton>

#include <QComboBox>

#include <QCheckBox>

#include <QSpinBox>

#include <QJsonArray>
#include <QJsonDocument>

#include <QTemporaryDir>

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QPixmap>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLabel>
#include <QScopedPointer>
#include <utility>
#include <optional>
#include <vector>

#include <QtNodes/DataFlowGraphicsScene>

#include <core_app/scheme_editor/editor_geometry.h>
#include <core_app/scheme_editor/node_preview_card.h>

#include <QtNodes/internal/UndoCommands.hpp>

#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <cstdio>

#include <limits>

namespace {

int failures=0;

void check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL %s\n",text);++failures;}}
void disappearing_native_output(NumericEditorGraphModel& model,const domain::NumericProcessingGraph& graph,
    const QJsonObject& adapters,QJsonArray& devices,QUndoStack& history){
    const auto before=model.save();const auto undo=history.index();
    devices[0]=QJsonObject{{"id","native-A"},{"is_default",false},{"native_sample_rate",44100},{"native_channels",1}};
    QString error;
    check(!model.set_numeric_graph(graph,&error,adapters),"Connected output removal must reject native import");
    check(error==ui::text("Node %1: connected output %2 is unavailable in the native format").arg(1).arg(2),"Native rejection identifies the unavailable connected port");
    check(model.save()==before && history.index()==undo,"Native output removal preserves graph, adapter selection and Undo");
}
void verify_sparse_metrics_edges(NumericEditorGraphModel& graph,QtNodes::NodeId first,QtNodes::NodeId second,QtNodes::NodeId metrics){
    const auto* node=graph.delegateModel<AudioNodeModel>(metrics);
    const auto first_input=node->port_index(QtNodes::PortType::In,2),second_input=node->port_index(QtNodes::PortType::In,3);
    check(graph.connectionExists({first,0,metrics,first_input}),"Sparse deletion preserves stable input ID2");
    check(graph.connectionExists({second,0,metrics,second_input}),"Sparse deletion preserves stable input ID3");
    check(graph.numeric_graph().edges.size()==2,"Sparse deletion does not retain a stale remapped edge");
}
void grow_metrics_fixture(NumericEditorGraphModel& graph,QtNodes::NodeId metrics){
    auto node=*graph.delegateModel<AudioNodeModel>(metrics)->numeric_node();
    for(unsigned count=1;count<4;++count){const bool added=numeric_editor::add_input(node);check(added,"Sparse metrics fixture input growth");if(!added)return;}
    check(graph.edit_node(metrics,node),"Sparse metrics fixture publication");
}
void sparse_port_publication(std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry){
    NumericEditorGraphModel graph(registry);graph.set_admission_limits(numeric_editor::limits());QUndoStack history;graph.set_undo_stack(&history);
    const auto first=graph.addNode("Number"),second=graph.addNode("Number"),metrics=graph.addNode("ScalarView");
    grow_metrics_fixture(graph,metrics);graph.addConnection({first,0,metrics,2});graph.addConnection({second,0,metrics,3});
    const auto before=graph.save();
    check(graph.delegateModel<AudioNodeModel>(metrics)->remove_scalar_input(1),"Remove disconnected interior scalar input");
    verify_sparse_metrics_edges(graph,first,second,metrics);
    history.undo();check(graph.save()==before,"Sparse input Undo restores original stable endpoints");
    history.redo();verify_sparse_metrics_edges(graph,first,second,metrics);
}
void set_parameter(domain::ConfiguredProcessingNode& node,const char* key,domain::ProcessingParameterValue value){
    auto& fields=std::get<domain::TransformProcessingParameters>(node.parameters).values;
    const auto found=std::find_if(fields.begin(),fields.end(),[&](const auto& field){return field.key==key;});
    if(found==fields.end())fields.push_back({key,std::move(value)});else found->value=std::move(value);
}
std::size_t ordinary_inputs(const domain::ConfiguredProcessingNode& node){
    return std::count_if(node.inputs.begin(),node.inputs.end(),[&](const auto& port){return domain::numeric_parameter_key(node.kind,port.id).empty();});
}
bool edit_row(NodeParameterModel* model,int row){return model->editLiteral("row",QString::number(row),0);}
double literal_field(NodeParameterModel* model,const char* key){
    const auto value=model->field(key)->read();const auto* literal=std::get_if<signal_contracts::NumericLiteral>(&value);
    check(literal,"literal field has typed numeric value");return literal?std::visit([](auto number){return double(number);},*literal):0;
}

}

void coefficient_port_identity(QApplication& app){
        AudioNodeModel generator("WindowFunction");
        generator.set_numeric_node(*generator.numeric_node()); // Canonical ordering used by bind_node.
        int deleted=0,aboutToDelete=0;
        QObject::connect(&generator,&QtNodes::NodeDelegateModel::portsAboutToBeDeleted,&app,[&](auto,auto,auto){++aboutToDelete;});
        QObject::connect(&generator,&QtNodes::NodeDelegateModel::portsDeleted,&app,[&]{++deleted;});
        auto changed=*generator.numeric_node();
        set_parameter(changed,"size",signal_contracts::NumericLiteral{std::uint64_t{2048}});
        domain::OwnedProcessingContract bank;const auto input=changed.inputs[0].contract.view();
        check(domain::array_generator_output_contract(changed,std::get<signal_contracts::NumericContract>(input),bank)==signal_contracts::Status::ok,"resize coefficient bank contract");changed.outputs[0].contract=std::move(bank);
        check(domain::attach_numeric_parameter_ports(changed,numeric_editor::limits())==signal_contracts::Status::ok,"retain scalar window outputs after resize");
        generator.set_numeric_node(changed);
        check(aboutToDelete==0 && deleted==0 && generator.nPorts(QtNodes::PortType::Out)==3,"contract resize preserves stable port identities without QtNodes deletion/shift events");
        check(std::get<signal_contracts::NumericContract>(generator.numeric_node()->outputs[0].contract.view()).shape.extents[0].maximum==2048,"coefficient bank exposes updated geometry");
    }

void axis_range_controls(){
        SpectrogramGraph view;view.update_frame_history({{-.5f,.25f},{0,.75f}});
        check(view.y_range().first<=-.5f && view.y_range().second>=.75f && view.y_range().second<2,"automatic signed actual history extent");
        view.set_y_range(-2,3);
        check(view.y_range()==QPair<float,float>{-2,3} && view.surface_settings().value("y_range_mode")=="manual","manual API changes display range");
        view.update_frame_history({{.001f,.002f}});check(view.y_range()==QPair<float,float>{-2,3},"new frames preserve manual range");
        const auto explicit_settings=view.surface_settings();SpectrogramGraph restored;restored.set_surface_settings(explicit_settings);
        check(restored.y_range()==QPair<float,float>{-2,3},"explicit manual settings restore");
        auto settings=view.surface_settings();settings["y_range_mode"]="auto";view.set_surface_settings(settings);
        check(view.y_range().first==0 && view.y_range().second>=.002f && view.y_range().second<.01f,"Auto uses actual current values with bounded display headroom");
        view.set_y_range(0,2e-8f);
        check(view.y_range()==QPair<float,float>{0,2e-8f},"small manual range stays applied instead of resetting to 0..1");
        AxisControlWidget control("Amplitude (Y)");control.set_state(0,2e-8,true,false);
        double appliedLow=-1,appliedHigh=-1;bool automatic=true;
        QObject::connect(&control,&AxisControlWidget::range_changed,&control,[&](double low,double high,bool mode){appliedLow=low;appliedHigh=high;automatic=mode;});
        control.auto_box()->setChecked(false);control.findChild<QDoubleSpinBox*>("axisMaximum")->setValue(3e-8);
        check(!automatic && appliedLow==0 && std::abs(appliedHigh-3e-8)<1e-15,"real range control emits small manual interval");
        settings["y_range_mode"]="auto";view.set_surface_settings(settings);const auto heldRange=view.y_range();view.update_frame_history({});
        check(view.projected_history_frames()==0 && view.y_range()==heldRange,"empty projection clears geometry and preserves interval amplitude bounds");
    }

void source_rate_dependencies(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel rates(registry);
        const auto source=rates.addNode("SignalGenerator"),buffer=rates.addNode("Window");
        auto* generator=rates.delegateModel<AudioNodeModel>(source);auto* window=rates.delegateModel<AudioNodeModel>(buffer);
        rates.addConnection({source,0,buffer,0});
        rates.addConnection({source,generator->port_index(QtNodes::PortType::Out,domain::sample_rate_output),buffer,window->port_index(QtNodes::PortType::In,domain::input_sample_rate_input)});
        auto changed=*generator->numeric_node();check(numeric_editor::clock_node(changed,{"local",{1,44100},44100}),"prepare explicit source Fs edit");
        check(rates.edit_node(source,changed),"source Fs edit with explicit numeric dependency reconciles atomically");
        check(std::get<signal_contracts::NumericContract>(window->numeric_node()->outputs[0].contract.view()).clock.sample_rate==44100,"window clock follows explicit source Fs change");
        domain::NumericProcessingGraph exported;check(rates.export_numeric_graph(exported),"source Fs edit produces valid complete graph");
        const auto clock=rates.addNode("SampleClock");changed=*generator->numeric_node();changed.clock_node=clock;check(rates.edit_node(source,changed),"bind source with explicit Fs dependency");
        changed=*generator->numeric_node();check(numeric_editor::clock_node(changed,{"local",{1,44100},44100}),"prepare bound source override");const auto bound_before=rates.save();
        check(rates.edit_node(source,changed) && rates.save()==bound_before,"explicit synchronization binding wins over a conflicting derived source contract");

    }

void capture_endpoint_guard(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel capture(registry);
        capture.set_admission_limits(numeric_editor::limits());capture.set_capture_device_provider([](int source,QString*){return QJsonArray{QJsonObject{{"id",source==0?"render-A":"capture-B"},{"name","Fake endpoint"},{"is_default",true},{"native_sample_rate",48000},{"native_channels",1}}};});
        const auto source=capture.addNode("AudioSource"),buffer=capture.addNode("Window");
        auto* physical=capture.delegateModel<AudioNodeModel>(source);auto* window=capture.delegateModel<AudioNodeModel>(buffer);
        check(capture.edit_adapter(source,{{"device_identity","render-A"}}),"pin loopback endpoint for source guard");
        capture.addConnection({source,0,buffer,0});capture.addConnection({source,physical->port_index(QtNodes::PortType::Out,domain::sample_rate_output),buffer,window->port_index(QtNodes::PortType::In,domain::input_sample_rate_input)});
        const auto change_kind=[](auto& node){auto& fields=std::get<domain::EndpointProcessingParameters>(node.parameters).values;for(auto& f:fields)if(f.key=="source"){f.value=signal_contracts::NumericLiteral{std::int64_t{1}};return;}fields.push_back({"source",signal_contracts::NumericLiteral{std::int64_t{1}}});};
        auto changed=*physical->numeric_node();change_kind(changed);const auto before=capture.save();
        check(!capture.edit_node(source,changed) && capture.save()==before,"explicit Fs dependencies cannot bypass pinned capture endpoint guard");
        check(capture.edit_adapter(source,{}),"restore default endpoint");changed=*physical->numeric_node();change_kind(changed);check(numeric_editor::set_sample_rate(changed,44100),"prepare simultaneous source/rate edit");const auto default_before=capture.save();
        check(capture.edit_node(source,changed) && capture.save()!=default_before,"unpinned draft may explicitly change source kind and declared rate");
    }

void fourier_port_roundtrip(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel fourier(registry);
        const auto complex=fourier.addNode("Fourier"),real=fourier.addNode("RealFourier"),polar=fourier.addNode("ComplexPolar");
        auto* a=fourier.delegateModel<AudioNodeModel>(complex);auto* b=fourier.delegateModel<AudioNodeModel>(real);
        check(a && a->numeric_node() && ordinary_inputs(*a->numeric_node())==2,"Complex Fourier exposes both data planes plus explicit settings ports");
        check(b && b->numeric_node() && ordinary_inputs(*b->numeric_node())==1,"Real Fourier exposes one data plane plus explicit settings ports");
        check(a->numeric_node()->outputs.size()==4 && b->numeric_node()->outputs.size()==4,"Fourier exposes Re/Im and separate DC scalars");
        for(const auto edge:std::array{QtNodes::ConnectionId{real,0,polar,0},QtNodes::ConnectionId{real,1,polar,1}}){
            check(fourier.connectionPossible(edge),"Fourier Re/Im can feed the polar projection");fourier.addConnection(edge);check(fourier.connectionExists(edge),"Polar connection persists");}
        domain::NumericProcessingGraph graph;QString error;check(fourier.export_numeric_graph(graph,&error),"Fourier/polar graph exports");
        QByteArray encoded;check(bool(desktop::encode_numeric_graph(graph,encoded)),"Fourier graph serializes");
        domain::NumericProcessingGraph restored;check(bool(desktop::decode_numeric_graph(encoded,restored)),"Fourier graph deserializes");
        desktop::NumericProjectDocument document;
        check(desktop::create_numeric_project(restored,{},document) && desktop::read_numeric_draft(document,restored),"Fourier project round-trip");
        NumericEditorGraphModel roundtrip(registry);check(roundtrip.set_numeric_graph(restored),"Fourier/polar ports restore atomically");
    }

void routing_point_roundtrip(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel routes(registry);
        const auto source=routes.addNode("RealFourier"),route=routes.addNode("Reroute"),view=routes.addNode("FFTView");
        for(const auto edge:std::array{QtNodes::ConnectionId{source,0,route,0},QtNodes::ConnectionId{route,0,view,0}}){
            check(routes.connectionPossible(edge),"Routing point adopts exact vector geometry");routes.addConnection(edge);check(routes.connectionExists(edge),"Routing point wire persists");
        }
        auto* model=routes.delegateModel<AudioNodeModel>(route);check(model && !model->embeddedWidget(),"Routing point has no processor body");
        check(!model->parameter_model()->supportsBypass(),"Routing point has no DSP execution toggle");
        domain::NumericProcessingGraph graph;check(routes.export_numeric_graph(graph),"Routing topology exports");
        QByteArray encoded;check(bool(desktop::encode_numeric_graph(graph,encoded)),"Routing point serializes");
        domain::NumericProcessingGraph restored;check(bool(desktop::decode_numeric_graph(encoded,restored)),"Routing point deserializes");
        NumericEditorGraphModel roundtrip(registry);check(roundtrip.set_numeric_graph(restored),"Routing point restores with ordinary wires");
    }

void numeric_primitive_roundtrip(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel numbers(registry);
        const auto fs=numbers.addNode("Fs"),integer=numbers.addNode("UInt32Constant"),route=numbers.addNode("Reroute"),view=numbers.addNode("ScalarView");
        auto* fsModel=numbers.delegateModel<AudioNodeModel>(fs);check(fsModel && fsModel->caption()=="Fs","Fs keeps a concise block caption");
        const QScopedPointer<QWidget> fsBody(fsModel->embeddedWidget());
        check(fsBody->toolTip()==ui::text("Sampling frequency, Hz"),"Fs tooltip explains the numeric sample rate");
        check(fsModel->parameter_model()->editLiteral("value","96000",1),"Fs numeric literal is editable");
        for(const auto edge:std::array{QtNodes::ConnectionId{integer,0,route,0},QtNodes::ConnectionId{route,0,view,0}}){
            check(numbers.connectionPossible(edge),"Ordinary UInt32 scalar connects through a routing point");numbers.addConnection(edge);check(numbers.connectionExists(edge),"Numeric scalar route persists");
        }
        const auto* scalar=numbers.delegateModel<AudioNodeModel>(route)->numeric_node();
        const auto data=scalar->outputs[0].contract.view();check(std::get<signal_contracts::NumericContract>(data).dtype==signal_contracts::DType::UInt32 && std::get<signal_contracts::NumericContract>(data).shape.rank==signal_contracts::Rank::Scalar,"Routing point adopts scalar dtype and geometry");
        const auto window=numbers.addNode("Window"),length=numbers.addNode("ArrayLength"),rate=numbers.addNode("SampleRate");
        for(const auto edge:std::array{QtNodes::ConnectionId{window,0,length,0},QtNodes::ConnectionId{window,0,rate,0}}){
            check(numbers.connectionPossible(edge),"Numeric array metadata connects to a buffer output");numbers.addConnection(edge);check(numbers.connectionExists(edge),"Array metadata wire persists");
        }
        domain::NumericProcessingGraph graph;check(numbers.export_numeric_graph(graph),"Fs and dimensions export without synchronization bindings");
        QByteArray encoded;check(bool(desktop::encode_numeric_graph(graph,encoded)),"Numeric primitive graph serializes");
        domain::NumericProcessingGraph restored;check(bool(desktop::decode_numeric_graph(encoded,restored)),"Numeric primitive graph deserializes");
        NumericEditorGraphModel roundtrip(registry);check(roundtrip.set_numeric_graph(restored),"Numeric primitives restore with ordinary ports");
    }

void parameter_wire_geometry(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel parameters(registry);QUndoStack history;parameters.set_undo_stack(&history);
        const auto window=parameters.addNode("Window"),fft=parameters.addNode("RealFourier"),length=parameters.addNode("ArrayLength");
        auto* buffer=parameters.delegateModel<AudioNodeModel>(window);
        check(buffer->port_index(QtNodes::PortType::In,domain::window_size_input)!=QtNodes::InvalidPortIndex,"Window exposes an explicit optional N command port");
        check(buffer->parameter_model()->field("size")->primary,"Window size is directly editable on card");
        parameters.addConnection({window,0,fft,0});parameters.addConnection({window,0,length,0});
        auto* fftNode=parameters.delegateModel<AudioNodeModel>(fft);
        parameters.addConnection({length,0,fft,fftNode->port_index(QtNodes::PortType::In,domain::window_size_input)});
        check(buffer->parameter_model()->editLiteral("size","2048",0),"Local window size updates FFT and measured array length");
        auto size=[&]{const auto value=parameters.delegateModel<AudioNodeModel>(fft)->numeric_node()->outputs[0].contract.view();return std::get<signal_contracts::NumericContract>(value).shape.extents[0].maximum;};
        check(size()==1024,"Real Fourier follows local window size");
        history.undo();check(size()==2048,"Local window size undo");history.redo();check(size()==1024,"Local window size redo");
        domain::NumericProcessingGraph exported;check(parameters.export_numeric_graph(exported),"Measured-size graph exports");
        NumericEditorGraphModel restored(registry);check(restored.set_numeric_graph(exported),"Measured-size graph restores");
        const auto number=parameters.addNode("Number");auto* value=parameters.delegateModel<AudioNodeModel>(number);
        check(value->parameter_model()->editLiteral("value","-1.25",3),"Generic Number accepts float64 negative fraction");
        check(std::get<signal_contracts::NumericContract>(value->numeric_node()->outputs[0].contract.view()).dtype==signal_contracts::DType::Float64,"Number type follows selected primitive dtype");

    }

void surviving_widget_localization(){
        QScopedPointer<QWidget> remainingBody;
        { AudioNodeModel transient("Fs");remainingBody.reset(transient.embeddedWidget());transient.set_processing_activity(true,true,120); }
        ui::Language::instance().set("ru");
        check(remainingBody->toolTip()==ui::text("Sampling frequency, Hz"),"Surviving widget localization does not call the destroyed node model");
        ui::Language::instance().set("en");
    }

void temporal_window_edit(NumericEditorGraphModel& temporal,QtNodes::NodeId window,AudioNodeModel* windowModel,NodeParameterModel* windowParameters){
        int unrelated_validations=0,wire_changes=0,immediate_requests=0;
        temporal.set_node_validator([&](const domain::ConfiguredProcessingNode& n){if(n.id!=domain::NodeId(window))++unrelated_validations;return domain::ProcessingDiagnostic{};});
        const auto deleted=QObject::connect(&temporal,&QtNodes::DataFlowGraphModel::connectionDeleted,&temporal,[&]{++wire_changes;});
        const auto created=QObject::connect(&temporal,&QtNodes::DataFlowGraphModel::connectionCreated,&temporal,[&]{++wire_changes;});
        const auto immediate=QObject::connect(&temporal,&NumericEditorGraphModel::immediate_apply_requested,&temporal,[&]{++immediate_requests;});
        check(windowParameters->edit("function",2),"Separate matrix window function is editable");
        check(unrelated_validations==0 && wire_changes==0,"Window function edit must not preflight unrelated nodes or detach wires");
        auto enabledWindow=*windowModel->numeric_node();enabledWindow.enabled=true;
        check(temporal.edit_node(window,enabledWindow),"Separate matrix window can leave generic bypass");
        check(unrelated_validations==0 && wire_changes==0 && immediate_requests==2,"Window selections retain topology and request Apply without the typing debounce");
        QObject::disconnect(deleted);QObject::disconnect(created);QObject::disconnect(immediate);temporal.set_node_validator({});

}
std::unique_ptr<QWidget> temporal_center_edit(NumericEditorGraphModel& temporal,QUndoStack& undo,QtNodes::NodeId center,AudioNodeModel* centerModel,NodeParameterModel* centerParameters){
        check(centerParameters->onBlock("method") && std::get<int>(centerParameters->field("method")->read())==0 && centerModel->numeric_node()->enabled,"Centering defaults to visible mean subtraction");
        std::unique_ptr<QWidget> centerForm(centerModel->parameter_widget());
        auto* offsetControl=centerForm->findChild<QWidget*>("offset");
        check(offsetControl && !offsetControl->isEnabled(),"Manual offset control is disabled for mean subtraction");
        undo.clear();const auto centerBefore=temporal.save();
        const auto centerEdges=temporal.numeric_graph().edges;int centerOtherValidations=0,centerWireChanges=0,centerImmediate=0;
        temporal.set_node_validator([&](const domain::ConfiguredProcessingNode& n){if(n.id!=domain::NodeId(center))++centerOtherValidations;return domain::ProcessingDiagnostic{};});
        const auto centerDeleted=QObject::connect(&temporal,&QtNodes::DataFlowGraphModel::connectionDeleted,&temporal,[&]{++centerWireChanges;});
        const auto centerCreated=QObject::connect(&temporal,&QtNodes::DataFlowGraphModel::connectionCreated,&temporal,[&]{++centerWireChanges;});
        const auto centerApplied=QObject::connect(&temporal,&NumericEditorGraphModel::immediate_apply_requested,&temporal,[&]{++centerImmediate;});
        check(centerParameters->edit("method",1) && !offsetControl->isEnabled(),"Midrange centering leaves the manual offset disabled");
        check(centerParameters->edit("method",2) && offsetControl->isEnabled() && centerParameters->editLiteral("offset","2.5",3),"Manual centering enables its exact numeric offset");
        check(!centerParameters->editLiteral("offset","1e100",3),"Manual centering refuses an offset outside finite Float32 range");
        check(centerOtherValidations==0 && centerWireChanges==0 && centerImmediate==3 && temporal.numeric_graph().edges==centerEdges,"Centering parameter edits retain ports and wires and request immediate Apply");
        QObject::disconnect(centerDeleted);QObject::disconnect(centerCreated);QObject::disconnect(centerApplied);temporal.set_node_validator({});
        const auto centerAfter=temporal.save();undo.undo();undo.undo();undo.undo();check(temporal.save()==centerBefore && !offsetControl->isEnabled(),"Centering method and offset undo restore the original settings");
        undo.redo();undo.redo();undo.redo();check(temporal.save()==centerAfter && offsetControl->isEnabled(),"Centering method and offset redo restore manual settings");

return centerForm;
}
void temporal_geometry_edits(NumericEditorGraphModel& temporal,QUndoStack& undo,QtNodes::NodeId mel,QtNodes::NodeId history,QtNodes::NodeId transpose,QtNodes::NodeId center,QtNodes::NodeId window,QtNodes::NodeId fft,QtNodes::NodeId row,QtNodes::NodeId map,QtNodes::NodeId line,AudioNodeModel* rowModel,NodeParameterModel* fftParameters,NodeParameterModel* rowParameters,const QString& temporalRejection,const auto& matrix){
        undo.clear();const auto before=temporal.save();
        check(fftParameters->editLiteral("fft_size","256",0),"Connected FFT size change accepted");
        check(matrix().shape.extents[1].maximum==128 && std::get<signal_contracts::NumericContract>(rowModel->numeric_node()->outputs[0].contract.view()).shape.extents[0].maximum==128 &&
            std::get<signal_contracts::NumericContract>(temporal.delegateModel<AudioNodeModel>(line)->numeric_node()->inputs[0].contract.view()).shape.extents[0].maximum==128,"FFT shape changes propagate through row selection to vector view");
        const auto after=temporal.save();undo.undo();check(temporal.save()==before,"Temporal shape edit undo is atomic");undo.redo();check(temporal.save()==after,"Temporal shape edit redo is atomic");
        auto* melParameters=temporal.delegateModel<AudioNodeModel>(mel)->parameter_model();
        check(edit_row(rowParameters,49),"Select final band before upstream reduction");undo.clear();const auto beforeReduction=temporal.save();
        check(melParameters->editLiteral("bands","8",0) && temporal.save()!=beforeReduction && undo.count()==1,"Upstream edit can retain an incompatible selected band in draft");
        domain::NumericProcessingGraph refused;check(!temporal.export_numeric_graph(refused),"invalid selected-band draft cannot execute");
        undo.undo();check(temporal.save()==beforeReduction,"Undo restores the valid band geometry atomically");
        check(edit_row(rowParameters,0),"Choose valid band before upstream reduction");undo.clear();const auto compatibleBefore=temporal.save();
        const bool reducedBands=melParameters->editLiteral("bands","32",0);
        if(!reducedBands)std::fprintf(stderr,"Temporal upstream reduction rejected: %s\n",qPrintable(temporalRejection));
        check(reducedBands,"Upstream Mel band count can change with temporal views connected");
        const auto contractOf=[&](QtNodes::NodeId node,bool output){const auto* value=temporal.delegateModel<AudioNodeModel>(node)->numeric_node();return std::get<signal_contracts::NumericContract>((output?value->outputs:value->inputs)[0].contract.view());};
        check(contractOf(mel,true).shape.extents[0].maximum==32 && contractOf(history,false).shape.extents[0].maximum==32 && contractOf(history,true).shape.extents[1].maximum==32 &&
            contractOf(transpose,false).shape.extents[1].maximum==32 && contractOf(transpose,true).shape.extents[0].maximum==32 && contractOf(center,false).shape.extents[0].maximum==32 && contractOf(center,true).shape.extents[0].maximum==32 && contractOf(window,false).shape.extents[0].maximum==32 && contractOf(window,true).shape.extents[0].maximum==32 && contractOf(fft,false).shape.extents[0].maximum==32 && contractOf(fft,true).shape.extents[0].maximum==32 && contractOf(row,false).shape.extents[0].maximum==32 && contractOf(map,false).shape.extents[0].maximum==32,
            "Mel geometry propagates through separate transpose, window, FFT, row and matrix view");
        check(contractOf(row,true).shape.extents[0].maximum==128 && contractOf(line,false).shape.extents[0].maximum==128 && rowParameters->field("row")->numeric_maximum.value_or(-1)==31,"Upstream bands preserve FFT size and refresh selection bounds");
        const auto reduced=temporal.save();undo.undo();check(temporal.save()==compatibleBefore,"Upstream Mel geometry undo restores entire branch");undo.redo();check(temporal.save()==reduced,"Upstream Mel geometry redo restores entire branch");
        check(melParameters->editLiteral("bands","8",0) && rowParameters->field("row")->numeric_maximum.value_or(-1)==7 && matrix().shape.extents[0].maximum==8,"Reduction to eight bands succeeds with selected band zero");

}
void temporal_matrix_chain(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel temporal(registry);QUndoStack undo;temporal.set_undo_stack(&undo);
        QString temporalRejection;QObject::connect(&temporal,&NumericEditorGraphModel::edit_rejected,&temporal,[&](const QString& error){temporalRejection=error;});
        const auto mel=temporal.addNode("Mel"),history=temporal.addNode("Spectrogram"),transpose=temporal.addNode("MatrixTranspose"),center=temporal.addNode("MatrixCenter"),window=temporal.addNode("MatrixWindowFunction"),fft=temporal.addNode("MatrixFFT"),row=temporal.addNode("MatrixRow"),map=temporal.addNode("SpectrogramView"),line=temporal.addNode("FFTView");
        auto* centerModel=temporal.delegateModel<AudioNodeModel>(center);auto* centerParameters=centerModel->parameter_model();
        auto* transposeModel=temporal.delegateModel<AudioNodeModel>(transpose);auto* windowModel=temporal.delegateModel<AudioNodeModel>(window);auto* fftModel=temporal.delegateModel<AudioNodeModel>(fft);auto* rowModel=temporal.delegateModel<AudioNodeModel>(row);
        auto* windowParameters=windowModel->parameter_model();auto* fftParameters=fftModel->parameter_model();auto* rowParameters=rowModel->parameter_model();
        check(fftParameters->field("fft_size") && !fftParameters->field("function") && !fftParameters->field("window_enabled") && windowParameters->field("function"),"Matrix FFT and window have separate registered controls");
        check(fftParameters->editLiteral("fft_size","512",0) && !windowModel->numeric_node()->enabled && fftModel->numeric_node()->enabled,"Matrix FFT fixture declares 512 explicitly while the separate window defaults to bypass");
        check(fftParameters->onBlock("fft_size") && windowParameters->onBlock("function") && rowParameters->onBlock("row"),"Matrix FFT, window and band selection are visible on their blocks");
        check(temporal.delegateModel<AudioNodeModel>(history)->parameter_model()->editLiteral("numericCapacity","200",1),"History capacity can differ from FFT zero-padding size");
        for(const auto edge:std::array{QtNodes::ConnectionId{mel,0,history,0},QtNodes::ConnectionId{history,0,transpose,0},QtNodes::ConnectionId{transpose,0,center,0},QtNodes::ConnectionId{center,0,window,0},QtNodes::ConnectionId{window,0,fft,0},QtNodes::ConnectionId{fft,0,map,0},QtNodes::ConnectionId{fft,0,row,0},QtNodes::ConnectionId{row,0,line,0}}) {
            check(temporal.connectionPossible(edge),"Temporal analysis connection preflight");temporal.addConnection(edge);check(temporal.connectionExists(edge),"Temporal analysis connection accepted");
        }
        const auto matrix=[&]{return std::get<signal_contracts::NumericContract>(fftModel->numeric_node()->outputs[0].contract.view());};
        check(matrix().shape.axes[0]==signal_contracts::Axis::Band && matrix().shape.axes[1]==signal_contracts::Axis::Bin && matrix().shape.extents[0].maximum==50 && matrix().shape.extents[1].maximum==256,"Matrix row FFT output is a band-by-bin ordinary matrix");
        const auto transposed=std::get<signal_contracts::NumericContract>(transposeModel->numeric_node()->outputs[0].contract.view());
        const auto centered=std::get<signal_contracts::NumericContract>(centerModel->numeric_node()->outputs[0].contract.view());
        check(centered.shape==transposed.shape && centered.coordinates==transposed.coordinates && centered.clock.domain==transposed.clock.domain && centered.clock.sample_rate==transposed.clock.sample_rate && centered.clock.tick_seconds==transposed.clock.tick_seconds && centered.semantic==signal_contracts::Semantic::Numeric && centered.representation.normalization==signal_contracts::Normalization::None,"Centering preserves matrix geometry and physical coordinates without a normalization claim");
        check(transposed.shape.axes[0]==signal_contracts::Axis::Band && transposed.shape.axes[1]==signal_contracts::Axis::Time && transposed.shape.extents[1].maximum==200 && std::get<signal_contracts::NumericContract>(fftModel->numeric_node()->inputs[0].contract.view()).shape.extents[1].maximum==200,"Separate transpose and window preserve the real History capacity before zero padding");
        check(rowParameters->field("row")->numeric_maximum.value_or(-1)==49 && edit_row(rowParameters,49) && !edit_row(rowParameters,50) && edit_row(rowParameters,0),"Every valid band can be selected and out-of-range selection is refused");
        temporal_window_edit(temporal,window,windowModel,windowParameters);
        auto centerForm=temporal_center_edit(temporal,undo,center,centerModel,centerParameters);
        temporal_geometry_edits(temporal,undo,mel,history,transpose,center,window,fft,row,map,line,rowModel,fftParameters,rowParameters,temporalRejection,matrix);
        desktop::NumericProjectDocument project;domain::NumericProcessingGraph restored;
        check(desktop::create_numeric_project(temporal.numeric_graph(),{},project) && desktop::read_numeric_draft(project,restored),"Separate matrix chain project round-trip");
        NumericEditorGraphModel reloaded(registry);check(reloaded.set_numeric_graph(restored),"Separate matrix chain saved graph can be restored");
        check(literal_field(reloaded.delegateModel<AudioNodeModel>(row)->parameter_model(),"row")==0,"Selected band survives project reload");
        auto* reloadedCenter=reloaded.delegateModel<AudioNodeModel>(center)->parameter_model();
        check(std::get<int>(reloadedCenter->field("method")->read())==2 && std::get<double>(std::get<signal_contracts::NumericLiteral>(reloadedCenter->field("offset")->read()))==2.5,"Centering method and manual offset survive project reload");
        for(const auto* language:{"ru","en"}) {ui::Language::instance().set(language);check(transposeModel->caption()==ui::text("Matrix transpose") && centerModel->caption()==ui::text("Matrix row centering") && windowModel->caption()==ui::text("Matrix window function") && fftModel->caption()==ui::text("Matrix row FFT") && rowModel->caption()==ui::text("Matrix row selector"),"Separate matrix controls use shared localization");}
        ui::Language::instance().set("en");
    }

void explicit_clock_bindings(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel clocks(registry);domain::NumericProcessingGraph exported;QString error;
        const auto unbound=clocks.addNode("Mixer");
        check(!clocks.export_numeric_graph(exported,&error) && error.contains(QString::number(unbound)) && error.contains("synchronization"),"Missing mixer synchronization reports actionable node identity before Apply");
        clocks.deleteNode(unbound);
        const auto clock=clocks.addNode("SampleClock");
        const auto generator=clocks.addNode("SignalGenerator"),mixer=clocks.addNode("Mixer");
        check(clocks.delegateModel<AudioNodeModel>(generator)->numeric_node()->clock_node==-1 &&
              clocks.delegateModel<AudioNodeModel>(mixer)->numeric_node()->clock_node==domain::NodeId(clock),"Generator starts internally; FIFO mixer uses the explicit editor synchronization source");
        auto synchronized=*clocks.delegateModel<AudioNodeModel>(generator)->numeric_node();synchronized.clock_node=clock;
        check(clocks.edit_node(generator,synchronized),"Generator can explicitly join shared synchronization");
        check(clocks.export_numeric_graph(exported,&error),"Explicit unique clock defaults export successfully");
        check(exported.configuration_bindings.size()==2,"Configuration dependency wires export separately from numeric edges");
        const auto* clockModel=clocks.delegateModel<AudioNodeModel>(clock);const auto* generatorModel=clocks.delegateModel<AudioNodeModel>(generator);
        const QtNodes::ConnectionId configEdge{clock,clockModel->configuration_port(QtNodes::PortType::Out,domain::ConfigurationRole::SampleClock),generator,generatorModel->configuration_port(QtNodes::PortType::In,domain::ConfigurationRole::SampleClock)};
        check(clocks.connectionExists(configEdge) && clocks.saveConnection(configEdge).contains("configuration"),"Visible typed clock pin has persistent config wire");
        NumericEditorGraphModel roundtrip(registry);check(roundtrip.set_numeric_graph(exported),"Explicit bindings import atomically");check(roundtrip.numeric_graph().configuration_bindings==exported.configuration_bindings,"Configuration bindings survive editor import/export");

        clocks.addNode("SampleClock");const auto ambiguous=clocks.addNode("Mixer");
        check(clocks.delegateModel<AudioNodeModel>(ambiguous)->numeric_node()->clock_node==-1 && !clocks.export_numeric_graph(exported,&error),"Independent clocks are never selected implicitly");
    }

void configuration_visual_import(NumericEditorGraphModel& configUndo,const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        auto visual=configUndo.save();const auto connections=visual["connections"].toArray();QJsonArray reversed;
        for(auto i=connections.size();i>0;--i)reversed.append(connections[i-1]);visual["connections"]=reversed;
        NumericEditorGraphModel strictReload(registry);strictReload.load(visual);
        const auto expectedImport=configUndo.numeric_graph(),actualImport=strictReload.numeric_graph();
        bool exactImport=expectedImport.nodes.size()==actualImport.nodes.size() && std::is_permutation(expectedImport.edges.begin(),expectedImport.edges.end(),actualImport.edges.begin(),actualImport.edges.end()) && std::is_permutation(expectedImport.configuration_bindings.begin(),expectedImport.configuration_bindings.end(),actualImport.configuration_bindings.begin(),actualImport.configuration_bindings.end());
        for(const auto& n:expectedImport.nodes){const auto found=std::find_if(actualImport.nodes.begin(),actualImport.nodes.end(),[&](const auto& other){return other.id==n.id;});QByteArray a,b;if(found==actualImport.nodes.end() || !desktop::encode_numeric_node(n,a) || !desktop::encode_numeric_node(*found,b) || a!=b || n.clock_node!=found->clock_node){exactImport=false;std::fprintf(stderr,"Strict import mismatch node=%d originalClock=%d loadedClock=%d original=%s loaded=%s\n",int(n.id),int(n.clock_node),found==actualImport.nodes.end()?-999:int(found->clock_node),a.constData(),b.constData());}}
        if(!exactImport)std::fprintf(stderr,"Strict import counts nodes=%zu/%zu edges=%zu/%zu config=%zu/%zu\n",expectedImport.nodes.size(),actualImport.nodes.size(),expectedImport.edges.size(),actualImport.edges.size(),expectedImport.configuration_bindings.size(),actualImport.configuration_bindings.size());
        check(exactImport,"Strict visual import preserves prepared downstream contracts independently of configuration-wire order");

}
void configuration_exact_undo(NumericEditorGraphModel& configUndo,const domain::NumericProcessingGraph& before){
        const auto restored=configUndo.numeric_graph();bool same=before.nodes.size()==restored.nodes.size();for(std::size_t i=0;same && i<before.nodes.size();++i){QByteArray a,b;desktop::encode_numeric_node(before.nodes[i],a);desktop::encode_numeric_node(restored.nodes[i],b);same=a==b && before.nodes[i].clock_node==restored.nodes[i].clock_node;if(!same)std::fprintf(stderr,"Config undo mismatch id=%lld beforeClock=%lld restoredClock=%lld before=%s restored=%s\n",static_cast<long long>(before.nodes[i].id),static_cast<long long>(before.nodes[i].clock_node),static_cast<long long>(restored.nodes[i].clock_node),a.constData(),b.constData());}check(same,"Config undo restores exact before processing state");

}
void configuration_wire_undo(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel configUndo(registry);
        configUndo.addNode("SampleClock");const auto clock=configUndo.addNode("SampleClock");
        check(configUndo.delegateModel<AudioNodeModel>(clock)->parameter_model()->edit("clockSampleRate",44100),"Configuration undo test distinct target rate");
        const auto generator=configUndo.addNode("SignalGenerator"),window=configUndo.addNode("Window"),view=configUndo.addNode("WindowView");
        configUndo.addConnection({generator,0,window,0});configUndo.addConnection({window,0,view,0});
        const auto* generatorPorts=configUndo.delegateModel<AudioNodeModel>(generator);
        const auto* windowPorts=configUndo.delegateModel<AudioNodeModel>(window);
        configUndo.addConnection({generator,generatorPorts->port_index(QtNodes::PortType::Out,domain::sample_rate_output),window,
            windowPorts->port_index(QtNodes::PortType::In,domain::input_sample_rate_input)});
        const auto* c=configUndo.delegateModel<AudioNodeModel>(clock);const auto* g=configUndo.delegateModel<AudioNodeModel>(generator);
        const QtNodes::ConnectionId edge{clock,c->configuration_port(QtNodes::PortType::Out,domain::ConfigurationRole::SampleClock),generator,g->configuration_port(QtNodes::PortType::In,domain::ConfigurationRole::SampleClock)};
        const auto before=configUndo.numeric_graph();const auto undo=configUndo.connectionUndoState(edge);configUndo.addConnection(edge);
        const auto rate=[&](QtNodes::NodeId id){const auto* node=configUndo.delegateModel<AudioNodeModel>(id)->numeric_node();const auto data=(node->outputs.empty()?node->inputs.front():node->outputs.front()).contract.view();return std::get<signal_contracts::NumericContract>(data).clock.sample_rate;};
        check(configUndo.connectionExists(edge) && rate(generator)==44100 && rate(window)==44100 && rate(view)==44100,"Config wire adapts source and downstream contracts");
        configuration_visual_import(configUndo,registry);
        check(configUndo.undoConnection(edge,undo),"Configuration connection undo succeeds");
        check(!configUndo.connectionExists(edge) && rate(generator)==48000 && rate(window)==48000 && rate(view)==48000 && configUndo.delegateModel<AudioNodeModel>(generator)->numeric_node()->clock_node==-1,"Config undo restores contracts and derived cache throughout downstream");
        configuration_exact_undo(configUndo,before);
        configUndo.addConnection(edge);check(configUndo.connectionExists(edge) && rate(generator)==44100 && rate(window)==44100 && rate(view)==44100,"Config redo reapplies source and downstream contracts");
        const auto mixer=configUndo.addNode("Mixer");auto boundMixer=*configUndo.delegateModel<AudioNodeModel>(mixer)->numeric_node();boundMixer.clock_node=clock;check(configUndo.edit_node(mixer,boundMixer),"Bind independent mixer for deletion isolation");
        int removals=0;QObject::connect(&configUndo,&NumericEditorGraphModel::atomic_edit_finished,&configUndo,[&]{++removals;});
        check(configUndo.deleteConnection(edge),"Manual configuration wire deletion succeeds");
        check(rate(generator)==44100 && rate(window)==44100 && configUndo.delegateModel<AudioNodeModel>(generator)->numeric_node()->clock_node==-1,"Manual deletion clears assignment cache without reverting numeric Fs");
        const auto remaining=configUndo.numeric_graph();check(domain::has_configuration_binding(remaining,mixer,domain::ConfigurationRole::SampleClock) && configUndo.delegateModel<AudioNodeModel>(mixer)->numeric_node()->clock_node==domain::NodeId(clock),"Manual deletion preserves other configuration bindings and caches");
        check(removals==1,"Manual deletion publishes dependency invalidation");

    }

void native_import_refusals(NumericEditorGraphModel& native,QUndoStack& history,QJsonArray& devices,const domain::NumericProcessingGraph& imported,QString& error,domain::NumericProcessingGraph& exported){
        const auto before=native.save();const auto undoBefore=history.index();
        devices[0]=QJsonObject{{"id","native-A"},{"is_default",true},{"native_sample_rate",999},{"native_channels",3}};
        check(!native.set_numeric_graph(imported,&error) && !error.isEmpty() && native.save()==before && history.index()==undoBefore,
            "Unsupported native import preserves prior graph and Undo atomically");
        devices[0]=QJsonObject{{"id","native-A"},{"is_default",false},{"native_sample_rate",44100},{"native_channels",3}};
        devices[1]=QJsonObject{{"id","native-B"},{"is_default",true},{"native_sample_rate",32000},{"native_channels",1}};
        auto saved=native.numeric_graph();saved.edges[0].output_port=2;
        desktop::NumericDraftPresentation display;display.node_adapters={{"1",QJsonObject{{"device_identity","native-A"}}}};
        desktop::NumericProjectDocument project;domain::NumericProcessingGraph roundtrip;desktop::NumericDraftPresentation restored;
        check(desktop::create_numeric_project(saved,display,project) && desktop::read_numeric_draft(project,roundtrip) &&
            desktop::read_numeric_presentation(project,restored),"Pinned multichannel project roundtrip");
        check(native.set_numeric_graph(roundtrip,&error,restored.node_adapters) &&
            native.numeric_graph().edges==saved.edges && domain::data_outputs(*native.delegateModel<AudioNodeModel>(1)->numeric_node()).size()==3 &&
            native.delegateModel<AudioNodeModel>(1)->adapter().value("device_identity")=="native-A",
            "Default mono device must not remove saved pinned output three before adapter import");
        check(native.export_numeric_graph(exported,&error) && exported.edges==saved.edges && domain::data_outputs(exported.nodes[0]).size()==3,
            "Pinned output three survives Apply without silent rewiring");
        const auto pinnedBefore=native.save();const auto pinnedUndo=history.index();
        disappearing_native_output(native,roundtrip,restored.node_adapters,devices,history);
        devices[0]=QJsonObject{{"id","native-A"},{"is_default",false},{"native_sample_rate",999},{"native_channels",3}};
        check(!native.set_numeric_graph(roundtrip,&error,restored.node_adapters) && native.save()==pinnedBefore && history.index()==pinnedUndo &&
            native.delegateModel<AudioNodeModel>(1)->adapter().value("device_identity")=="native-A",
            "Failed pinned import preserves graph, adapter binding and Undo atomically");
}
void native_format_admission(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel native(registry);QUndoStack history;native.set_undo_stack(&history);
        QJsonArray devices{QJsonObject{{"id","native-A"},{"is_default",true},{"native_sample_rate",44100},{"native_channels",3}},
            QJsonObject{{"id","native-B"},{"native_sample_rate",32000},{"native_channels",1}}};
        native.set_admission_limits(numeric_editor::limits());native.set_capture_device_provider([&](int,QString*){return devices;});
        domain::NumericProcessingGraph imported;imported.graph_id="main";
        auto source=numeric_editor::default_node("AudioSource");source.id=1;
        auto view=numeric_editor::default_node("AudioView");view.id=2;view.inputs[0].contract=source.outputs[0].contract;
        imported.nodes={source,view};imported.edges={{1,2,0,0}};QString error;
        check(native.set_numeric_graph(imported,&error),"Native-format import must preflight before publication");
        auto* audio=native.delegateModel<AudioNodeModel>(1);
        check(audio && domain::data_outputs(*audio->numeric_node()).size()==3 &&
            std::get<signal_contracts::NumericContract>(audio->numeric_node()->outputs[0].contract.view()).clock.sample_rate==44100,
            "Imported draft displays actual source outputs and native Fs");
        domain::NumericProcessingGraph exported;QByteArray shown,applied;
        const bool exportedNative=native.export_numeric_graph(exported,&error);
        const auto shownResult=desktop::encode_numeric_graph(native.numeric_graph(),shown);
        const auto appliedResult=desktop::encode_numeric_graph(exported,applied);
        const bool encodedShown=bool(shownResult),encodedApplied=exportedNative && bool(appliedResult);
        if(!exportedNative || !encodedShown || !encodedApplied || shown!=applied)
            std::fprintf(stderr,"native import/export diagnostic error=%s exported=%d shownError=%s appliedError=%s shown=%s applied=%s\n",
                qPrintable(error),int(exportedNative),qPrintable(shownResult.error),qPrintable(appliedResult.error),shown.constData(),applied.constData());
        check(exportedNative && encodedShown && encodedApplied && shown==applied,"Apply must export exactly the displayed native draft");
        check(native.edit_adapter(1,{{"device_identity","native-B"}}) && domain::data_outputs(*audio->numeric_node()).size()==1 &&
            std::get<signal_contracts::NumericContract>(audio->numeric_node()->outputs[0].contract.view()).clock.sample_rate==32000,
            "Pinned native device changes displayed shape and Fs atomically");
        // Import contains no adapter preferences: its default is not the prior
        // document's pinned device, even when node IDs coincide.
        check(native.set_numeric_graph(imported,&error) && domain::data_outputs(*native.delegateModel<AudioNodeModel>(1)->numeric_node()).size()==3,
            "New draft must not inherit prior pinned device by coincident node ID");
native_import_refusals(native,history,devices,imported,error,exported);

    }

void layout_capture_font(const QString& artifactDirectory,QWidget* body,QWidget* inspector){
        if(!artifactDirectory.isEmpty()) {
            check(QDir().mkpath(artifactDirectory),"Create requested node layout artifact directory");
            // The offscreen platform may omit Windows system font discovery.
            // Load a Cyrillic-capable font explicitly for these two owned
            // fixture trees, before measuring their layout or capturing them.
            const QString windowsDirectory=qEnvironmentVariable("WINDIR","C:/Windows");
            const int fontId=QFontDatabase::addApplicationFont(QDir(windowsDirectory).filePath("Fonts/segoeui.ttf"));
            const auto families=QFontDatabase::applicationFontFamilies(fontId);
            check(fontId>=0 && !families.isEmpty(),"Load readable Windows font for requested widget captures");
            if(!families.isEmpty()) {
                const QFont captureFont(families.front(),9);
                check(QFontMetrics(captureFont).inFontUcs4(0x0416),"Capture font supports Cyrillic glyphs");
                body->setFont(captureFont);
                inspector->setFont(captureFont);
            }
        }

}
void tall_inspector_geometry(const std::unique_ptr<QWidget>& inspector,const QString& artifactDirectory,const auto& settle,const auto& capture){
        inspector->resize(640,inspector->sizeHint().height());
        inspector->layout()->setGeometry(inspector->rect());
        settle();
        std::vector<std::pair<QComboBox*,int>> compactChoiceHeights;
        for(const auto* key:{"numericRounding","numericNan_type"}) {
            auto* choice=inspector->findChild<QComboBox*>(key);
            check(choice && choice->isVisibleTo(inspector.get()),"Tall inspector fixture contains processing and contract selectors");
            if(choice)compactChoiceHeights.emplace_back(choice,choice->height());
        }
        struct CompactRowGeometry { ui::FieldRow* row; int height; int labelEditorGap; };
        std::vector<CompactRowGeometry> compactRows;
        for(const auto* key:{"numericRounding_row","numericNan_row"}) {
            auto* row=dynamic_cast<ui::FieldRow*>(inspector->findChild<QWidget*>(key));
            check(row && row->isVisibleTo(inspector.get()),"Tall inspector fixture contains visible parameter rows");
            if(row)compactRows.push_back({row,row->height(),
                row->editor()->mapTo(row,QPoint{}).y()-row->label()->geometry().bottom()});
        }
        inspector->resize(inspector->width(),inspector->height()+600);
        inspector->layout()->setGeometry(inspector->rect());
        settle();
        for(const auto& [choice,height]:compactChoiceHeights)
            check(choice->height()==height,"Processing and contract selectors retain native height when inspector grows vertically");
        for(const auto& compact:compactRows) {
            check(compact.row->height()==compact.height,"Parameter rows remain compact when inspector grows vertically");
            const int gap=compact.row->editor()->mapTo(compact.row,QPoint{}).y()-compact.row->label()->geometry().bottom();
            check(gap==compact.labelEditorGap,"Tall inspector does not add space between parameter labels and editors");
        }
        if(!artifactDirectory.isEmpty()) {
            inspector->ensurePolished();
            capture(inspector.get(),"node-inspector-fields-collapsed.png");
        }

}
void inline_resize_and_shrink(QtNodes::DataFlowGraphicsScene& scene,QtNodes::NodeId id,QJsonObject& geometrySettings,QWidget* body,const std::unique_ptr<QWidget>& inspector,int collapsedHeight,const auto& settle,const auto& capture,const auto& verify){
        for(int width:{400,140,240}) {
            geometrySettings["node_width"]=width;
            scene.nodeGeometry().recomputeSize(id);settle();verify();
            if(width==140)capture(body,"node-inline-narrow.png");
            auto* choice=body->findChild<QComboBox*>("numericRounding");
            check(choice && choice->count()==2,"Pinned processing selector remains available after resize");
            if(choice)choice->setCurrentIndex(1-choice->currentIndex());
            settle();verify();
        }
        // Compare the shrink result at the same width and polished font/style
        // as the baseline, independently of the preceding resize/capture steps.
        geometrySettings["node_width"]=140;
        scene.nodeGeometry().recomputeSize(id);settle();verify();
        for(const auto* key:{"numericNan","numericRounding"})
            if(auto* action=inspector->findChild<QAction*>(QString("showOnBlock_")+key))action->setChecked(false);
        settle();
        if(body->height()!=collapsedHeight)
            std::fprintf(stderr,"Inline shrink height: baseline=%d actual=%d width=%d preferred=%d hint=%d minimum=%d\n",
                         collapsedHeight,body->height(),body->width(),geometrySettings["node_width"].toInt(),
                         body->sizeHint().height(),body->minimumSizeHint().height());
        check(body->height()==collapsedHeight,"Removing inline controls releases their previous height");
}
void verify_inline_geometry(QtNodes::DataFlowGraphicsScene& scene,QtNodes::NodeId id,QWidget* body){
            const auto bounds=scene.nodeGeometry().boundingRect(id);
            check(bounds.contains(QRectF(scene.nodeGeometry().widgetPosition(id),body->size())),
                  "Dynamic node bounds contain all embedded controls");
            int previousBottom=-1;
            for(const auto children=body->findChildren<QWidget*>();auto* child:children) {
                auto* row=dynamic_cast<ui::FieldRow*>(child);
                if(!row)continue;
                if(!row->isVisibleTo(body))continue;
                auto* editor=row->editor();
                check(editor->width()>=editor->minimumSizeHint().width(),"Inline editors retain their usable minimum width");
                const auto top=row->mapTo(body,QPoint{}).y();
                check(top>=previousBottom && top+row->height()<=body->height(),"Pinned rows neither overlap nor overflow the body");
                previousBottom=top+row->height();
            }
        }
void inline_field_layout(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel layoutGraph(registry);
        QtNodes::DataFlowGraphicsScene scene(layoutGraph);
        QJsonObject geometrySettings{{"node_width",140}};
        scene.setNodeGeometry(std::make_unique<EditorNodeGeometry>(layoutGraph,[&]{return geometrySettings;}));
        const auto id=layoutGraph.addNode("Converter");
        auto* node=layoutGraph.delegateModel<AudioNodeModel>(id);
        auto* body=node->embeddedWidget();
        std::unique_ptr<QWidget> inspector(node->parameter_widget());
        auto settle=[] { for(int i=0;i<8;++i)QApplication::processEvents(); };
        const QString artifactDirectory=qEnvironmentVariable("UI_FIX_ARTIFACTS");
        layout_capture_font(artifactDirectory,body,inspector.get());
        auto capture=[&](QWidget* widget,const char* name) {
            if(artifactDirectory.isEmpty())return;
            // Render only this fixture's widget tree; no window activation,
            // desktop capture or user input is involved.
            widget->ensurePolished();
            if(widget->layout())widget->layout()->activate();
            const auto image=widget->grab();
            check(!image.isNull() && image.save(QDir(artifactDirectory).filePath(QString::fromUtf8(name))),
                  "Save requested node layout widget artifact");
        };
        body->ensurePolished();
        inspector->ensurePolished();
        scene.nodeGeometry().recomputeSize(id);
        settle();
        capture(body,"node-inline-collapsed-baseline.png");
        scene.nodeGeometry().recomputeSize(id);settle();
        const auto collapsedHeight=body->height();
        const auto collapsible=inspector->findChildren<ui::CollapsibleSection*>();
        check(collapsible.size()==1 && collapsible.front()->objectName()=="fieldsOnBlockSection",
              "Only field placement is collapsible; parameters stay visible");
        for(const auto* key:{"numericRounding_row","numericResetSettings_row"}) {
            auto* row=inspector->findChild<QWidget*>(key);
            check(row && row->isVisibleTo(inspector.get()),"Processing, contracts and reset are visible without expansion");
        }
        for(const auto* key:{"numericNan","numericRounding"}) {
            auto* action=inspector->findChild<QAction*>(QString("showOnBlock_")+key);
            check(action,"Inline geometry fixture has placement action");
            if(action)action->setChecked(true);
        }
        settle();
        check(body->height()>collapsedHeight,"Adding inline controls grows the existing node body automatically");
        auto verify=[&]{verify_inline_geometry(scene,id,body);};
        verify();
        capture(body,"node-inline-three-fields.png");
        tall_inspector_geometry(inspector,artifactDirectory,settle,capture);
        inline_resize_and_shrink(scene,id,geometrySettings,body,inspector,collapsedHeight,settle,capture,verify);

    }

void view_contract_adoption(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        for(const auto* kind:{"AudioView","WindowView","WindowedView","FFTView","MelView","SpectrogramView","ScalarView"}) {
            NumericEditorGraphModel views(registry);QtNodes::DataFlowGraphicsScene scene(views);
            const QString sourceKind=QString(kind)=="ScalarView"?"ScalarReduce":QString(kind)=="SpectrogramView"?"Spectrogram":"Window";
            const auto source=views.addNode(sourceKind),target=views.addNode(kind);const QtNodes::ConnectionId edge{source,0,target,0};
            auto* node=views.delegateModel<AudioNodeModel>(target);
            check(!node->parameter_model()->field("input0Rank") && !node->parameter_model()->field("input0Extent0") && !node->parameter_model()->field("input0Dtype"),"View dimensions/types belong to upstream contract");
            const auto before=views.save();check(views.connectionPossible(edge),"View accepts its structural rank without semantic gate");scene.undoStack().push(new QtNodes::ConnectCommand(&scene,edge));
            check(signal_contracts::same_contract(views.delegateModel<AudioNodeModel>(source)->numeric_node()->outputs[0].contract.view(),node->numeric_node()->inputs[0].contract.view()),"Compatible view adopts full producer contract");
            check(!node->portCaption(QtNodes::PortType::In,0).contains("4096") && !node->portCaption(QtNodes::PortType::In,0).contains("400"),"View labels expose dtype and rank without dimensions");
            scene.undoStack().undo();check(views.save()==before,"Rank-compatible connect Undo restores entire document");scene.undoStack().redo();
            const auto other=views.addNode(sourceKind=="Window"?"Spectrogram":"Window"),emptyTarget=views.addNode(kind);const auto unchanged=views.save();
            check(views.connectionPossible({other,0,emptyTarget,0}) && views.save()==unchanged,"Rank mismatch remains structurally connectable without mutating the draft during hover");
            views.addConnection({other,0,emptyTarget,0});
            domain::NumericProcessingGraph invalidDraft;
            check(!views.export_numeric_graph(invalidDraft),"Rank-incompatible draft refuses executable export");
        }
        NumericEditorGraphModel metrics(registry);metrics.set_admission_limits(numeric_editor::limits());QUndoStack undo;metrics.set_undo_stack(&undo);
        const auto id=metrics.addNode("ScalarView");auto* metric=metrics.delegateModel<AudioNodeModel>(id);
        check(metric->add_scalar_input() && metric->numeric_node()->inputs.size()==2,"Metrics support independent scalar inputs");
        check(metric->edit_scalar_input_name(1,"Exact count") && metric->set_scalar_input_enabled(1,false),"Metric names and visibility are editable presentation");
        auto numeric=metrics.numeric_graph();auto contract=std::get<signal_contracts::NumericContract>(numeric.nodes[0].inputs[1].contract.view());contract.dtype=signal_contracts::DType::UInt64;
        check(numeric_editor::set_contract(numeric.nodes[0],domain::PortDirection::Input,1,contract) && metrics.edit_node(id,numeric.nodes[0]),"Metric inputs have independent primitive dtype contracts");
        const auto beforeRemove=metrics.save();const auto undoBefore=undo.index();check(metric->remove_scalar_input(1) && metric->numeric_node()->inputs.size()==1 && undo.index()==undoBefore+1,"Metric removal is one topology plus presentation Undo");
        undo.undo();check(metrics.save()==beforeRemove,"Metric removal Undo restores names visibility and contract");
        auto copied=metrics.saveNode(id);copied["id"]=70;metrics.loadNode(copied);check(metrics.delegateModel<AudioNodeModel>(70)->presentation()==metric->presentation(),"Metric clipboard retains input names and visibility");
    }

void preview_extraction(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel extraction(registry);QtNodes::DataFlowGraphicsScene scene(extraction);
        const auto source=extraction.addNode("NetworkInput"),extractor=extraction.addNode("IndexExtract"),metric=extraction.addNode("ScalarView");
        auto producer=*extraction.delegateModel<AudioNodeModel>(source)->numeric_node();
        const auto matrixTemplate=numeric_editor::default_node("Spectrogram");
        auto matrix=std::get<signal_contracts::NumericContract>(matrixTemplate.outputs[0].contract.view());
        matrix.dtype=signal_contracts::DType::UInt64;matrix.semantic=signal_contracts::Semantic::Numeric;matrix.representation.normalization=signal_contracts::Normalization::None;
        check(numeric_editor::set_contract(producer,domain::PortDirection::Output,0,matrix) && extraction.edit_node(source,producer),"Extractor matrix UInt64 producer fixture");
        const auto before=extraction.save();const QtNodes::ConnectionId edge{source,0,extractor,0};
        check(extraction.connectionPossible(edge),"Extractor adopts matrix rank and exact primitive source metadata");scene.undoStack().push(new QtNodes::ConnectCommand(&scene,edge));
        const auto* adopted=extraction.delegateModel<AudioNodeModel>(extractor)->numeric_node();
        const auto scalar=std::get<signal_contracts::NumericContract>(adopted->outputs[0].contract.view());
        if(scalar.shape.rank!=signal_contracts::Rank::Scalar || scalar.dtype!=signal_contracts::DType::UInt64 || scalar.channel_order.size()!=1)
            std::fprintf(stderr,"IndexExtract derived rank=%d dtype=%d channels=%zu\n",int(scalar.shape.rank),int(scalar.dtype),scalar.channel_order.size());
        check(signal_contracts::same_contract(adopted->inputs[0].contract.view(),producer.outputs[0].contract.view()) && scalar.shape.rank==signal_contracts::Rank::Scalar && scalar.dtype==signal_contracts::DType::UInt64 && scalar.channel_order.size()==1,"Extractor derives an exact independent scalar output");
        check(extraction.connectionPossible({extractor,0,metric,0}),"Extracted scalar feeds metrics");
        const auto accepted=extraction.save();auto malformed=*adopted;
        std::get<domain::TransformProcessingParameters>(malformed.parameters).values.push_back({"channel_index",signal_contracts::NumericLiteral{std::numeric_limits<double>::infinity()}});
        check(!extraction.edit_node(extractor,malformed) && extraction.save()==accepted,"Malformed extractor channel refuses atomically before numeric conversion");
        scene.undoStack().undo();check(extraction.save()==before,"Extractor adoption Undo restores all endpoint contracts");
    }

void native_source_contract(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel native(registry);
        const auto source=native.addNode("AudioSource"),resampler=native.addNode("PcmResampler");
        native.addConnection({source,0,resampler,0});
        native.addConnection({source,native.delegateModel<AudioNodeModel>(source)->port_index(QtNodes::PortType::Out,domain::sample_rate_output),resampler,native.delegateModel<AudioNodeModel>(resampler)->port_index(QtNodes::PortType::In,domain::input_sample_rate_input)});
        auto* model=native.delegateModel<AudioNodeModel>(resampler);
        check(model->parameter_model()->editLiteral("output_rate","32000",0) && model->parameter_model()->setBypassed(true),"Native-format fixture has disabled resampler with retained target");
        domain::NumericProcessingGraph graph;QString error;
        check(native.export_numeric_graph(graph,&error),"Native-format fixture exports");
        check(desktop::resolve_numeric_source_format(graph,source,44100,2,numeric_editor::limits(),&error),"Native source Fs change accepts disabled resampler");
        const auto resolved=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& node){return node.id==resampler;});
        check(resolved!=graph.nodes.end() && signal_contracts::same_physical_contract(resolved->inputs.front().contract.view(),resolved->outputs.front().contract.view()) && std::get<signal_contracts::NumericContract>(resolved->outputs.front().contract.view()).clock.sample_rate==44100,"Native negotiation retains exact bypass geometry and physical Fs");
        bool targetRetained=false;if(resolved!=graph.nodes.end())for(const auto& parameter:std::get<domain::TransformProcessingParameters>(resolved->parameters).values)if(parameter.key=="output_rate")targetRetained=std::visit([](auto value){return value==32000;},std::get<signal_contracts::NumericLiteral>(parameter.value));
        check(targetRetained,"Native Fs negotiation preserves requested resampling target");
    }

void source_rate_undo(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel rates(registry);QUndoStack history;rates.set_undo_stack(&history);
        const auto genericSource=rates.addNode("NetworkInput"),accumulator=rates.addNode("Window");
        auto generic=*rates.delegateModel<AudioNodeModel>(genericSource)->numeric_node();
        auto samples=std::get<signal_contracts::NumericContract>(generic.outputs[0].contract.view());
        samples.semantic=signal_contracts::Semantic::Numeric;samples.shape.extents[0]={768,768};samples.clock.tick_seconds={1,44100};samples.clock.sample_rate=44100;
        check(numeric_editor::set_contract(generic,domain::PortDirection::Output,0,samples) && rates.edit_node(genericSource,generic),"Retagged sample-vector source fixture");
        rates.addConnection({genericSource,0,accumulator,0});
        const auto* accumulated=rates.delegateModel<AudioNodeModel>(accumulator)->numeric_node();
        const QtNodes::ConnectionId draftEdge{genericSource,0,accumulator,0};
        check(rates.connectionExists(draftEdge) && rates.connection_visual_state(draftEdge)==3 && !rates.connection_failure(draftEdge).isEmpty() &&
              std::get<signal_contracts::NumericContract>(accumulated->outputs[0].contract.view()).shape.extents[0].maximum==4096,
              "Incompatible source geometry remains a diagnosed draft while Window owns its output size");
    }

void clock_rate_dependency(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel rates(registry);QUndoStack history;rates.set_undo_stack(&history);
        // Connected clipboard round-trip exercises adapter-owned policy, placement and names.
        const auto a=rates.addNode("Converter"),b=rates.addNode("Converter");
        rates.setNodeData(a,QtNodes::NodeRole::Position,QPointF{20,40});rates.setNodeData(b,QtNodes::NodeRole::Position,QPointF{240,160});
        rates.delegateModel<AudioNodeModel>(a)->edit_display_name("Exact source");
        rates.delegateModel<AudioNodeModel>(b)->edit_display_name("Exact target");
        const QtNodes::ConnectionId original{a,0,b,0};rates.addConnection(original);rates.edit_connection_policy(original,3);
        QtNodes::DataFlowGraphicsScene scene(rates);
        scene.nodeGraphicsObject(a)->setSelected(true);scene.nodeGraphicsObject(b)->setSelected(true);
        QtNodes::CopyCommand copy(&scene);copy.redo();const auto before=rates.save();
        scene.undoStack().push(new QtNodes::PasteCommand(&scene,QPointF{900,700}));
        QtNodes::NodeId ca=QtNodes::InvalidNodeId,cb=QtNodes::InvalidNodeId;
        for(auto id:rates.allNodeIds())if(id!=a && id!=b) {
            const auto title=rates.delegateModel<AudioNodeModel>(id)->caption();if(title=="Exact source")ca=id;if(title=="Exact target")cb=id;
        }
        const QtNodes::ConnectionId pasted{ca,0,cb,0};
        check(ca!=QtNodes::InvalidNodeId && cb!=QtNodes::InvalidNodeId && rates.connectionExists(pasted) && rates.connection_policy(pasted)==3,"Connected paste preserves policy and user names on new node IDs");
        check(rates.nodeData(cb,QtNodes::NodeRole::Position).toPointF()-rates.nodeData(ca,QtNodes::NodeRole::Position).toPointF()==QPointF{220,120},"Paste preserves relative node positions");
        const auto after=rates.save();scene.undoStack().undo();check(rates.save()==before,"Connected paste undo restores the entire original document");
        scene.undoStack().redo();check(rates.save()==after,"Connected paste redo restores metadata, connections and placement");
    }

void resampler_refused_settings(){
        auto malformed=numeric_editor::default_node("PcmResampler");malformed.inputs.clear();
        check(!numeric_editor::set_sample_rate(malformed,44100),"Malformed resampler rate edit must fail without indexing a missing input");
        auto belowResampler=numeric_editor::default_node("PcmResampler");
        const auto initialResampler=belowResampler.outputs[0].contract;
        check(!numeric_editor::set_sample_rate(belowResampler,1000) && signal_contracts::same_contract(initialResampler.view(),belowResampler.outputs[0].contract.view()),"Resampler rejects 1000 Hz without changing its contract");
        auto unsupported=numeric_editor::default_node("PcmResampler");auto lowClock=std::get<signal_contracts::NumericContract>(unsupported.inputs[0].contract.view());lowClock.clock.tick_seconds={1,8000};lowClock.clock.sample_rate=8000;
        check(numeric_editor::set_contract(unsupported,domain::PortDirection::Input,0,lowClock) && !numeric_editor::set_sample_rate(unsupported,192000),"Editor refuses unsupported resampling ratios before device-specific validation");

}
void resampler_connection_undo(NumericEditorGraphModel& rates,QtNodes::NodeId resampler,QtNodes::NodeId fresh,const QJsonObject& beforeConnection,QtNodes::DataFlowGraphicsScene& scene){
        const QtNodes::ConnectionId edge{resampler,0,fresh,0};
        scene.undoStack().push(new QtNodes::ConnectCommand(&scene,edge));
        check(rates.connectionExists(edge),"Connection adapts default downstream clock and PCM block bounds");
        scene.undoStack().push(new QtNodes::DisconnectCommand(&scene,edge));
        const auto disconnected=std::get<signal_contracts::NumericContract>(rates.delegateModel<AudioNodeModel>(fresh)->numeric_node()->inputs[0].contract.view());
        check(!rates.connectionExists(edge) && disconnected.clock.sample_rate==32000,"Ordinary disconnect retains known clock for typed-zero continuity");
        scene.undoStack().undo();check(rates.connectionExists(edge),"Undo disconnect must restore the connection with its current contract");
        scene.undoStack().undo();check(rates.save()==beforeConnection,"Undo connection creation restores adapted downstream contracts atomically");
        scene.undoStack().redo();check(rates.connectionExists(edge),"Redo connection creation adapts downstream clock again");

}
void resampler_card_render(NumericEditorGraphModel& rates,QtNodes::NodeId resampler,QtNodes::NodeId selector,QtNodes::NodeId sink,QWidget* body,QtNodes::DataFlowGraphicsScene& scene){
        scene.setNodeGeometry(std::make_unique<EditorNodeGeometry>(rates,[]{return QJsonObject{};}));
        scene.setNodePainter(std::make_unique<EditorNodePainter>());
        auto& geometry=static_cast<EditorNodeGeometry&>(scene.nodeGeometry());geometry.recomputeSize(resampler);geometry.recomputeSize(selector);
        check(!geometry.bypassRect(resampler).isEmpty() && geometry.bypassRect(selector).isEmpty() && geometry.bypassRect(sink).isEmpty(),"Header bypass belongs only to meaningful processing nodes");
        check(!body->findChild<QCheckBox*>("numericBypass"),"Card body does not duplicate the header bypass");
        auto* card=scene.nodeGraphicsObject(resampler);card->updateQWidgetEmbedPos();
        for(const auto items=scene.items();auto* item:items)if(!item->parentItem())item->setVisible(item==card);
        const auto oldTheme=ui::Theme::instance().code();
        const auto oldLanguage=ui::Language::instance().code();
        for(const auto& language:{QString("en"),QString("ru")})for(const auto& theme:{QString("light"),QString("dark")}){
            ui::Language::instance().set(language);
            ui::Theme::instance().set(theme);QCoreApplication::processEvents();geometry.recomputeSize(resampler);card->updateQWidgetEmbedPos();
            const auto bounds=card->sceneBoundingRect();QImage image(bounds.size().toSize()+QSize(24,24),QImage::Format_ARGB32_Premultiplied);image.fill(QApplication::palette().color(QPalette::Window));
            QPainter painter(&image);scene.render(&painter,QRectF(12,12,bounds.width(),bounds.height()),bounds);painter.end();
            const QDir evidence(QDir(QString::fromUtf8(QT_TESTCASE_SOURCEDIR)).absoluteFilePath("../.agent-workspace"));
            check(image.save(evidence.filePath("blocks17-resampler-"+language+"-"+theme+".png")),"Own scene renderer image saved");
            if(language=="en")check(image.save(evidence.filePath("blocks17-resampler-"+theme+".png")),"English scene renderer image saved");
        }
        ui::Theme::instance().set(oldTheme);
        ui::Language::instance().set(oldLanguage);

}
void resampler_inline_bypass(NumericEditorGraphModel& rates,QtNodes::NodeId resampler,QtNodes::NodeId sink,AudioNodeModel* resamplerModel,QtNodes::DataFlowGraphicsScene& scene){
        check(!rates.delegateModel<AudioNodeModel>(sink)->parameter_model()->supportsBypass(),"Graph has no processing bypass");
        const auto selector=rates.addNode("InputSelector");check(!rates.delegateModel<AudioNodeModel>(selector)->parameter_model()->supportsBypass(),"Selector cannot bypass its routing decision");
        for(const auto* kind:{"Gate","ArrayLength","ArrayFill","MatrixTranspose"}){AudioNodeModel node(kind);check(!node.parameter_model()->supportsBypass(),"Same storage alone does not define an identity bypass for routing, metadata or geometry changes");}
        auto* body=resamplerModel->embeddedWidget();auto* rateEdit=body->findChild<QLineEdit*>("output_rate");
        check(rateEdit!=nullptr,"Resampler inline card exposes a real rate editor");
        if(rateEdit){rateEdit->setText("36000");rateEdit->setModified(true);QMetaObject::invokeMethod(rateEdit,"editingFinished",Qt::DirectConnection);}
        check(std::get<signal_contracts::NumericContract>(resamplerModel->numeric_node()->outputs[0].contract.view()).clock.sample_rate==36000,"Actual inline field commits 36000 Hz");
        check(resamplerModel->parameter_model()->setBypassed(true),"Resampler bypass can be enabled");
        check(signal_contracts::same_physical_contract(resamplerModel->numeric_node()->inputs[0].contract.view(),resamplerModel->numeric_node()->outputs[0].contract.view()),"Resampler bypass preserves input geometry and Fs");
        check(std::visit([](auto v){return double(v);},std::get<signal_contracts::NumericLiteral>(resamplerModel->parameter_model()->field("output_rate")->read()))==36000,"Configured target remains visible while bypassed");
        check(resamplerModel->parameter_model()->setBypassed(false),"Resampler processing resumes");
        check(std::get<signal_contracts::NumericContract>(resamplerModel->numeric_node()->outputs[0].contract.view()).clock.sample_rate==36000,"Leaving bypass restores the requested 36000 Hz");
        resampler_card_render(rates,resampler,selector,sink,body,scene);

}
void resampler_editor(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel rates(registry);QUndoStack history;rates.set_undo_stack(&history);
        resampler_refused_settings();
        const auto source=rates.addNode("SignalGenerator"),resampler=rates.addNode("PcmResampler"),window=rates.addNode("Window"),sink=rates.addNode("WindowView");
        rates.addConnection({source,0,resampler,0});
        rates.addConnection({source,rates.delegateModel<AudioNodeModel>(source)->port_index(QtNodes::PortType::Out,domain::sample_rate_output),resampler,
            rates.delegateModel<AudioNodeModel>(resampler)->port_index(QtNodes::PortType::In,domain::input_sample_rate_input)});
        rates.addConnection({resampler,0,window,0});
        rates.addConnection({resampler,rates.delegateModel<AudioNodeModel>(resampler)->port_index(QtNodes::PortType::Out,domain::sample_rate_output),window,
            rates.delegateModel<AudioNodeModel>(window)->port_index(QtNodes::PortType::In,domain::input_sample_rate_input)});
        rates.addConnection({window,0,sink,0});
        const auto sampleClock=rates.addNode("SampleClock");auto bound=*rates.delegateModel<AudioNodeModel>(source)->numeric_node();bound.clock_node=sampleClock;check(rates.edit_node(source,bound),"Explicit resampling source clock binding");
        auto* resamplerModel=rates.delegateModel<AudioNodeModel>(resampler);
        check(rates.delegateModel<AudioNodeModel>(sampleClock)->parameter_model()->edit("clockSampleRate",44100),"Source rate propagates up to explicit resampler boundary");
        const auto resampleInput=std::get<signal_contracts::NumericContract>(resamplerModel->numeric_node()->inputs[0].contract.view());
        const auto resampleOutput=std::get<signal_contracts::NumericContract>(resamplerModel->numeric_node()->outputs[0].contract.view());
        if(resampleInput.clock.sample_rate!=44100 || resampleOutput.clock.sample_rate!=48000)
            std::fprintf(stderr,"Resampler initial rates source=%u input=%u output=%u sourceFs=%d downstreamFs=%d\n",std::get<signal_contracts::NumericContract>(rates.delegateModel<AudioNodeModel>(source)->numeric_node()->outputs[0].contract.view()).clock.sample_rate,resampleInput.clock.sample_rate,resampleOutput.clock.sample_rate,
                int(rates.connectionExists({source,rates.delegateModel<AudioNodeModel>(source)->port_index(QtNodes::PortType::Out,domain::sample_rate_output),resampler,resamplerModel->port_index(QtNodes::PortType::In,domain::input_sample_rate_input)})),
                int(rates.connectionExists({resampler,resamplerModel->port_index(QtNodes::PortType::Out,domain::sample_rate_output),window,rates.delegateModel<AudioNodeModel>(window)->port_index(QtNodes::PortType::In,domain::input_sample_rate_input)})));
        check(resampleInput.clock.sample_rate==44100 && resampleOutput.clock.sample_rate==48000 && resampleOutput.shape.extents[0].maximum==(256*48000+44100-1)/44100+1,"Resampler retains its own output rate and admitted variable block geometry");
        check(resamplerModel->parameter_model()->field("output_rate") && resamplerModel->parameter_model()->editLiteral("output_rate","32000",0),"Resampler exposes editable output sample rate");
        const auto downstream=std::get<signal_contracts::NumericContract>(rates.delegateModel<AudioNodeModel>(window)->numeric_node()->outputs[0].contract.view());
        if(downstream.clock.sample_rate!=32000)std::fprintf(stderr,"Resampler downstream rates window=%u resampler=%u\n",downstream.clock.sample_rate,std::get<signal_contracts::NumericContract>(resamplerModel->numeric_node()->outputs[0].contract.view()).clock.sample_rate);
        check(downstream.clock.sample_rate==32000,"Resampler output-rate change re-clocks its downstream chain");
        const auto fresh=rates.addNode("AudioView");const auto beforeConnection=rates.save();
        QtNodes::DataFlowGraphicsScene scene(rates);
        resampler_connection_undo(rates,resampler,fresh,beforeConnection,scene);
        check(resamplerModel->port_index(QtNodes::PortType::In,domain::target_sample_rate_input)==QtNodes::InvalidPortIndex,"No external resampler target command input");
        check(resamplerModel->port_index(QtNodes::PortType::Out,domain::sample_rate_output)!=QtNodes::InvalidPortIndex,"Resampler publishes Fs on the right");
        check(resamplerModel->parameter_model()->editLiteral("output_rate","96000",0),"Local resampler target edit");
        const auto boundOutput=std::get<signal_contracts::NumericContract>(resamplerModel->numeric_node()->outputs[0].contract.view());
        check(boundOutput.clock.sample_rate==96000 && resamplerModel->numeric_node()->clock_node==-1,"Local setting changes output Fs");
        domain::NumericProcessingGraph resolvedTarget;QString targetError;const auto targetPrepared=rates.export_numeric_graph(resolvedTarget,&targetError);
        if(!targetPrepared)std::fprintf(stderr,"Resampler export rejected: %s\n",targetError.toUtf8().constData());
        check(targetPrepared,"Local resampler target agrees with prepared contract");
        resampler_inline_bypass(rates,resampler,sink,resamplerModel,scene);

    }

void delivery_diagnostics(NumericEditorGraphModel& graph,QUndoStack& undo){

        const auto source=graph.addNode("AudioSource"),view=graph.addNode("Converter");auto* target=graph.delegateModel<AudioNodeModel>(view);

        const QtNodes::ConnectionId edge{source,0,view,0};graph.addConnection(edge);
        check(graph.connection_visual_state(edge)==0,"Exact connection is observed before its contract changes");
        auto changed=*target->numeric_node();
        check(numeric_editor::set_dtype(changed,domain::PortDirection::Input,0,signal_contracts::DType::UInt16) && graph.edit_node(view,changed),
              "Connected converter retains an explicit UInt16 input contract");
        if(!graph.connectionExists(edge) || graph.connection_visual_state(edge)!=2 || !graph.connection_effects(edge).contains("65535"))
            std::fprintf(stderr,"Converter risk edge=%d state=%d effects=%s\n",int(graph.connectionExists(edge)),graph.connection_visual_state(edge),graph.connection_effects(edge).toUtf8().constData());

        check(graph.connectionExists(edge) && graph.connection_visual_state(edge)==2 && graph.connection_effects(edge).contains("65535"),"float to uint16 edge exposes potential range and precision loss");

        graph.edit_connection_policy(edge,3);check(graph.connection_policy(edge)==3 && graph.numeric_graph().edges.back().conversion==3,"edge policy is actual processing metadata");

        undo.undo();check(graph.connection_policy(edge)==0,"connection policy undo restores exact rejection");

        auto context=std::make_shared<pipeline::NumericDeliveryContext>();context->endpoint={"main",domain::NodeId(source),domain::PortDirection::Output,0};context->contract=graph.delegateModel<AudioNodeModel>(source)->numeric_node()->outputs[0].contract;

        check(graph.delegateModel<AudioNodeModel>(source)->preview(pipeline::DeliveredNumericFrame{context,{1,1},{},signal_contracts::Status::overflow}) && graph.connection_visual_state(edge)==3 && !graph.connection_failure(edge).isEmpty(),"reported kernel overflow changes connection to observed failure");

        graph.delegateModel<AudioNodeModel>(source)->clear_preview();check(graph.connection_visual_state(edge)==2,"clearing failed delivery restores potential risk state");

        graph.deleteNode(view);graph.deleteNode(source);

    }

void grouped_selector_envelope(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        {
            NumericEditorGraphModel grouped(registry);
            const auto source=grouped.addNode("AudioSource"),destination=grouped.addNode("InputSelector");
            auto node=*grouped.delegateModel<AudioNodeModel>(destination)->numeric_node();
            check(numeric_editor::set_selector_channels(node,2),"Create grouped selector for chunk-size adaptation");
            auto envelope=node.outputs.front().contract.view();
            std::get<signal_contracts::NumericContract>(envelope).shape.extents[0]={1,8192};
            domain::OwnedProcessingContract owned;
            check(domain::own_processing_contract(envelope,numeric_editor::limits().frame,numeric_editor::limits().max_owned_metadata_bytes,owned)==signal_contracts::Status::ok,"Own selector output envelope");
            for(auto& port:node.outputs)port.contract=owned;
            check(numeric_editor::set_input_count(node,3) && grouped.edit_node(destination,node),"Add grouped source stream with bounded input envelope");
            grouped.addConnection({source,0,destination,4});grouped.addConnection({source,0,destination,5});
            const auto* actual=grouped.delegateModel<AudioNodeModel>(destination)->numeric_node();
            check(grouped.connectionExists({source,0,destination,4}) && grouped.connectionExists({source,0,destination,5}),"New selector group accepts both fixed-size source lanes");
            check(signal_contracts::same_contract(actual->outputs[0].contract.view(),owned.view()) && signal_contracts::same_contract(actual->outputs[1].contract.view(),owned.view()) &&
                  signal_contracts::same_contract(actual->inputs[0].contract.view(),node.inputs[0].contract.view()),"Connecting a new group preserves output envelope and earlier group contracts");
        }

}
void selector_growth_undo(NumericEditorGraphModel& graph,QUndoStack& undo,AudioNodeModel* model,QtNodes::NodeId generator,QtNodes::NodeId audio,QtNodes::NodeId selector){
        QObject selectorWitness;int selectorPublications=0;bool selectorPublicationComplete=true;
        QObject::connect(&graph,&NumericEditorGraphModel::atomic_edit_finished,&selectorWitness,[&]{
            ++selectorPublications;
            selectorPublicationComplete&=graph.connectionExists({generator,0,selector,0}) && graph.connectionExists({audio,0,selector,1}) && !graph.atomic_edit_in_progress();
        });
        model->select_input(1);
        if(selectorPublications!=1 || !selectorPublicationComplete || model->selected_input()!=1)
            std::fprintf(stderr,"Selector edit state publications=%d complete=%d selected=%u wires=%d/%d\n",selectorPublications,int(selectorPublicationComplete),unsigned(model->selected_input()),int(graph.connectionExists({generator,0,selector,0})),int(graph.connectionExists({audio,0,selector,1})));
        check(selectorPublications==1 && selectorPublicationComplete && model->selected_input()==1,"Selector parameter edit publishes one complete graph after restoring incident wires");
        undo.undo();check(model->selected_input()==0,"selector undo selection");undo.redo();
        check(selectorPublications==3 && selectorPublicationComplete,"Selector undo and redo publish complete graphs for live Apply");
        auto changed=*model->numeric_node();check(numeric_editor::set_input_count(changed,3) && graph.edit_node(selector,changed),"add selector input");
        QApplication::processEvents();
        check(model->numeric_node()->inputs.size()==3 && graph.connectionExists({generator,0,selector,0}) && graph.connectionExists({audio,0,selector,1}),"retained stable edges after growth");
        graph.addConnection({generator,0,selector,2});model->select_input(2);
        changed=*model->numeric_node();numeric_editor::set_input_count(changed,2);const auto tailRemoved=graph.edit_node(selector,changed);
        if(!tailRemoved)std::fprintf(stderr,"Selector tail removal rejected inputs=%zu edges=%zu\n",model->numeric_node()->inputs.size(),graph.numeric_graph().edges.size());
        check(tailRemoved,"remove connected tail input");
        check(!graph.connectionExists({generator,0,selector,2}) && graph.connectionExists({audio,0,selector,1}),"only removed tail edge disappears");
        QObject tailUndoWitness;int tailUndoPublications=0;bool tailUndoComplete=true;
        QObject::connect(&graph,&NumericEditorGraphModel::atomic_edit_finished,&tailUndoWitness,[&]{
            ++tailUndoPublications;tailUndoComplete&=model->numeric_node()->inputs.size()==3 && graph.connectionExists({generator,0,selector,2}) && !graph.atomic_edit_in_progress();
        });
        undo.undo();check(model->numeric_node()->inputs.size()==3 && graph.connectionExists({generator,0,selector,2}),"undo count restores removed edge");
        check(tailUndoPublications==1 && tailUndoComplete,"Structural undo publishes only after the removed tail edge is restored");
        QObject::disconnect(&graph,nullptr,&tailUndoWitness,nullptr);
        changed=*model->numeric_node();numeric_editor::set_input_count(changed,1);check(graph.edit_node(selector,changed) && graph.connectionExists({generator,0,selector,0}) && !graph.connectionExists({audio,0,selector,1}),"multi-tail shrink retains stable input zero");
        undo.undo();check(graph.connectionExists({audio,0,selector,1}) && graph.connectionExists({generator,0,selector,2}),"multi-tail undo restores exact IDs");

}
void selector_hit_geometry(QtNodes::DataFlowGraphicsScene& scene,QtNodes::NodeId selector,AudioNodeModel* model,QWidget* body,NumericEditorGraphModel& graph){
        for(const auto& language:{QString("ru"),QString("en")}) {
            ui::Language::instance().set(language);QApplication::processEvents();
            check(model->portCaption(QtNodes::PortType::In,2)==model->stream_name(2)+" · "+ui::text("Channel %1").arg(1),"physical input captions match stable mono stream and channel numbering");
            scene.nodeGeometry().recomputeSize(selector);
            check(scene.nodeGeometry().boundingRect(selector).contains(QRectF(scene.nodeGeometry().widgetPosition(selector),body->size())),"node bounds contain compact controls");
        }
        for(unsigned count:{1u,2u,3u,8u}) {
            auto n=*model->numeric_node();numeric_editor::set_input_count(n,count);check(graph.edit_node(selector,n) || model->nPorts(QtNodes::PortType::In)==count,"geometry count edit");
            auto& geometry=static_cast<EditorNodeGeometry&>(scene.nodeGeometry());geometry.recomputeSize(selector);
            const auto center=geometry.size(selector).height()/2.0;
            for(unsigned i=0;i<count;++i){const auto p=geometry.portPosition(selector,QtNodes::PortType::In,i);
                check(std::abs(p.y()+geometry.portPosition(selector,QtNodes::PortType::In,count-1-i).y()-2*center)<1e-6,"ports symmetric about whole node");
                check(geometry.checkPortHit(selector,QtNodes::PortType::In,p)==i && geometry.selectorButtonHit(selector,geometry.selectorButtonRect(selector,i).center())==i,"shared pin/button hit geometry");}
            const auto outputCount=model->nPorts(QtNodes::PortType::Out);
            for(unsigned i=0;i<outputCount;++i)check(std::abs(geometry.portPosition(selector,QtNodes::PortType::Out,i).y()+geometry.portPosition(selector,QtNodes::PortType::Out,outputCount-1-i).y()-2*center)<1e-6,"output data and configuration pins centered independently");
        }

}
void selector_sparse_roundtrip(NumericEditorGraphModel& graph,QUndoStack& undo,AudioNodeModel* model,QtNodes::NodeId generator,QtNodes::NodeId audio,QtNodes::NodeId selector,QByteArray& bytes,domain::NumericProcessingGraph& restored,NumericEditorGraphModel& loaded,const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        auto n=*model->numeric_node();numeric_editor::set_input_count(n,3);graph.edit_node(selector,n);
        graph.addConnection({audio,0,selector,1});graph.addConnection({generator,0,selector,2});model->select_input(2);
        n=*model->numeric_node();check(numeric_editor::remove_input(n,1) && graph.edit_node(selector,n),"remove arbitrary connected input");
        check(model->port_id(QtNodes::PortType::In,1)==2 && graph.connectionExists({generator,0,selector,1}) && model->selected_input()==2,"stable remaining input/connection/selection");
        const auto canonical=graph.numeric_graph();check(desktop::encode_numeric_graph(canonical,bytes) && desktop::decode_numeric_graph(bytes,restored) && loaded.set_numeric_graph(restored),"sparse IDs canonical round trip");
        check(loaded.connectionExists({generator,0,selector,1}) && loaded.delegateModel<AudioNodeModel>(selector)->port_id(QtNodes::PortType::In,1)==2,"sparse connection mapped on load");
        undo.undo();check(model->port_id(QtNodes::PortType::In,1)==1 && graph.connectionExists({audio,0,selector,1}) && graph.connectionExists({generator,0,selector,2}),"arbitrary delete undo exact endpoints");
        undo.redo();n=*model->numeric_node();check(numeric_editor::remove_input(n,0) && graph.edit_node(selector,n) && model->port_id(QtNodes::PortType::In,0)==2,"selector permits removal of original anchor zero");
        const auto sparseVisual=graph.save();NumericEditorGraphModel visualLoaded(registry);visualLoaded.load(sparseVisual);
        check(visualLoaded.delegateModel<AudioNodeModel>(selector)->port_id(QtNodes::PortType::In,0)==2 && visualLoaded.connectionExists({generator,0,selector,0}),"sparse visual document round trip");
        domain::NumericProcessingGraph decodedVisual;const auto visualBytes=QJsonDocument(sparseVisual).toJson(QJsonDocument::Compact);
        check(desktop::decode_numeric_editor_graph(visualBytes,decodedVisual) && std::any_of(decodedVisual.edges.begin(),decodedVisual.edges.end(),[&](const auto& e){return e.to==domain::NodeId(selector) && e.input_port==2;}),"visual indices decode to stable domain IDs");
        std::unique_ptr<QWidget> selectorForm(model->parameter_widget());auto* reset=selectorForm->findChild<QPushButton*>("numericResetSettings");check(reset,"selector reset inspector");reset->click();
        check(model->numeric_node()->inputs.size()==1 && model->port_id(QtNodes::PortType::In,0)==2 && graph.connectionExists({generator,0,selector,0}),"settings reset preserves selector topology and stable IDs");

}
int selector_topology(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry,NumericEditorGraphModel& graph,QUndoStack& undo){

        grouped_selector_envelope(registry);
        const auto generator=graph.addNode("AudioSource"),audio=graph.addNode("AudioSource"),selector=graph.addNode("InputSelector");

        graph.addConnection({generator,0,selector,0});graph.addConnection({audio,0,selector,1});

        auto* model=graph.delegateModel<AudioNodeModel>(selector);auto* body=model->embeddedWidget();

        check(!body->findChild<QComboBox*>("numericSelected") && !body->findChild<QPushButton*>("addSelectorInput"),"selector has no central strip");
        check(!body->findChild<QWidget*>("pipelineDataStatus"),"no default diagnostic card in node body");
        selector_growth_undo(graph,undo,model,generator,audio,selector);
        QByteArray bytes;domain::NumericProcessingGraph restored;
        check(desktop::encode_numeric_graph(graph.numeric_graph(),bytes) && desktop::decode_numeric_graph(bytes,restored),"N selector typed save/load");
        NumericEditorGraphModel loaded(registry);
        QString selectorLoadError;const bool loadedSelector=loaded.set_numeric_graph(restored,&selectorLoadError);
        if(!loadedSelector)std::fprintf(stderr,"Selector typed reload rejected: %s\n",selectorLoadError.toUtf8().constData());
        const auto* loadedModel=loadedSelector?loaded.delegateModel<AudioNodeModel>(selector):nullptr;
        check(loadedModel && loadedModel->numeric_node() && loadedModel->numeric_node()->inputs.size()==3,"N selector model load");
        if(!loadedModel || !loadedModel->numeric_node() || loadedModel->numeric_node()->inputs.size()!=3 || model->nPorts(QtNodes::PortType::In)<3)
            return failures?failures:1;
        QtNodes::DataFlowGraphicsScene scene(graph);scene.setNodeGeometry(std::make_unique<EditorNodeGeometry>(graph,[]{return QJsonObject{};}));
        selector_hit_geometry(scene,selector,model,body,graph);
        selector_sparse_roundtrip(graph,undo,model,generator,audio,selector,bytes,restored,loaded,registry);
        scene.clearScene();




    return 0;
}

void window_function_edit(NumericEditorGraphModel& graph,QUndoStack& undo){

        std::fprintf(stderr,"editor fixture phase window-function\n");
        const auto id=graph.addNode("WindowFunction");auto* window=graph.delegateModel<AudioNodeModel>(id);std::unique_ptr<QWidget> controls(window->parameter_widget());auto* function=controls->findChild<QComboBox*>("function");check(function && function->count()==7,"registered window-function choice editor exists");

        if(function)function->setCurrentIndex(1);

        domain::ResolvedProcessingNode resolved;domain::ProcessingParameterView value;

        check(bool(domain::resolve_processing_node(*window->numeric_node(),numeric_editor::limits(),resolved)) && domain::resolve_processing_parameter(resolved,"function",value)==signal_contracts::Status::ok && std::get<std::string_view>(value)=="hamming","window function actual edit resolves supported algorithm");

        undo.undo();check(function && function->currentIndex()!=1,"window-function undo refreshes registered editor");undo.redo();check(function && function->currentIndex()==1,"window-function redo preserves selected algorithm");graph.deleteNode(id);

    }

void network_adapter_undo(NumericEditorGraphModel& graph,QUndoStack& undo,QtNodes::NodeId source){

        auto* sourceModel=graph.delegateModel<AudioNodeModel>(source);const QJsonObject adapter{{"provider_uuid","00000000-0000-4000-8000-000000000001"},{"stream_id","spectrum-main"},{"host","127.0.0.1"},{"port",47655},{"remote_endpoint",QJsonObject{{"graph_id","main"},{"node_id",4},{"direction","output"},{"port_index",0}}}};

        QByteArray beforeAdapter,afterAdapter;desktop::encode_numeric_graph(graph.numeric_graph(),beforeAdapter);

        check(graph.edit_adapter(source,adapter) && sourceModel->adapter()==adapter,"owned registered adapter edit");desktop::encode_numeric_graph(graph.numeric_graph(),afterAdapter);check(beforeAdapter==afterAdapter,"adapter remains outside DSP parameters");

        undo.undo();check(sourceModel->adapter().isEmpty(),"adapter undo clears route metadata");undo.redo();check(sourceModel->adapter()==adapter,"adapter redo restores route metadata");

        auto invalid=adapter;invalid["port"]=70000;check(!graph.edit_adapter(source,invalid) && sourceModel->adapter()==adapter,"invalid adapter edit is atomic");

        auto copy=graph.saveNode(source);copy["id"]=50;graph.loadNode(copy);check(graph.delegateModel<AudioNodeModel>(50)->adapter()==adapter,"clipboard remapped node preserves adapter");graph.deleteNode(50);

    }

void routing_inline_count(NumericEditorGraphModel& graph,QUndoStack& undo,QtNodes::NodeId route){

        auto* routing=graph.delegateModel<AudioNodeModel>(route);
        check(!routing->parameter_on_block("numericOutputs") && routing->set_parameter_on_block("numericOutputs",true),"Switch output count can be explicitly pinned on block");
        std::unique_ptr<QWidget> routeForm(routing->parameter_widget());auto* routeBody=routing->embeddedWidget();

        auto* inlineCount=routeBody->findChild<QSpinBox*>("numericOutputs");check(inlineCount && inlineCount->isVisibleTo(routeBody),"routing selected control visible inline");

        std::fprintf(stderr,"editor fixture phase inline-routing\n");
        if(inlineCount)inlineCount->setValue(4);QApplication::processEvents();

        check(routing->numeric_node()->outputs.size()==4 && routing->parameter_model()->field("output3Dtype") && !routeForm->findChild<QAction*>("showOnBlock_output3Dtype"),"Routing grow updates modal contract fields without placing contracts on the block");

        undo.undo();QApplication::processEvents();

        check(routing->numeric_node()->outputs.size()==2 && !routeForm->findChild<QAction*>("showOnBlock_output3Dtype") && routing->parameter_on_block("numericOutputs"),"routing undo restores form shape and keeps visibility flag");

    }

void converter_clipboard(NumericEditorGraphModel& graph,QtNodes::NodeId converter,AudioNodeModel* model){

        QtNodes::DataFlowGraphicsScene scene(graph);scene.nodeGraphicsObject(converter)->setSelected(true);

        QtNodes::CopyCommand copyCommand(&scene);copyCommand.redo();const auto before=graph.allNodeIds();

        scene.undoStack().push(new QtNodes::PasteCommand(&scene,QPointF{1000,1000}));

        QtNodes::NodeId pasted=QtNodes::InvalidNodeId;for(auto id:graph.allNodeIds())if(!before.contains(id))pasted=id;

        auto* pastedNode=graph.delegateModel<AudioNodeModel>(pasted);

        check(pastedNode && pastedNode->numeric_node()->id==domain::NodeId(pasted) && domain::same_processing_parameters(model->numeric_node()->parameters,pastedNode->numeric_node()->parameters),"actual QtNodes clipboard exact typed snapshot");

        check(pastedNode && pastedNode->presentation()==model->presentation() && pastedNode->caption()==model->caption() && pasted!=converter,"actual clipboard retains user caption and visibility with a distinct stable ID");

        scene.undoStack().undo();check(!graph.nodeExists(pasted),"paste undo removes copy");scene.undoStack().redo();check(graph.nodeExists(pasted),"paste redo restores copy");

    }

void restored_graph_validation(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry,NumericEditorGraphModel& restoredGraph,const domain::NumericProcessingGraph& restored,QtNodes::NodeId sink){

        auto timed=restored;timed.execution_interval={1,40};
        check(restoredGraph.set_numeric_graph(timed),"custom analysis interval imports");
        NumericEditorGraphModel timedReload(registry);timedReload.load(restoredGraph.save());
        check(timedReload.numeric_graph().execution_interval==timed.execution_interval,"editor save/load preserves physical interval");
        auto invalidInterval=timed;invalidInterval.execution_interval={2,80};const auto intervalBefore=restoredGraph.save();
        check(!restoredGraph.set_numeric_graph(invalidInterval) && restoredGraph.save()==intervalBefore,"nonreduced interval refuses atomically");
        check(restoredGraph.set_numeric_graph(restored),"restore ordinary fixture");
        const auto before=restoredGraph.save();auto invalid=restored;

        invalid.nodes[0].kind="UnknownNumericNode";QString error;

        check(!restoredGraph.set_numeric_graph(invalid,&error) && !error.isEmpty() && restoredGraph.save()==before,"unknown node import refuses atomically");

        invalid=restored;invalid.edges.push_back({-1,domain::NodeId(sink),0,0});

        check(!restoredGraph.set_numeric_graph(invalid,&error) && restoredGraph.save()==before,"invalid edge import refuses without losing draft");

        invalid=restored;invalid.versions.processing=77;

        check(!restoredGraph.set_numeric_graph(invalid,&error) && restoredGraph.save()==before,"future processing edition refuses atomically");

        check(restoredGraph.set_numeric_graph(restored),"restore supported graph and processing editions");
        NumericEditorGraphModel versionReload(registry);versionReload.load(restoredGraph.save());
        check(versionReload.numeric_graph().versions.graph==restored.versions.graph && versionReload.numeric_graph().versions.processing==restored.versions.processing,"graph and processing editions survive draft save/load");

        NumericEditorGraphModel empty(registry);check(empty.set_numeric_graph(domain::NumericProcessingGraph{}),"empty typed draft loads");

        check(empty.has_numeric_nodes() && empty.save().value("version").toInt()==5,"empty typed draft retains source-backed save mode");

        const auto only=empty.addNode("Converter");empty.deleteNode(only);

        check(empty.has_numeric_nodes(),"deleting final typed node never falls back to legacy writer");

    }

int source_backed_project(NumericEditorGraphModel& restoredGraph,const QByteArray& bytes,QtNodes::NodeId converter,AudioNodeModel* model){

        QByteArray sourceBytes="{\"schema_version\":4,\"scene_graph\":"+bytes+",\"ui\":{\"unknown\":9007199254740993},\"app\":-0}";

        desktop::NumericProjectDocument sourceDocument,staged,reloaded;domain::NumericProcessingGraph snapshot,readback;

        check(bool(desktop::read_numeric_project(sourceBytes,sourceDocument)),"lossless project baseline");

        check(restoredGraph.export_numeric_graph(snapshot),"owned draft export");

        for(auto& node:snapshot.nodes)if(node.id==domain::NodeId(converter))std::get<domain::ConverterProcessingParameters>(node.parameters).policy.positive_inf_replacement=std::uint64_t{9007199254740993ULL};

        check(bool(desktop::stage_numeric_draft(sourceDocument,snapshot,staged)),"typed GUI snapshot stages source-backed draft");

        desktop::NumericDraftPresentation visibility;visibility.node_presentations[QString::number(converter)]=QJsonObject{
            {"inline_parameters",model->presentation().value("inline_parameters")},{"display_name",model->caption()}};

        desktop::NumericProjectDocument presented;check(bool(desktop::stage_numeric_presentation(staged,visibility,presented)),"presentation flags stage separately from processing DTO");staged=std::move(presented);

        QTemporaryDir directory;check(directory.isValid(),"temporary project directory");const auto path=directory.filePath("config.json");

        check(bool(desktop::save_numeric_project(staged,path)),"one-file draft save");

        check(bool(desktop::load_numeric_project(path,reloaded,readback)),"one-file draft reload");
        QString readbackError;
        if(!restoredGraph.set_numeric_graph(readback,&readbackError)){std::fprintf(stderr,"one-file draft import: %s\n",qPrintable(readbackError));check(false,"one-file draft imports owning graph");return 1;}

        desktop::NumericDraftPresentation loadedPresentation;check(bool(desktop::read_numeric_presentation(reloaded,loadedPresentation)),"read owning presentation after reload");

        std::fprintf(stderr,"editor converter before reloaded presentation node=%u exists=%d\n",unsigned(converter),int(restoredGraph.nodeExists(converter)));
        if(!restoredGraph.delegateModel<AudioNodeModel>(converter)){check(false,"reloaded draft retains original converter ID");return 1;}
        restoredGraph.delegateModel<AudioNodeModel>(converter)->set_presentation(loadedPresentation.node_presentations.value(QString::number(converter)).toObject());

        check(restoredGraph.delegateModel<AudioNodeModel>(converter)->parameter_on_block("numericNan"),"visibility survives one-file project reload");

        const auto& p=std::get<domain::ConverterProcessingParameters>(restoredGraph.delegateModel<AudioNodeModel>(converter)->numeric_node()->parameters).policy;

        check(std::get<std::uint64_t>(p.nan_replacement)==std::numeric_limits<std::uint64_t>::max() && std::get<std::int64_t>(p.negative_inf_replacement)==std::numeric_limits<std::int64_t>::min() && std::get<std::uint64_t>(p.positive_inf_replacement)==9007199254740993ULL,"exact UI literals survive source-backed save/reload");

        QFile file(path);check(file.open(QIODevice::ReadOnly) && file.readAll().contains("\"unknown\":9007199254740993"),"unknown raw64 preserved beside GUI edits");


    return 0;
}

int physical_endpoint_controls(NumericEditorGraphModel& graph,QUndoStack& undo){

        QJsonArray choices{QJsonObject{{"id","render-A"},{"name","Endpoint A"},{"is_default",true}},QJsonObject{{"id","render-B"},{"name","Endpoint B"}}};

        int enumerations=0;graph.set_admission_limits(numeric_editor::limits());graph.set_capture_device_provider([&](int kind,QString*){++enumerations;return kind==0?choices:QJsonArray{};});

        const auto physical=graph.addNode("AudioSource"),view=graph.addNode("AudioView");graph.addConnection({physical,0,view,0});

        check(graph.connectionExists({physical,0,view,0}),"Graph data delivery remains safe after clipboard scene destruction");

        auto* node=graph.delegateModel<AudioNodeModel>(physical);

        if(!node || !node->numeric_node()){check(false,"Fresh physical node missing numeric DTO");return 1;}

        std::unique_ptr<QWidget> controls(node->parameter_widget());

        auto* devices=controls->findChild<QComboBox*>("captureDevice");auto* refresh=controls->findChild<QPushButton*>("refreshCaptureDevices");

        if(!devices || !refresh){check(false,"Physical endpoint chooser controls missing");return 1;}

        check(devices->count()==3,"Physical endpoint chooser exposes default and discovered devices");

        const auto configured=*node->numeric_node();const auto edges=graph.allConnectionIds(physical);const int beforeDevice=undo.index();

        if(devices)devices->setCurrentIndex(devices->findData("render-B"));

        check(node->adapter().value("device_identity")=="render-B" && undo.index()==beforeDevice+1 && graph.allConnectionIds(physical)==edges && signal_contracts::same_contract(configured.outputs[0].contract.view(),node->numeric_node()->outputs[0].contract.view()),"Physical endpoint choice atomically preserves declared PCM and links");

        auto* sourceKind=controls->findChild<QComboBox*>("source");const auto beforeKind=node->save();const int beforeKindUndo=undo.index();if(sourceKind)sourceKind->setCurrentIndex(1);

        check(sourceKind && sourceKind->currentIndex()==0 && node->save()==beforeKind && undo.index()==beforeKindUndo && graph.allConnectionIds(physical)==edges,"Capture kind change silently reuses incompatible pinned endpoint");

        undo.undo();check(node->adapter().isEmpty() && devices->currentData().toString().isEmpty(),"Endpoint undo restores OS default and registered chooser");undo.redo();

        choices={QJsonObject{{"id","render-A"},{"name","Endpoint A"},{"is_default",true}}};const int beforeRefresh=undo.index();if(refresh)refresh->click();

        check(node->adapter().value("device_identity")=="render-B" && devices->currentData()=="render-B" && devices->currentText().contains(ui::text("Unavailable device: %1").arg("render-B")) && undo.index()==beforeRefresh,"Refresh silently loses unavailable pinned endpoint");

        auto unavailable=node->adapter();unavailable["device_identity"]="missing-C";check(!graph.edit_adapter(physical,unavailable) && undo.index()==beforeRefresh && graph.allConnectionIds(physical)==edges,"Unknown endpoint bypasses atomic model admission");

        if(devices)devices->setCurrentIndex(0);check(node->adapter().isEmpty() && graph.allConnectionIds(physical)==edges && enumerations>=3,"OS default endpoint repair loses links or does not discover");

        graph.deleteNode(physical);const int afterDeletion=enumerations;

        refresh->click();const auto language=ui::Language::instance().code();

        ui::Language::instance().set(language=="ru"?"en":"ru");ui::Language::instance().set(language);

        check(enumerations==afterDeletion,"External capture form accesses discovery after its model is deleted");


    return 0;
}

void frequency_step_wires(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel steps(registry);QtNodes::DataFlowGraphicsScene scene(steps);steps.set_undo_stack(&scene.undoStack());
        const auto source=steps.addNode("AudioSource"),window=steps.addNode("Window"),length=steps.addNode("ArrayLength"),delta=steps.addNode("FrequencyStep"),spectrum=steps.addNode("AmplitudePhase"),plot=steps.addNode("FFTView");
        auto connect=[&](QtNodes::NodeId from,domain::PortId out,QtNodes::NodeId to,domain::PortId in){
            const QtNodes::ConnectionId edge{from,steps.delegateModel<AudioNodeModel>(from)->port_index(QtNodes::PortType::Out,out),to,steps.delegateModel<AudioNodeModel>(to)->port_index(QtNodes::PortType::In,in)};
            check(steps.connectionPossible(edge),"Frequency step has visible connectable ordinary numeric ports");scene.undoStack().push(new QtNodes::ConnectCommand(&scene,edge));check(steps.connectionExists(edge),"Frequency step numeric wire is actually created");
        };
        connect(source,0,window,0);connect(source,domain::sample_rate_output,window,domain::input_sample_rate_input);
        connect(window,0,length,0);connect(length,0,delta,domain::window_size_input);connect(source,domain::sample_rate_output,delta,0);connect(spectrum,0,plot,0);connect(delta,0,plot,domain::frequency_step_input);
        check(!domain::node_kind("SpectrumAxis") && !numeric_editor::catalog().contains("SpectrumAxis"),"Removed frequency-axis block is unavailable");
        check(steps.delegateModel<AudioNodeModel>(plot)->numeric_node()->inputs.size()==2,"FFT graph exposes data and explicit frequency-step inputs");
        auto plotStep=[&]{const auto* fields=std::get_if<domain::EndpointProcessingParameters>(&steps.delegateModel<AudioNodeModel>(plot)->numeric_node()->parameters);for(const auto& value:fields->values)if(value.key=="bin_hz")return std::visit([](auto n){return double(n);},std::get<signal_contracts::NumericLiteral>(value.value));return 0.;};
        check(plotStep()==48000./4096,"Graph obtains Fs/N through its scalar step wire");
        auto configured=*steps.delegateModel<AudioNodeModel>(source)->numeric_node();check(numeric_editor::set_sample_rate(configured,36000) && steps.edit_node(source,configured),"Changing source Fs updates explicit step dependency");
        check(plotStep()==36000./4096,"Source Fs edit updates wired frequency step and graph");
        scene.undoStack().undo();check(plotStep()==48000./4096,"Step propagation Undo restores physical graph axis");
        NumericEditorGraphModel restored(registry);restored.load(steps.save());check(restored.save()==steps.save(),"Graph step connection and parameters reload");
        check(restored.delegateModel<AudioNodeModel>(plot)->port_index(QtNodes::PortType::In,domain::frequency_step_input)!=QtNodes::InvalidPortIndex,"Reload preserves visible graph step input");
    }

void port_labels(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel labels(registry);QtNodes::DataFlowGraphicsScene scene(labels);labels.set_undo_stack(&scene.undoStack());
        const auto id=labels.addNode("Window"),length=labels.addNode("ArrayLength");auto* node=labels.delegateModel<AudioNodeModel>(id);
        const auto before=labels.numeric_graph();std::unique_ptr<QWidget> inspector(node->parameter_widget());
        check(!inspector->findChild<QComboBox*>("input0Dtype"),"Inspector no longer contains contract controls");
        auto* contracts=inspector->findChild<QPushButton*>("editNodeContracts");auto* names=inspector->findChild<QPushButton*>("editPortLabels");check(contracts&&names,"Inspector exposes separate contract and label dialogs");
        bool opened=false;
        QObject dialogCallbacks;
        QTimer::singleShot(0,&dialogCallbacks,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());opened=dialog&&dialog->objectName()=="portContractsDialog";check(opened&&dialog->findChild<QComboBox*>("input0Dtype"),"Actual modal contains the contract editor");if(dialog)dialog->accept();});if(contracts)contracts->click();check(opened,"Contract dialog opens modally");
        QTimer::singleShot(0,&dialogCallbacks,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());auto* edit=dialog?dialog->findChild<QLineEdit*>("portLabel_input65531"):nullptr;check(edit,"Fs label editor uses stable port ID");if(edit)edit->setText("Моя частота");if(dialog)dialog->accept();});if(names)names->click();
        const auto port=node->port_index(QtNodes::PortType::In,domain::input_sample_rate_input);
        check(node->portCaption(QtNodes::PortType::In,port).startsWith("Моя частота\n"),"Custom text is rendered on the port");
        QByteArray a,b;check(desktop::encode_numeric_graph(before,a)&&desktop::encode_numeric_graph(labels.numeric_graph(),b)&&a==b,"Label edit does not change processing graph or contracts");
        scene.undoStack().undo();check(!node->presentation().contains("port_labels"),"Label changes support Undo");scene.undoStack().redo();
        NumericEditorGraphModel restored(registry);restored.load(labels.save());check(restored.delegateModel<AudioNodeModel>(id)->portCaption(QtNodes::PortType::In,port)==node->portCaption(QtNodes::PortType::In,port),"Labels survive project reload");
        check(!labels.delegateModel<AudioNodeModel>(length)->embeddedWidget(),"Array length has no empty fixed-width body");scene.setNodeGeometry(std::make_unique<EditorNodeGeometry>(labels,[]{return QJsonObject{};}));scene.nodeGeometry().recomputeSize(length);
        check(scene.nodeGeometry().size(length).width()<260,"Array length node is compact");
    }

void compact_graph_card(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel compact(registry);const auto plot=compact.addNode("FFTView");
        QtNodes::DataFlowGraphicsScene scene(compact);scene.setNodeGeometry(std::make_unique<EditorNodeGeometry>(compact,[]{return QJsonObject{};}));scene.setNodePainter(std::make_unique<EditorNodePainter>());
        auto& geometry=static_cast<EditorNodeGeometry&>(scene.nodeGeometry());geometry.recomputeSize(plot);
        const auto title=geometry.captionRect(plot).translated(geometry.captionPosition(plot));
        check(!title.intersects(QRectF(12,12,8,8)),"Compact graph title does not overlap activity lamp");
        check(title.bottom()<geometry.portTextPosition(plot,QtNodes::PortType::In,0).y(),"Compact graph title is above input labels");
        auto* item=scene.nodeGraphicsObject(plot);item->updateQWidgetEmbedPos();const auto bounds=item->sceneBoundingRect();
        QImage image(bounds.size().toSize()+QSize(24,24),QImage::Format_ARGB32_Premultiplied);image.fill(QApplication::palette().color(QPalette::Window));QPainter painter(&image);scene.render(&painter,QRectF(12,12,bounds.width(),bounds.height()),bounds);painter.end();
        check(image.save(QDir(QString::fromUtf8(QT_TESTCASE_SOURCEDIR)).absoluteFilePath("../.agent-workspace/graph-step-card.png")),"Own graph card renderer shows the explicit step port");
    }

void musical_row_edits(NumericEditorGraphModel& rows,QtNodes::DataFlowGraphicsScene& scene,QtNodes::NodeId notes,QtNodes::NodeId broad,QtNodes::NodeId broadHistory,QtNodes::NodeId noteHistory,QtNodes::NodeId combinedHistory,QtNodes::NodeId piano,QtNodes::NodeId join,QtNodes::NodeId metrics,const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        auto* note=rows.delegateModel<AudioNodeModel>(notes);auto* bands=rows.delegateModel<AudioNodeModel>(broad);
        auto failureContext=std::make_shared<pipeline::NumericDeliveryContext>();failureContext->endpoint={"main",domain::NodeId(notes),domain::PortDirection::Output,0};failureContext->contract=note->numeric_node()->outputs[0].contract;
        check(note->preview(pipeline::DeliveredNumericFrame{failureContext,{1,1},{},signal_contracts::Status::invalid_argument}) && note->diagnostic_failure(domain::PortDirection::Output,0)==ui::text("Input spectrum must contain finite nonnegative values. Check correction coefficients and array operation."),"Rejected musical spectrum explains input domain and correction at its origin");
        note->clear_preview();
        check(!note->parameter_model()->field("high_count") && !bands->parameter_model()->field("first_midi"),"Broadband and note settings are independent cards");
        const auto width=[&](auto id){return std::get<signal_contracts::NumericContract>(rows.delegateModel<AudioNodeModel>(id)->numeric_node()->inputs[0].contract.view()).shape.extents[0].maximum;};
        const auto bandCount=width(broadHistory);const auto before=rows.save();check(note->parameter_model()->editLiteral("first_midi","60",0),"Actual note control edits an independent row");
        check(width(broadHistory)==bandCount && width(noteHistory)==49 && width(combinedHistory)==bandCount+49,"Note range edits propagate only note geometry and explicit join");
        domain::ResolvedProcessingNode pianoNode;domain::ProcessingParameterView first;
        check(bool(domain::resolve_processing_node(*rows.delegateModel<AudioNodeModel>(piano)->numeric_node(),numeric_editor::limits(),pianoNode)) && domain::resolve_processing_parameter(pianoNode,"first_midi",first)==signal_contracts::Status::ok && std::visit([](auto v){return unsigned(v);},std::get<signal_contracts::NumericLiteral>(first))==60,"Piano key origin follows an explicit ordinary numeric wire");
        scene.undoStack().undo();check(rows.save()==before,"Independent note edit Undo restores the entire wired document");
        NumericEditorGraphModel restored(registry);restored.load(rows.save());
        check(restored.connectionExists({notes,rows.delegateModel<AudioNodeModel>(notes)->port_index(QtNodes::PortType::Out,1),metrics,0}) && restored.connectionExists({notes,rows.delegateModel<AudioNodeModel>(notes)->port_index(QtNodes::PortType::Out,2),metrics,1}),"Both note agreement connections survive document reload");
        check(restored.delegateModel<AudioNodeModel>(join)->numeric_node()->inputs.size()==2 && restored.delegateModel<AudioNodeModel>(broad)->numeric_node()->inputs.back().id==domain::frequency_step_input && restored.delegateModel<AudioNodeModel>(notes)->numeric_node()->inputs.back().id==domain::frequency_step_input,"Independent rows, stable step ports and generic join survive reload");
}
void independent_musical_rows(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel rows(registry);rows.set_admission_limits(numeric_editor::limits());QtNodes::DataFlowGraphicsScene scene(rows);rows.set_undo_stack(&scene.undoStack());
        const auto spectrum=rows.addNode("AmplitudePhase"),broad=rows.addNode("BroadbandRow"),notes=rows.addNode("NoteSpectrum"),join=rows.addNode("ArrayConcat"),broadHistory=rows.addNode("Spectrogram"),noteHistory=rows.addNode("Spectrogram"),combinedHistory=rows.addNode("Spectrogram"),piano=rows.addNode("PianoView");
        const auto source=rows.addNode("AudioSource"),window=rows.addNode("Window"),length=rows.addNode("ArrayLength"),step=rows.addNode("FrequencyStep");
        auto connect=[&](QtNodes::NodeId from,domain::PortId out,QtNodes::NodeId to,domain::PortId in){const QtNodes::ConnectionId edge{from,rows.delegateModel<AudioNodeModel>(from)->port_index(QtNodes::PortType::Out,out),to,rows.delegateModel<AudioNodeModel>(to)->port_index(QtNodes::PortType::In,in)};check(rows.connectionPossible(edge),"Independent row and join ordinary wire is admissible");scene.undoStack().push(new QtNodes::ConnectCommand(&scene,edge));check(rows.connectionExists(edge),"Independent row and join wire is created");};
        connect(spectrum,0,broad,0);connect(spectrum,0,notes,0);connect(broad,0,join,0);connect(notes,0,join,1);connect(broad,0,broadHistory,0);connect(notes,0,noteHistory,0);connect(join,0,combinedHistory,0);
        connect(source,0,window,0);connect(window,0,length,0);connect(length,0,step,domain::window_size_input);connect(source,domain::sample_rate_output,step,0);connect(step,0,broad,domain::frequency_step_input);connect(step,0,notes,domain::frequency_step_input);connect(notes,0,piano,0);connect(notes,domain::note_first_output,piano,domain::note_first_input);
        const auto metrics=rows.addNode("ScalarView");
        check(rows.delegateModel<AudioNodeModel>(metrics)->add_scalar_input(),"Note agreement has two independent metric inputs");
        connect(notes,1,metrics,0);connect(notes,2,metrics,1);
        for(const auto& port:rows.delegateModel<AudioNodeModel>(metrics)->numeric_node()->inputs){
            const auto value=std::get<signal_contracts::NumericContract>(port.contract.view());
            check(value.dtype==signal_contracts::DType::Float32 && value.shape.rank==signal_contracts::Rank::Scalar,"Note agreement keeps its scalar Float32 contract in the metrics view");
        }
        musical_row_edits(rows,scene,notes,broad,broadHistory,noteHistory,combinedHistory,piano,join,metrics,registry);

    }

void activity_telemetry(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel telemetry(registry);const auto id=telemetry.addNode("SignalGenerator");auto* node=telemetry.delegateModel<AudioNodeModel>(id);
        int edits=0,redraws=0;QObject::connect(&telemetry,&NumericEditorGraphModel::nodeUpdated,&telemetry,[&](auto){++edits;});QObject::connect(node,&AudioNodeModel::activity_changed,&telemetry,[&]{++redraws;});
        const auto before=telemetry.save();for(unsigned i=0;i<128;++i)node->set_processing_activity(true,bool(i%2),double(i));
        check(edits==0 && redraws==128 && telemetry.save()==before,"Activity telemetry redraws without persistent node updates or graph serialization triggers");
    }

void incompatible_draft(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry){
        NumericEditorGraphModel draft(registry);QtNodes::DataFlowGraphicsScene scene(draft);auto& history=scene.undoStack();draft.set_undo_stack(&history);
        const auto source=draft.addNode("SignalGenerator"),sink=draft.addNode("WLEDOutput");
        const QtNodes::ConnectionId edge{source,0,sink,0};
        check(draft.connectionPossible(edge),"Structural draft accepts incompatible numeric-to-packet edge");
        history.push(new QtNodes::ConnectCommand(&scene,edge));
        check(draft.connectionExists(edge) && draft.connection_visual_state(edge)==3 && !draft.connection_failure(edge).isEmpty(),"Invalid draft edge has a visible reason");
        domain::NumericProcessingGraph raw;check(draft.export_numeric_graph(raw,nullptr,false),"Saving an incompatible draft does not prepare it");
        NumericEditorGraphModel restored(registry);check(restored.set_numeric_graph(raw) && restored.connectionExists(edge),"Typed draft load retains an incompatible edge");
        domain::NumericProcessingGraph executable;check(!draft.export_numeric_graph(executable),"Execution export remains strict");
        history.undo();check(!draft.connectionExists(edge),"Undo removes the invalid draft connection");history.redo();check(draft.connectionExists(edge),"Redo restores the invalid draft connection");
    }

void native_selection_readout(SpectrogramGraph& view,QQuickItem* gl,QQuickWidget* host,QFile& file,QElapsedTimer& timer){
        view.resize(321,259);view.quick_action("selection",1);
        timer.restart();while(timer.elapsed()<300){QCoreApplication::processEvents();QThread::msleep(2);}
        for(int y=20;y<gl->height() && view.axis_layout_telemetry().value("selectionReadoutRows").toStringList().isEmpty();y+=16)
            for(int x=20;x<gl->width() && view.axis_layout_telemetry().value("selectionReadoutRows").toStringList().isEmpty();x+=16){
                view.quick_pointer(0,x,y,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
                view.quick_pointer(2,x,y,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
                QCoreApplication::processEvents();
            }
        timer.restart();while(view.axis_layout_telemetry().value("selectionReadoutRows").toStringList().isEmpty() && timer.elapsed()<2000){QCoreApplication::processEvents();QThread::msleep(2);}
        const auto narrow=view.axis_layout_telemetry();const auto readout=narrow.value("selectionReadoutRect").toRect();
        check(!narrow.value("selectionReadoutRows").toStringList().isEmpty(),"small native surface has a selected sample");
        check(!readout.isEmpty() && QRect(0,0,qRound(gl->width()),qRound(gl->height())).contains(readout),"small native selection readout fits the actual viewport");
        const auto smallImage=host->grabFramebuffer();check(!smallImage.isNull(),"small native framebuffer remains rendered after resize");
        if(!smallImage.isNull())smallImage.save(QFileInfo(file).absolutePath()+"/native-history-small-frame.png");

}
std::vector<std::vector<float>> native_history_rows(QFile& file){
        std::vector<std::vector<float>> history;QTextStream input(&file);
        while(!input.atEnd()){const auto fields=input.readLine().split(',');if(fields.size()<52 || fields[0]!="7")continue;
            history.clear();for(int i=2;i+49<fields.size();i+=50){std::vector<float> row;for(int b=0;b<50;++b)row.push_back(fields[i+b].toFloat());history.push_back(std::move(row));}}
        return history;
}
int native_history_path(const char* path){
        QFile file(QString::fromLocal8Bit(path));check(file.open(QIODevice::ReadOnly),"native path data file");
        const auto history=native_history_rows(file);
        check(!history.empty(),"native runtime History matrix");float peak=0;size_t peakAge=0,peakBand=0;
        for(size_t r=0;r<history.size();++r)for(size_t b=0;b<50;++b)if(history[r][b]>peak){peak=history[r][b];peakAge=history.size()-1-r;peakBand=b;}
        SpectrogramGraph view;view.resize(900,600);check(view.initialize({240,180},{1200,900}),"native renderer initialization");
        view.set_history_dimensions(400,50);view.update_frame_history(history);view.set_y_range(0,peak);
        view.setAttribute(Qt::WA_ShowWithoutActivating);view.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
        auto* host=view.findChild<QQuickWidget*>("spectrogramQuickView");
        auto* gl=host && host->rootObject()?host->rootObject()->findChild<SurfaceQuickItem*>():nullptr;check(gl,"native Quick renderer container");
        gl->backend()->setProperty("verifyGeometryUpload",true);view.show();gl->update();
        QElapsedTimer timer;timer.start();while(!view.axis_layout_telemetry().value("geometryUploadVerified").toBool() && timer.elapsed()<5000){QCoreApplication::processEvents();QThread::msleep(2);}
        const auto telemetry=view.axis_layout_telemetry();const auto image=host->grabFramebuffer();
        if(!image.isNull())image.save(QFileInfo(file).absolutePath()+"/native-history-frame.png");
        check(!image.isNull() && host->status()==QQuickWidget::Ready && telemetry.value("rendererReady").toBool(),"native Quick FBO rendering");
        check(telemetry.value("geometryUploadVerified").toBool() && telemetry.value("vertexCount").toInt()==49*399*6,"actual GPU VBO exact readback/mesh topology");
        check(telemetry.value("surfacePeakAge").toULongLong()==peakAge && telemetry.value("surfacePeakBand").toULongLong()==peakBand,"GPU mesh retains History band/age peak");
        std::printf("native Quick runtime History rows=%zu bands=50 peak=%.9g age=%zu band=%zu GPU vertices=%d exact VBO PASS\n",history.size(),peak,peakAge,peakBand,telemetry.value("vertexCount").toInt());
        native_selection_readout(view,gl,host,file,timer);
        return failures?1:0;
    }

void product_connection_order(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry,bool coefficientsFirst){
        NumericEditorGraphModel factors(registry);factors.set_admission_limits(numeric_editor::limits());QtNodes::DataFlowGraphicsScene scene(factors);
        factors.set_undo_stack(&scene.undoStack());
        const auto amplitude=factors.addNode("AmplitudePhase"),length=factors.addNode("ArrayLength"),bank=factors.addNode("ArrayFill"),product=factors.addNode("ArrayMultiply");
        auto connect=[&](QtNodes::ConnectionId edge){
            check(factors.connectionPossible(edge),"Ordinary factor wire is admissible in either order");
            scene.undoStack().push(new QtNodes::ConnectCommand(&scene,edge));
            check(factors.connectionExists(edge),"Ordinary factor wire is created by the editor command");
        };
        connect({amplitude,0,length,0});connect({length,0,bank,0});
        const QtNodes::ConnectionId data{amplitude,0,product,1},coefficient{bank,0,product,0};
        connect(coefficientsFirst?coefficient:data);connect(coefficientsFirst?data:coefficient);
        auto* model=factors.delegateModel<AudioNodeModel>(product);auto node=*model->numeric_node();
        std::get<domain::TransformProcessingParameters>(node.parameters).values.push_back({"frame_input",signal_contracts::NumericLiteral{std::uint64_t{1}}});
        check(factors.edit_node(product,node),"Product explicitly selects amplitude input as frame context");
        node=*model->numeric_node();
        auto compatible=node;
        const auto fixedFactor=numeric_editor::default_node("ArrayFill").outputs[0].contract;
        compatible.inputs[0].contract=fixedFactor;compatible.inputs[1].contract=fixedFactor;
        domain::OwnedProcessingContract output;std::size_t driver=0;
        check(domain::array_product_output_contract(compatible,output,&driver)==signal_contracts::Status::ok && driver==1,
              "Explicit frame input selects the second equal-size factor in either connection order");
        const auto before=factors.save();
        std::unique_ptr<QWidget> factorForm(model->parameter_widget());
        auto* factorCount=factorForm->findChild<QSpinBox*>("factorCount");
        check(factorCount,"Product exposes factor count in its actual editor form");
        if(factorCount)factorCount->setValue(4);
        check(model->numeric_node()->inputs.size()==4 && factors.connectionExists(data) && factors.connectionExists(coefficient),"Adding factors retains existing wires");
        scene.undoStack().undo();check(factors.save()==before,"Factor count Undo restores the full document");scene.undoStack().redo();
        NumericEditorGraphModel restored(registry);restored.load(factors.save());
        check(restored.delegateModel<AudioNodeModel>(product)->numeric_node()->inputs.size()==4 && restored.connectionExists(data) && restored.connectionExists(coefficient),"Four factors and ordinary connections survive document reload");
    }

void scalar_route_order(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry,bool sourceFirst){
        NumericEditorGraphModel routes(registry);QtNodes::DataFlowGraphicsScene scene(routes);routes.set_undo_stack(&scene.undoStack());
        const auto fs=routes.addNode("Fs"),route=routes.addNode("Reroute"),tail=routes.addNode("Reroute"),delta=routes.addNode("FrequencyStep"),amplitude=routes.addNode("AmplitudePhase");
        auto edge=[&](QtNodes::NodeId from,domain::PortId out,QtNodes::NodeId to,domain::PortId in){return QtNodes::ConnectionId{from,routes.delegateModel<AudioNodeModel>(from)->port_index(QtNodes::PortType::Out,out),to,routes.delegateModel<AudioNodeModel>(to)->port_index(QtNodes::PortType::In,in)};};
        auto connect=[&](QtNodes::ConnectionId wire){check(routes.connectionPossible(wire),"Typed scalar routing wire accepted independently of source order");scene.undoStack().push(new QtNodes::ConnectCommand(&scene,wire));check(routes.connectionExists(wire),"Scalar routing command retained its wire");};
        const auto input=edge(fs,0,route,0);connect(input);connect(edge(route,0,tail,0));
        if(!sourceFirst)check(routes.deleteConnection(input),"Detach scalar source while retaining typed routing points");
        connect(edge(tail,0,delta,0));connect(edge(tail,0,amplitude,domain::target_sample_rate_input));
        domain::NumericProcessingGraph exported;
        if(!sourceFirst){check(!routes.export_numeric_graph(exported),"Unresolved scalar draft does not execute with a fabricated parameter");connect(input);}
        check(routes.export_numeric_graph(exported),"Completed scalar routing chain exports for execution");
        NumericEditorGraphModel restored(registry);check(restored.set_numeric_graph(exported),"Scalar parameter routing reloads");
        check(restored.connectionExists(edge(tail,0,delta,0)) && restored.connectionExists(edge(tail,0,amplitude,domain::target_sample_rate_input)),"Both scalar fan-out wires survive reload");
    }

struct EditorDocumentScenario {
 const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry;
 NumericEditorGraphModel graph;QUndoStack undo;
 QtNodes::NodeId source{},route{},sink{},other{},tail{},converter{};AudioNodeModel* model=nullptr;
 std::unique_ptr<QWidget> form;QComboBox* type=nullptr;QLineEdit* text=nullptr;QWidget* body=nullptr;QLineEdit* inlineText=nullptr;QAction* show=nullptr;
 QByteArray processingBefore,bytes;domain::NumericProcessingGraph restored;
 std::optional<NumericEditorGraphModel> restoredGraphStorage;std::unique_ptr<QWidget> historyForm;
 explicit EditorDocumentScenario(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& value):registry(value),graph(value){}
 auto value()const{return std::get<domain::ConverterProcessingParameters>(model->numeric_node()->parameters).policy.nan_replacement;}
int initialize(){
    graph.set_admission_limits(numeric_editor::limits());graph.set_undo_stack(&undo);

    for(const auto kinds=numeric_editor::catalog();const auto& kind:kinds){

        auto node=numeric_editor::default_node(kind);domain::ResolvedProcessingNode resolved;

        check(bool(domain::resolve_processing_node(node,numeric_editor::limits(),resolved)),qPrintable(kind));

    }

    delivery_diagnostics(graph,undo);

    if(const auto result=selector_topology(registry,graph,undo);result)return result;

    window_function_edit(graph,undo);


return 0;
}
void routing(){
    std::fprintf(stderr,"editor fixture phase routing\n");
    source=graph.addNode("NetworkInput");route=graph.addNode("Switch");sink=graph.addNode("NetworkOutput");other=graph.addNode("NetworkOutput");

    check(graph.connectionPossible({source,0,route,0}),"compatible typed source route");graph.addConnection({source,0,route,0});

    graph.addConnection({route,0,sink,0});graph.addConnection({route,1,other,0});

    network_adapter_undo(graph,undo,source);

    auto n=*graph.delegateModel<AudioNodeModel>(route)->numeric_node();numeric_editor::set_output_count(n,4);graph.edit_node(route,n);

    check(graph.connectionExists({route,0,sink,0}) && graph.connectionExists({route,1,other,0}),"grow preserves existing edges");

    tail=graph.addNode("NetworkOutput");graph.addConnection({route,3,tail,0});

    n=*graph.delegateModel<AudioNodeModel>(route)->numeric_node();numeric_editor::set_output_count(n,2);

    const int undoBeforeShrink=undo.index();
    check(graph.edit_node(route,n) && undo.index()==undoBeforeShrink+1 && !graph.connectionExists({route,3,tail,0}),"Connected endpoint shrink removes only its incident edge atomically");
    check(graph.connectionExists({route,0,sink,0}) && graph.connectionExists({route,1,other,0}),"Connected shrink preserves stable surviving endpoints");
    undo.undo();check(graph.delegateModel<AudioNodeModel>(route)->numeric_node()->outputs.size()==4 && graph.connectionExists({route,3,tail,0}),"Shrink Undo restores removed port and exact edge");
    undo.redo();check(!graph.connectionExists({route,3,tail,0}) && graph.connectionExists({route,0,sink,0}),"Shrink Redo removes only the tail edge");

    routing_inline_count(graph,undo,route);

    auto mismatch=*graph.delegateModel<AudioNodeModel>(tail)->numeric_node();numeric_editor::set_dtype(mismatch,domain::PortDirection::Input,0,signal_contracts::DType::UInt64);graph.edit_node(tail,mismatch);

    check(graph.connectionPossible({source,0,tail,0}),"dtype-only link supports prepared conversion");graph.addConnection({source,0,tail,0});check(graph.connectionExists({source,0,tail,0}),"raw add admits dtype-only conversion");graph.deleteConnection({source,0,tail,0});

}
void exact_fields(){
    std::fprintf(stderr,"editor fixture phase typed-converter\n");
    converter=graph.addNode("Converter");model=graph.delegateModel<AudioNodeModel>(converter);

    std::fprintf(stderr,"editor converter before form\n");
    form.reset(model->parameter_widget());
    std::fprintf(stderr,"editor converter after form\n");

    type=form->findChild<QComboBox*>("numericNan_type");text=form->findChild<QLineEdit*>("numericNan");

    check(type && text,"exact typed field exists");

    std::fprintf(stderr,"editor converter before type\n");
    if(type && text){type->setCurrentIndex(1);std::fprintf(stderr,"editor converter after type\n");text=form->findChild<QLineEdit*>("numericNan");text->setText("18446744073709551615");text->setModified(true);QMetaObject::invokeMethod(text,"editingFinished",Qt::DirectConnection);}

    std::fprintf(stderr,"editor converter after edit\n");


    check(std::holds_alternative<std::uint64_t>(value()) && std::get<std::uint64_t>(value())==std::numeric_limits<std::uint64_t>::max(),"UI UInt64 exact");

    std::fprintf(stderr,"editor converter before undo\n");
    undo.undo();check(std::get<std::uint64_t>(value())==0,"undo exact field");undo.redo();

    auto* signedText=form->findChild<QLineEdit*>("numericNegativeInf");

    if(signedText){signedText->setText("-9223372036854775808");signedText->setModified(true);QMetaObject::invokeMethod(signedText,"editingFinished",Qt::DirectConnection);}

    check(std::get<std::int64_t>(std::get<domain::ConverterProcessingParameters>(model->numeric_node()->parameters).policy.negative_inf_replacement)==std::numeric_limits<std::int64_t>::min(),"UI Int64 minimum exact");

    auto* exponent=form->findChild<QSpinBox*>("numericGainExponent");check(exponent,"dyadic gain exponent is editable");

    if(exponent)exponent->setValue(-17);

    check(std::get<domain::ConverterProcessingParameters>(model->numeric_node()->parameters).policy.scaling.gain.exponent2==-17,"dyadic exponent edit preserves signed value");

}
void inline_fields(){
    std::fprintf(stderr,"editor converter before inline\n");
    body=model->embeddedWidget();inlineText=body->findChild<QLineEdit*>("numericNan");

    show=form->findChild<QAction*>("showOnBlock_numericNan");

    check(show && inlineText && !show->isChecked() && !inlineText->isVisibleTo(body),"compact default hides editable parameters");

    desktop::encode_numeric_graph(graph.numeric_graph(),processingBefore);

    if(show)show->setChecked(true);

    check(inlineText && inlineText->isVisibleTo(body) && inlineText->text()=="18446744073709551615","selected inline parameter displays exact inspector value");

    QByteArray processingAfter;desktop::encode_numeric_graph(graph.numeric_graph(),processingAfter);

    check(processingBefore==processingAfter,"presentation flags leak into processing serialization");

    undo.undo();check(show && !show->isChecked() && !inlineText->isVisibleTo(body),"visibility undo synchronizes inspector and block");

    undo.redo();check(show && show->isChecked() && inlineText->isVisibleTo(body),"visibility redo restores block field");

    if(inlineText){inlineText->setText("9007199254740993");inlineText->setModified(true);QMetaObject::invokeMethod(inlineText,"editingFinished",Qt::DirectConnection);}

    check(text && text->text()=="9007199254740993" && std::get<std::uint64_t>(value())==9007199254740993ULL,"inline exact edit synchronizes inspector");

    undo.undo();check(inlineText->text()=="18446744073709551615" && text->text()==inlineText->text(),"parameter undo synchronizes both forms");

    if(show)show->setChecked(false);

    check(!inlineText->isVisibleTo(body) && std::get<std::uint64_t>(value())==std::numeric_limits<std::uint64_t>::max(),"hiding inline preserves parameter value");

    if(show)show->setChecked(true);

}
void presentation(){
    const auto beforeNameUndo=undo.count();
    std::fprintf(stderr,"editor converter before custom presentation node=%u exists=%d\n",unsigned(converter),int(graph.nodeExists(converter)));
    auto display=model->presentation();display["future_display_field"]="kept";model->set_presentation(display);
    check(model->edit_display_name(QString::fromUtf8("Мой преобразователь")) && model->caption()==QString::fromUtf8("Мой преобразователь") && model->name()=="Converter","Custom caption preserves stable algorithm kind");
    QByteArray namedProcessing;desktop::encode_numeric_graph(graph.numeric_graph(),namedProcessing);
    check(namedProcessing==processingBefore && undo.count()==beforeNameUndo+1,"Node rename is one presentation-only undo command");
    const auto namedPresentation=model->presentation();
    check(!model->edit_display_name(QString(257,'x')) && !model->edit_display_name(QString("bad")+QChar(1)) && model->presentation()==namedPresentation,"Invalid display names refuse without mutation");
    auto malformedName=model->save();auto invalidDisplay=namedPresentation;invalidDisplay["display_name"]=17;malformedName["presentation"]=invalidDisplay;
    AudioNodeModel malformedCaption("Converter");malformedCaption.load(malformedName);
    check(!malformedCaption.numeric_node(),"Malformed persisted display name must reject the node rather than silently discard metadata");
    undo.undo();check(model->caption()!=QString::fromUtf8("Мой преобразователь") && model->presentation().value("future_display_field")=="kept","Rename undo preserves unknown presentation fields");
    undo.redo();check(model->caption()==QString::fromUtf8("Мой преобразователь"),"Rename redo restores user caption");

    const auto saved=graph.saveNode(converter);auto copied=saved;copied["id"]=100;graph.loadNode(copied);

    auto* copy=graph.delegateModel<AudioNodeModel>(100);

    check(copy && copy->numeric_node() && copy->numeric_node()->id==100 && domain::same_processing_parameters(model->numeric_node()->parameters,copy->numeric_node()->parameters),"clipboard remaps id preserves exact literals");

    check(copy && copy->presentation()==model->presentation() && copy->parameter_on_block("numericNan"),"node snapshot preserves presentation flags");

    converter_clipboard(graph,converter,model);

}
int restore_snapshot(){
    check(bool(desktop::encode_numeric_graph(graph.numeric_graph(),bytes)),"known snapshot encode");check(bool(desktop::decode_numeric_graph(bytes,restored)),"known snapshot decode");

    if(restored.nodes.empty()){check(false,"Known snapshot unexpectedly contains no restored nodes");return 1;}

    restoredGraphStorage.emplace(registry);auto& restoredGraph=*restoredGraphStorage;restoredGraph.set_numeric_graph(restored);check(restoredGraph.connectionExists({route,0,sink,0}),"typed graph reload keeps ports");

    restored_graph_validation(registry,restoredGraph,restored,sink);

    if(const auto result=source_backed_project(restoredGraph,bytes,converter,model);result)return result;


return 0;
}
void history_capacity(){
    auto history=graph.addNode("Spectrogram");auto* accumulator=graph.delegateModel<AudioNodeModel>(history);historyForm.reset(accumulator->parameter_widget());auto* capacity=historyForm->findChild<QLineEdit*>("numericCapacity");
    const auto historyContract=std::get<signal_contracts::NumericContract>(accumulator->numeric_node()->outputs[0].contract.view());
    check(historyContract.coordinates[0].mode==signal_contracts::CoordinateMode::ExplicitFloat64LE && historyContract.coordinates[0].unit==signal_contracts::NumericUnit::Seconds,"new History preserves individual source-clock seconds");

    if(capacity){capacity->setText("17");capacity->setModified(true);QMetaObject::invokeMethod(capacity,"editingFinished",Qt::DirectConnection);}

    auto h=*accumulator->numeric_node();domain::ResolvedProcessingNode hr;

    check(bool(domain::resolve_processing_node(h,numeric_editor::limits(),hr)) && std::get<signal_contracts::NumericContract>(h.outputs[0].contract.view()).shape.extents[0].maximum==17,"history capacity edits both configured parameter and contract");

}
void incompatible_contracts(){
    auto bad=graph.saveNode(source);bad["id"]=101;auto data=bad.value("internal-data").toObject();data["numeric_processing"]="{invalid";bad["internal-data"]=data;graph.loadNode(bad);

    check(!graph.connectionPossible({101,0,sink,0}),"malformed typed snapshot never defaults to connectable node");

    graph.loadNode({{"id",102},{"internal-data",QJsonObject{{"model-name","NetworkOutput"}}}});

    check(!graph.connectionPossible({101,0,102,0}),"malformed typed snapshot never falls back to legacy ports");

    const QtNodes::ConnectionId incompatibleUnit{source,0,tail,0};
    check(graph.connectionPossible(incompatibleUnit),"Units mismatch keeps a structurally valid draft endpoint");
    graph.addConnection(incompatibleUnit);
    auto changed=*graph.delegateModel<AudioNodeModel>(tail)->numeric_node();
    auto contract=changed.inputs[0].contract.view();std::get<signal_contracts::NumericContract>(contract).unit=signal_contracts::NumericUnit::RawAmplitude;
    check(numeric_editor::set_contract(changed,domain::PortDirection::Input,0,contract) &&
          numeric_editor::set_dtype(changed,domain::PortDirection::Input,0,signal_contracts::DType::Float32) && graph.edit_node(tail,changed),
          "Connected draft accepts an explicit incompatible physical unit edit");
    check(graph.connectionExists(incompatibleUnit) && graph.connection_visual_state(incompatibleUnit)==3 && !graph.connection_failure(incompatibleUnit).isEmpty(),"Units mismatch is retained with a visible typed incompatibility");
    graph.deleteConnection(incompatibleUnit);

}
int physical_endpoints(){
    if(const auto result=physical_endpoint_controls(graph,undo);result)return result;


return 0;
}
int run(){
 if(const auto result=initialize();result)return result;
 routing();exact_fields();inline_fields();presentation();
 if(const auto result=restore_snapshot();result)return result;
 history_capacity();incompatible_contracts();return physical_endpoints();
}
};

static int connection_diagnostics_only(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry) {
    NumericEditorGraphModel graph(registry);QUndoStack undo;graph.set_undo_stack(&undo);
    delivery_diagnostics(graph,undo);
    std::printf("connection diagnostics failures=%d\n",failures);return failures?1:0;
}
static void native_clock_editor_cases(const std::shared_ptr<QtNodes::NodeDelegateModelRegistry>& registry) {
    native_source_contract(registry);source_rate_undo(registry);clock_rate_dependency(registry);resampler_editor(registry);
}
int main(int argc,char** argv){

    QApplication app(argc,argv);
#ifdef Q_OS_WIN
    const auto fontFile=QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf");
    check(QFontDatabase::addApplicationFont(fontFile)>=0,"Own offscreen scene uses the system font with Cyrillic glyphs");
    app.setFont(QFont("Segoe UI",9));
#endif
    if(argc==3 && QString::fromLocal8Bit(argv[1])=="--native-path")return native_history_path(argv[2]);


    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();

    for(const auto kinds=numeric_editor::catalog();const auto& kind:kinds)registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    if(argc==2 && QString::fromLocal8Bit(argv[1])=="--connection-diagnostics")return connection_diagnostics_only(registry);
    coefficient_port_identity(app);
    axis_range_controls();
    sparse_port_publication(registry);

    source_rate_dependencies(registry);
    capture_endpoint_guard(registry);
    fourier_port_roundtrip(registry);
    routing_point_roundtrip(registry);
    numeric_primitive_roundtrip(registry);
    parameter_wire_geometry(registry);
    surviving_widget_localization();

    temporal_matrix_chain(registry);

    explicit_clock_bindings(registry);

    configuration_wire_undo(registry);

    native_format_admission(registry);

    inline_field_layout(registry);

    view_contract_adoption(registry);

    preview_extraction(registry);

    native_clock_editor_cases(registry);

    EditorDocumentScenario document(registry);
    if(const auto result=document.run();result)return result;

    for(const bool coefficientsFirst:{false,true})product_connection_order(registry,coefficientsFirst);

    frequency_step_wires(registry);

    for(const bool sourceFirst:{false,true})scalar_route_order(registry,sourceFirst);
    port_labels(registry);
    compact_graph_card(registry);
    independent_musical_rows(registry);

    activity_telemetry(registry);

    incompatible_draft(registry);
    document.graph.set_admission_limits(numeric_editor::limits());document.graph.set_capture_device_provider({});

    std::printf("numeric editor failures=%d\n",failures);return failures?1:0;

}

